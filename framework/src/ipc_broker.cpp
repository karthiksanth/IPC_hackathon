#include "ipc_utils.h"
#include "ipc_protocol.h"
#include <sys/mman.h>
#include <sys/stat.h> // For chmod()
#include <linux/memfd.h>
#include <linux/limits.h> // For PATH_MAX
#include <fcntl.h>
#include <grp.h>      // For getgrnam()
#include <unistd.h>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <thread>
#include <cstdlib>

struct SharedGroup {
    UniqueFd mem_fd;
    size_t size;
    uint64_t type_hash;
    size_t struct_size;
};

// Constant-Time String Comparison (Prevents Timing Attacks against the token)
bool secure_compare(const char* a, const char* b, size_t len) {
    volatile char result = 0;
    for (size_t i = 0; i < len; ++i) {
        result |= (a[i] ^ b[i]);
    }
    return result == 0;
}

class BrokerRegistry {
public:
    Result<int> get_or_create_group(const RegistrationRequest& req) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string group_name(req.group_name);
        auto it = groups_.find(group_name);
        
        if (it != groups_.end()) {
            if (it->second.type_hash != req.type_hash) {
                return std::unexpected("REJECTED: Struct layout mismatch! Check type_hash.");
            }
            if (it->second.struct_size != req.struct_size) {
                return std::unexpected("REJECTED: Struct size mismatch!");
            }
            return it->second.mem_fd.get();
        }

        // Create new sealed memfd
        int fd = ::memfd_create(group_name.c_str(), MFD_ALLOW_SEALING | MFD_CLOEXEC);
        if (fd < 0) return std::unexpected("memfd_create failed");
        
        if (::ftruncate(fd, req.memory_size) == -1) {
            ::close(fd);
            return std::unexpected("ftruncate failed");
        }

        ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK);

        groups_.emplace(group_name, SharedGroup{ 
            UniqueFd{fd}, req.memory_size, req.type_hash, req.struct_size 
        });
        
        return groups_[group_name].mem_fd.get();
    }
private:
    std::mutex mutex_;
    std::unordered_map<std::string, SharedGroup> groups_;
};

class SecureIpcBroker {
public:
    SecureIpcBroker() {
        // Resolve the GID dynamically on boot
        struct group* grp = ::getgrnam("ipc_group");
        if (!grp) {
            std::cerr << "FATAL: 'ipc_group' does not exist on OS. Run setup script.\n";
            exit(1);
        }
        allowed_gid_ = grp->gr_gid;
        trusted_directory_ = "/opt/ipc_framework/bin/";
    }

    Result<void> run() {
        const char* env_token = std::getenv("IPC_SECRET_TOKEN");
        if (!env_token) return std::unexpected("FATAL: Missing IPC_SECRET_TOKEN");
        expected_token_ = env_token;

        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return std::unexpected(std::strerror(errno));
        server_socket_ = UniqueFd{sock};

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, BROKER_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        ::unlink(BROKER_SOCKET_PATH);

        if (::bind(server_socket_.get(), (struct sockaddr*)&addr, sizeof(addr)) < 0)
            return std::unexpected(std::strerror(errno));

        // --- SECURITY: LOCK DOWN SOCKET FILE PERMISSIONS ---
        // 0660: User (root) can Read/Write, Group (ipc_group) can Read/Write. Others DENIED.
        if (::chmod(BROKER_SOCKET_PATH, 0660) < 0) {
            return std::unexpected(std::strerror(errno));
        }

        if (::listen(server_socket_.get(), 10) < 0)
            return std::unexpected(std::strerror(errno));

        std::cout << "[+] Broker running securely. Socket restricted to GID: " << allowed_gid_ << "\n";

        while (true) {
            int client = ::accept(server_socket_.get(), nullptr, nullptr);
            if (client < 0) continue;

            std::jthread([this, client_fd = UniqueFd{client}]() mutable {
                handle_client(std::move(client_fd));
            }).detach();
        }
    }

private:
    BrokerRegistry registry_;
    std::string expected_token_;
    UniqueFd server_socket_;
    gid_t allowed_gid_;
    std::string trusted_directory_;

    void handle_client(UniqueFd client_fd) {
        // --- DEFENSE 1: ANTI-STALL TIMEOUT ---
        struct timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(client_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // --- DEFENSE 2: EXTRACT SO_PEERCRED ---
        struct ucred cred{};
        socklen_t len = sizeof(cred);
        if (::getsockopt(client_fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
            std::cerr << "[SECURITY REJECT] Failed to extract Kernel Credentials.\n";
            return;
        }

        // --- DEFENSE 3: ENFORCE KERNEL GROUP ID (GID) ---
        if (cred.gid != allowed_gid_ && cred.uid != 0) {
            std::cerr << "[SECURITY REJECT] PID " << cred.pid << " is not in 'ipc_group'.\n";
            return;
        }

        // --- DEFENSE 4: ENFORCE DIRECTORY ENCLAVE ---
        char proc_path[PATH_MAX];
        std::snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", cred.pid);
        
        char exe_path[PATH_MAX];
        ssize_t bytes_read = ::readlink(proc_path, exe_path, sizeof(exe_path) - 1);
        if (bytes_read <= 0) return;
        
        exe_path[bytes_read] = '\0';
        std::string client_binary(exe_path);

        if (!client_binary.starts_with(trusted_directory_)) {
            std::cerr << "[SECURITY REJECT] Binary outside enclave -> " << client_binary << "\n";
            return;
        }

        // --- DEFENSE 5: CONSTANT-TIME TOKEN VERIFICATION ---
        RegistrationRequest req{};
        if (::recv(client_fd.get(), &req, sizeof(req), MSG_WAITALL) != sizeof(req)) return;

        // Ensure token is null-terminated for safety before comparison
        req.secret_token[sizeof(req.secret_token) - 1] = '\0'; 
        
        if (!secure_compare(req.secret_token, expected_token_.c_str(), 32)) {
            std::cerr << "[SECURITY REJECT] Invalid Token from PID " << cred.pid << "\n";
            return;
        }

        // --- SUCCESS: Map memory and send Zero-Copy FD ---
        auto fd_res = registry_.get_or_create_group(req);
        if (fd_res) {
            UdsIpc::send_fd(client_fd.get(), fd_res.value());
            std::cout << "[+] Verified & Registered PID " << cred.pid 
                      << " (" << client_binary << ") to '" << req.group_name << "'\n";
        }
    }
};

int main() {
    SecureIpcBroker broker;
    auto res = broker.run();
    if (!res) {
        std::cerr << res.error() << "\n";
        return 1;
    }
    return 0;
}