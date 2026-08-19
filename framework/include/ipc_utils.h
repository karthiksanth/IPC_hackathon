#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <string>
#include <expected>
#include <cstring>
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <climits>
#include <atomic>
#include <type_traits> 

// --- COMPILE-TIME QoS POLICIES ---
struct KeepLastPolicy {}; 
struct KeepAllPolicy {};

// --- MODERN C++ ERROR HANDLING ---
template <typename T>
using Result = std::expected<T, std::string>;

// --- RAII FILE DESCRIPTOR WRAPPER ---
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }
    
    void reset() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
private:
    int fd_{-1};
};

// --- UNIX DOMAIN SOCKET IPC (FD PASSING) ---
class UdsIpc {
public:
    static Result<void> send_fd(int socket_fd, int fd_to_send) {
        struct msghdr msg{}; 
        char buf[1] = {'!'}; 
        
        struct iovec iov{}; 
        iov.iov_base = buf;
        iov.iov_len = 1;
        
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } control_msg{}; 

        msg.msg_control = control_msg.buf;
        msg.msg_controllen = sizeof(control_msg.buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

        if (::sendmsg(socket_fd, &msg, 0) < 0) {
            return std::unexpected(std::strerror(errno));
        }
        return {};
    }

    static Result<UniqueFd> recv_fd(int socket_fd) {
        struct msghdr msg{}; 
        char buf[1];
        
        struct iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = 1;
        
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } control_msg{}; 

        msg.msg_control = control_msg.buf;
        msg.msg_controllen = sizeof(control_msg.buf);

        if (::recvmsg(socket_fd, &msg, 0) < 0) {
            return std::unexpected(std::strerror(errno));
        }

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int received_fd;
            std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
            return UniqueFd{received_fd};
        }
        return std::unexpected("No valid FD received");
    }
};

// --- LINUX KERNEL FUTEX WRAPPER ---
class IpcFutex {
public:
    static void wait(std::atomic<uint32_t>& futex_word, uint32_t expected_val) {
        ::syscall(SYS_futex, &futex_word, FUTEX_WAIT, expected_val, nullptr, nullptr, 0);
    }
    static void wake_all(std::atomic<uint32_t>& futex_word) {
        ::syscall(SYS_futex, &futex_word, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }
};