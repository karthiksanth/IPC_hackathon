#pragma once
#include "ipc_utils.h"
#include <atomic>
#include <span>

template <typename T>
struct alignas(64) Slot {
    std::atomic<uint64_t> seqlock{0};
    T payload;
};

template <typename T>
struct alignas(64) RingBufferHeader {
    std::atomic<uint64_t> head_sequence{0};
    std::atomic<uint32_t> futex_bell{0};
    uint64_t capacity{0};
    
    // Kept for memory layout consistency, updated only if KeepAllPolicy is used
    std::atomic<uint64_t> tail_sequence{0}; 

    static constexpr size_t calculate_size(uint64_t cap) {
        return sizeof(RingBufferHeader<T>) + (sizeof(Slot<T>) * cap);
    }
};

template <typename T, typename Policy = KeepLastPolicy>
class IpcProducer {
public:
    IpcProducer(std::span<std::byte> memory, uint64_t capacity) {
        header_ = new (memory.data()) RingBufferHeader<T>{
            .capacity = capacity
        };
        slots_ = reinterpret_cast<Slot<T>*>(memory.data() + sizeof(RingBufferHeader<T>));
    }

    void publish(const T& data) {
        uint64_t seq = header_->head_sequence.load(std::memory_order_relaxed);

        // --- ZERO-COST COMPILE-TIME BRANCHING ---
        // The compiler deletes this entirely if Policy == KeepLastPolicy!
        if constexpr (std::is_same_v<Policy, KeepAllPolicy>) {
            while (seq - header_->tail_sequence.load(std::memory_order_acquire) >= header_->capacity) {
                std::this_thread::yield(); 
            }
        }

        auto& slot = slots_[seq % header_->capacity];
        uint64_t current_seqlock = slot.seqlock.load(std::memory_order_relaxed);
        slot.seqlock.store(current_seqlock + 1, std::memory_order_release);
        
        slot.payload = data;
        
        slot.seqlock.store(current_seqlock + 2, std::memory_order_release);

        header_->head_sequence.store(seq + 1, std::memory_order_release);
        header_->futex_bell.fetch_add(1, std::memory_order_release);
        IpcFutex::wake_all(header_->futex_bell);
    }

private:
    RingBufferHeader<T>* header_;
    Slot<T>* slots_;
};

template <typename T, typename Policy = KeepLastPolicy>
class IpcListener {
public:
    explicit IpcListener(std::span<std::byte> memory) {
        header_ = reinterpret_cast<RingBufferHeader<T>*>(memory.data());
        slots_ = reinterpret_cast<Slot<T>*>(memory.data() + sizeof(RingBufferHeader<T>));
    }

    void wait_for_data(T& out_data) {
        uint64_t current_head = header_->head_sequence.load(std::memory_order_acquire);
        
        while (local_sequence_ >= current_head) {
            uint32_t bell = header_->futex_bell.load(std::memory_order_acquire);
            if (local_sequence_ < header_->head_sequence.load(std::memory_order_acquire)) break;
            IpcFutex::wait(header_->futex_bell, bell);
            current_head = header_->head_sequence.load(std::memory_order_acquire);
        }

        // --- ZERO-COST FAST FORWARD ---
        if constexpr (std::is_same_v<Policy, KeepLastPolicy>) {
            if (current_head - local_sequence_ > header_->capacity) {
                local_sequence_ = current_head - header_->capacity;
            }
        }

        auto& slot = slots_[local_sequence_ % header_->capacity];

        while (true) {
            uint64_t seq1 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 % 2 != 0) { std::this_thread::yield(); continue; }

            out_data = slot.payload;

            uint64_t seq2 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 == seq2) break;
        }

        local_sequence_++;

        // --- ZERO-COST BACKPRESSURE UPDATE ---
        if constexpr (std::is_same_v<Policy, KeepAllPolicy>) {
            header_->tail_sequence.store(local_sequence_, std::memory_order_release);
        }
    }

private:
    RingBufferHeader<T>* header_;
    Slot<T>* slots_;
    uint64_t local_sequence_{0};
};