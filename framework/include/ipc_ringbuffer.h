#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <new>
#include <span>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <climits>
#include <thread>

template <typename T>
concept IpcSafe = std::is_trivially_copyable_v<T>;

template <IpcSafe T>
struct alignas(64) Slot {
    std::atomic<uint64_t> seqlock{0};
    T payload;
};

template <IpcSafe T>
struct alignas(64) RingBufferHeader {
    std::atomic<uint64_t> head_sequence{0};
    std::atomic<uint32_t> futex_bell{0};    
    uint64_t capacity{0};
    
    static constexpr size_t calculate_size(uint64_t cap) {
        return sizeof(RingBufferHeader<T>) + (sizeof(Slot<T>) * cap);
    }
};

template <IpcSafe T>
class IpcProducer {
public:
    IpcProducer(std::span<std::byte> memory, uint64_t capacity) {
        // Initialize header directly into shared memory
        header_ = new (memory.data()) RingBufferHeader<T>{0, 0, capacity};
        slots_ = reinterpret_cast<Slot<T>*>(memory.data() + sizeof(RingBufferHeader<T>));
    }

    void publish(const T& data) {
        uint64_t seq = header_->head_sequence.load(std::memory_order_relaxed);
        uint64_t index = seq % header_->capacity;
        auto& slot = slots_[index];

        // Mark slot as writing (odd)
        uint64_t current_seqlock = slot.seqlock.load(std::memory_order_relaxed);
        slot.seqlock.store(current_seqlock + 1, std::memory_order_release);
        
        slot.payload = data;
        
        // Mark slot as done (even)
        slot.seqlock.store(current_seqlock + 2, std::memory_order_release);

        // Advance sequence
        header_->head_sequence.store(seq + 1, std::memory_order_release);

        // Wake sleepers
        header_->futex_bell.fetch_add(1, std::memory_order_release);
        IpcFutex::wake_all(header_->futex_bell);
    }
private:
    RingBufferHeader<T>* header_;
    Slot<T>* slots_;
};

template <IpcSafe T>
class IpcListener {
public:
    explicit IpcListener(std::span<std::byte> memory) {
        header_ = reinterpret_cast<RingBufferHeader<T>*>(memory.data());
        slots_ = reinterpret_cast<Slot<T>*>(memory.data() + sizeof(RingBufferHeader<T>));
    }

    void wait_for_data(T& out_data) {
        uint64_t current_head = header_->head_sequence.load(std::memory_order_acquire);
        
        // Sleep if no new data
        while (local_sequence_ >= current_head) {
            uint32_t bell = header_->futex_bell.load(std::memory_order_acquire);
            if (local_sequence_ < header_->head_sequence.load(std::memory_order_acquire)) break;
            
            IpcFutex::wait(header_->futex_bell, bell);
            current_head = header_->head_sequence.load(std::memory_order_acquire);
        }

        // Handle rollover/falling behind
        if (current_head - local_sequence_ > header_->capacity) {
            local_sequence_ = current_head - header_->capacity;
        }

        uint64_t index = local_sequence_ % header_->capacity;
        auto& slot = slots_[index];

        // SeqLock Read
        while (true) {
            uint64_t seq1 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 % 2 != 0) {
                std::this_thread::yield();
                continue;
            }

            out_data = slot.payload;

            uint64_t seq2 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 == seq2) break;
        }
        local_sequence_++;
    }
private:
    RingBufferHeader<T>* header_;
    Slot<T>* slots_;
    uint64_t local_sequence_{0};
};