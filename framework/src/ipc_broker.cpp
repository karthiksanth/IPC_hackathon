#include "ipc_utils.h"
#include "ipc_protocol.h"
#include <sys/mman.h>
#include <linux/memfd.h>
#include <fcntl.h>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <thread>
#include <cstdlib>

struct SharedGroup {
    UniqueFd mem_fd;
    size_t size;
};

class BrokerRegistry {
public:
    Result<int> get_or_create_group(const RegistrationRequest& req) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string group_name(req.group_name);

        auto it = groups_.find(group_name);
        if (it != groups_.end()) {
            if (req.memory_size != it->second.size) {
                return std::unexpected("Group size mismatch");
            }
            return it->second.mem_fd.get();
        }

        int fd = ::memfd_create(group_name.c_str(), MFD_ALLOW_SEALING | MFD_CLOEXEC);
        if (fd < 0) return std::unexpected("memfd_create failed");
        
        if (::ftruncate(fd, req.memory_size) == -1) {
            ::close(fd);
            return std::unexpected("ftruncate failed");
        }

        ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK);

        groups_.emplace(group_name, SharedGroup{ UniqueFd{fd}, req.memory_size });
        return groups_[group_name].mem_fd.get();
    }
private:
    std::mutex mutex_;
    std::unordered_map<std::string, SharedGroup> groups_;
};

class SecureIpcBroker {
public:
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

        if (::listen(server_socket_.get(), 10) < 0)
            return std::unexpected(std::strerror(errno));

        std::cout << "Broker running securely on " << BROKER_SOCKET_PATH << "\n";

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

    void handle_client(UniqueFd client_fd) {
        struct ucred cred{};
        socklen_t len = sizeof(cred);
        if (::getsockopt(client_fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) return;

        RegistrationRequest req{};
        struct timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(client_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (::recv(client_fd.get(), &req, sizeof(req), MSG_WAITALL) != sizeof(req)) return;

        if (std::strncmp(req.secret_token, expected_token_.c_str(), 32) != 0) {
            std::cerr << "Auth failed for PID " << cred.pid << "\n";
            return;
        }

        auto fd_res = registry_.get_or_create_group(req);
        if (fd_res) {
            UdsIpc::send_fd(client_fd.get(), fd_res.value());
            std::cout << "Registered PID " << cred.pid << " to '" << req.group_name << "'\n";
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