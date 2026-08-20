#pragma once
#include "ipc_utils.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <span>
#include <cstring>
#include <vector>

template <typename Meta>
struct alignas(64) EnvelopeSlot {
    std::atomic<uint64_t> seqlock{0};
    Meta metadata; 
    uint32_t payload_size{0};
    uint64_t logical_payload_offset{0}; 
};

struct alignas(64) VariableMemoryHeader {
    std::atomic<uint64_t> head_sequence{0};
    std::atomic<uint32_t> futex_bell{0};
    uint64_t slot_capacity{0};
    
    std::atomic<uint64_t> slot_tail_sequence{0}; 

    std::atomic<uint64_t> logical_heap_write_head{0};
    std::atomic<uint64_t> logical_heap_read_tail{0}; 
    uint64_t heap_total_capacity{0};

    template <typename Meta>
    static constexpr size_t calculate_size(uint64_t slot_cap, uint64_t heap_cap) {
        return sizeof(VariableMemoryHeader) + (sizeof(EnvelopeSlot<Meta>) * slot_cap) + heap_cap;
    }
};

template <typename Meta, typename Policy = KeepLastPolicy>
class VariableProducer {
public:
    VariableProducer(std::span<std::byte> memory, uint64_t slot_capacity, uint64_t heap_capacity) {
        header_ = new (memory.data()) VariableMemoryHeader{
            .slot_capacity = slot_capacity,
            .heap_total_capacity = heap_capacity
        };
        size_t slots_offset = sizeof(VariableMemoryHeader);
        slots_ = reinterpret_cast<EnvelopeSlot<Meta>*>(memory.data() + slots_offset);
        heap_start_ = memory.data() + slots_offset + (sizeof(EnvelopeSlot<Meta>) * slot_capacity);
    }

    void publish(const Meta& meta, std::span<const std::byte> payload) {
        if (payload.size() > header_->heap_total_capacity) return;

        uint64_t current_heap_head = header_->logical_heap_write_head.load(std::memory_order_relaxed);
        uint64_t current_slot_head = header_->head_sequence.load(std::memory_order_relaxed);

        // --- COMPILE-TIME HEAP & SLOT BACKPRESSURE ---
        if constexpr (std::is_same_v<Policy, KeepAllPolicy>) {
            // Wait if Slots are full
            while (current_slot_head - header_->slot_tail_sequence.load(std::memory_order_acquire) >= header_->slot_capacity) {
                std::this_thread::yield();
            }
            // Wait if Heap is full
            while (current_heap_head - header_->logical_heap_read_tail.load(std::memory_order_acquire) + payload.size() > header_->heap_total_capacity) {
                std::this_thread::yield();
            }
        }

        uint64_t physical_heap_index = current_heap_head % header_->heap_total_capacity;
        if (physical_heap_index + payload.size() > header_->heap_total_capacity) {
            uint64_t gap = header_->heap_total_capacity - physical_heap_index;
            current_heap_head += gap; 
            physical_heap_index = 0;
        }
        
        std::byte* destination = heap_start_ + physical_heap_index;
        std::memcpy(destination, payload.data(), payload.size());

        auto& slot = slots_[current_slot_head % header_->slot_capacity];
        uint64_t current_seqlock = slot.seqlock.load(std::memory_order_relaxed);
        slot.seqlock.store(current_seqlock + 1, std::memory_order_release);
        
        slot.metadata = meta;
        slot.payload_size = static_cast<uint32_t>(payload.size());
        slot.logical_payload_offset = current_heap_head; 
        
        slot.seqlock.store(current_seqlock + 2, std::memory_order_release);

        header_->logical_heap_write_head.store(current_heap_head + payload.size(), std::memory_order_release);
        header_->head_sequence.store(current_slot_head + 1, std::memory_order_release);
        header_->futex_bell.fetch_add(1, std::memory_order_release);
        IpcFutex::wake_all(header_->futex_bell);
    }

private:
    VariableMemoryHeader* header_;
    EnvelopeSlot<Meta>* slots_;
    std::byte* heap_start_;
};

template <typename Meta, typename Policy = KeepLastPolicy>
class VariableListener {
public:
    explicit VariableListener(std::span<std::byte> memory) {
        header_ = reinterpret_cast<VariableMemoryHeader*>(memory.data());
        slots_ = reinterpret_cast<EnvelopeSlot<Meta>*>(memory.data() + sizeof(VariableMemoryHeader));
        heap_start_ = memory.data() + sizeof(VariableMemoryHeader) + (sizeof(EnvelopeSlot<Meta>) * header_->slot_capacity);
    }

    template <typename Callback>
    void wait_and_read(Callback&& process_func) {
        uint64_t current_head = header_->head_sequence.load(std::memory_order_acquire);
        
        while (local_sequence_ >= current_head) {
            uint32_t bell = header_->futex_bell.load(std::memory_order_acquire);
            if (local_sequence_ < header_->head_sequence.load(std::memory_order_acquire)) break;
            IpcFutex::wait(header_->futex_bell, bell);
            current_head = header_->head_sequence.load(std::memory_order_acquire);
        }

        // --- COMPILE-TIME FAST FORWARD ---
        if constexpr (std::is_same_v<Policy, KeepLastPolicy>) {
            if (current_head - local_sequence_ > header_->slot_capacity) {
                local_sequence_ = current_head - header_->slot_capacity;
            }
        }

        auto& slot = slots_[local_sequence_ % header_->slot_capacity];

        while (true) {
            uint64_t seq1 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 % 2 != 0) { std::this_thread::yield(); continue; }

            Meta meta = slot.metadata;
            uint32_t size = slot.payload_size;
            uint64_t logical_offset = slot.logical_payload_offset;

            // Logical Sliding Window Check
            uint64_t current_heap_head = header_->logical_heap_write_head.load(std::memory_order_acquire);
            if (current_heap_head - logical_offset > header_->heap_total_capacity) {
                break; // Heap crushed data, drop frame safely
            }

            std::byte* payload_addr = heap_start_ + (logical_offset % header_->heap_total_capacity);

            local_payload_buffer_.resize(size);
            if (size > 0) {
                std::memcpy(local_payload_buffer_.data(), payload_addr, size);
            }

            uint64_t heap_head_after_copy = header_->logical_heap_write_head.load(std::memory_order_acquire);
            if (heap_head_after_copy - logical_offset > header_->heap_total_capacity) {
                continue; 
            }

            uint64_t seq2 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                std::span<const std::byte> safe_data_view(local_payload_buffer_);
                process_func(meta, safe_data_view);
                
                // --- COMPILE-TIME BACKPRESSURE HEAP UPDATE ---
                if constexpr (std::is_same_v<Policy, KeepAllPolicy>) {
                    header_->logical_heap_read_tail.store(logical_offset + size, std::memory_order_release);
                }
                break;
            }
        }

        local_sequence_++;
        
        // --- COMPILE-TIME BACKPRESSURE SLOT UPDATE ---
        if constexpr (std::is_same_v<Policy, KeepAllPolicy>) {
            header_->slot_tail_sequence.store(local_sequence_, std::memory_order_release);
        }
    }

private:
    VariableMemoryHeader* header_;
    EnvelopeSlot<Meta>* slots_;
    uint64_t local_sequence_{0};
    std::vector<std::byte> local_payload_buffer_;
    std::byte* heap_start_;
};