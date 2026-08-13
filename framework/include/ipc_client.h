#pragma once
#include "ipc_utils.h"
#include "ipc_protocol.h"
#include "ipc_ringbuffer.h"          // For Fixed types
#include "ipc_variable_ringbuffer.h" // For Variable types
#include <sys/mman.h>
#include <span>

class IpcClient {
public:
    // ==========================================================
    // 1. FIXED-SIZE API (For standard structs)
    // ==========================================================
    template <typename T>
    static Result<IpcClient> create_fixed_producer(const std::string& group, uint64_t slot_capacity) {
        size_t memory_needed = RingBufferHeader<T>::calculate_size(slot_capacity);
        return register_app<T>(group, memory_needed, ClientRole::PRODUCER);
    }

    template <typename T>
    static Result<IpcClient> create_fixed_listener(const std::string& group, uint64_t slot_capacity) {
        size_t memory_needed = RingBufferHeader<T>::calculate_size(slot_capacity);
        return register_app<T>(group, memory_needed, ClientRole::LISTENER);
    }

    // ==========================================================
    // 2. VARIABLE-SIZE API (For Metadata + Dynamic Heap)
    // ==========================================================
    template <typename Meta>
    static Result<IpcClient> create_variable_producer(const std::string& group, uint64_t slot_capacity, uint64_t heap_capacity) {
        size_t memory_needed = VariableMemoryHeader::calculate_size<Meta>(slot_capacity, heap_capacity);
        return register_app<Meta>(group, memory_needed, ClientRole::PRODUCER);
    }

    template <typename Meta>
    static Result<IpcClient> create_variable_listener(const std::string& group, uint64_t slot_capacity, uint64_t heap_capacity) {
        size_t memory_needed = VariableMemoryHeader::calculate_size<Meta>(slot_capacity, heap_capacity);
        return register_app<Meta>(group, memory_needed, ClientRole::LISTENER);
    }

    // Memory access and RAII cleanup...
    std::span<std::byte> memory() { return std::span<std::byte>(static_cast<std::byte*>(mapped_ptr_), size_); }

    ~IpcClient() {
        if (mapped_ptr_ != MAP_FAILED) ::munmap(mapped_ptr_, size_);
    }

    // Move Semantics
    IpcClient(IpcClient&& other) noexcept 
        : mapped_ptr_(std::exchange(other.mapped_ptr_, MAP_FAILED)),
          size_(std::exchange(other.size_, 0)),
          shm_fd_(std::move(other.shm_fd_)) {}
          
    IpcClient& operator=(IpcClient&& other) noexcept {
        if (this != &other) {
            if (mapped_ptr_ != MAP_FAILED) ::munmap(mapped_ptr_, size_);
            mapped_ptr_ = std::exchange(other.mapped_ptr_, MAP_FAILED);
            size_ = std::exchange(other.size_, 0);
            shm_fd_ = std::move(other.shm_fd_);
        }
        return *this;
    }

private:
    void* mapped_ptr_{MAP_FAILED};
    size_t size_{0};
    UniqueFd shm_fd_;
    IpcClient() = default;

    template <typename T>
    static Result<IpcClient> register_app(const std::string& group, size_t size, ClientRole role) {
        const char* env_token = std::getenv("IPC_SECRET_TOKEN");
        if (!env_token) return std::unexpected("Missing IPC_SECRET_TOKEN in environment");

        RegistrationRequest req{};
        std::strncpy(req.secret_token, env_token, sizeof(req.secret_token));
        std::strncpy(req.group_name, group.c_str(), sizeof(req.group_name) - 1);
        req.role = role;
        req.memory_size = size;
        
        // AUTOMATICALLY INJECT COMPILE-TIME FINGERPRINT
        req.type_hash = get_type_fingerprint<T>();
        req.struct_size = sizeof(T);
        req.struct_align = alignof(T);
        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        UniqueFd client_sock{sock};
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, BROKER_SOCKET_PATH, sizeof(addr.sun_path)-1);

        if (::connect(client_sock.get(), (struct sockaddr*)&addr, sizeof(addr)) < 0)
            return std::unexpected(std::strerror(errno));

        if (::send(client_sock.get(), &req, sizeof(req), 0) != sizeof(req))
            return std::unexpected("Send failed");

        auto fd_res = UdsIpc::recv_fd(client_sock.get());
        if (!fd_res) return std::unexpected(fd_res.error());

        IpcClient client;
        client.size_ = size;
        client.shm_fd_ = std::move(fd_res.value());
        
        client.mapped_ptr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, 
                                    MAP_SHARED, client.shm_fd_.get(), 0);
        if (client.mapped_ptr_ == MAP_FAILED) return std::unexpected(std::strerror(errno));

        return client;
    }
};