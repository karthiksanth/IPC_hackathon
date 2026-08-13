#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <span>
#include <cstring>
#include <thread>
#include "ipc_utils.h"

template <typename T>
class RelativePtr {
public:
    void set(T* target) noexcept {
        if (!target) { offset_ = 0; return; }
        const auto* self_bytes = reinterpret_cast<const std::byte*>(this);
        const auto* target_bytes = reinterpret_cast<const std::byte*>(target);
        offset_ = target_bytes - self_bytes;
    }

    [[nodiscard]] T* get() const noexcept {
        if (offset_ == 0) return nullptr;
        const auto* self_bytes = reinterpret_cast<const std::byte*>(this);
        return reinterpret_cast<T*>(const_cast<std::byte*>(self_bytes + offset_));
    }
private:
    std::ptrdiff_t offset_{0};
};

template <typename Meta>
struct alignas(64) EnvelopeSlot {
    std::atomic<uint64_t> seqlock{0};
    Meta metadata; 
    uint32_t payload_size{0};
    RelativePtr<std::byte> payload_ptr;
};

struct alignas(64) VariableMemoryHeader {
    std::atomic<uint64_t> head_sequence{0};
    std::atomic<uint32_t> futex_bell{0};
    uint64_t slot_capacity{0};
    std::atomic<uint64_t> heap_write_offset{0};
    uint64_t heap_total_capacity{0};

    // Helper for the Client API to know how much RAM to request from Broker
    template <typename Meta>
    static constexpr size_t calculate_size(uint64_t slot_cap, uint64_t heap_cap) {
        return sizeof(VariableMemoryHeader) + (sizeof(EnvelopeSlot<Meta>) * slot_cap) + heap_cap;
    }
};

template <typename Meta>
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

        uint64_t current_off = header_->heap_write_offset.load(std::memory_order_relaxed);
        uint64_t heap_index = current_off % header_->heap_total_capacity;
        
        if (heap_index + payload.size() > header_->heap_total_capacity) {
            current_off += (header_->heap_total_capacity - heap_index); 
            heap_index = 0;
        }
        
        std::byte* destination = heap_start_ + heap_index;
        std::memcpy(destination, payload.data(), payload.size());
        header_->heap_write_offset.store(current_off + payload.size(), std::memory_order_relaxed);

        uint64_t seq = header_->head_sequence.load(std::memory_order_relaxed);
        auto& slot = slots_[seq % header_->slot_capacity];

        uint64_t current_seqlock = slot.seqlock.load(std::memory_order_relaxed);
        slot.seqlock.store(current_seqlock + 1, std::memory_order_release);
        slot.metadata = meta;
        slot.payload_size = static_cast<uint32_t>(payload.size());
        slot.payload_ptr.set(destination);
        slot.seqlock.store(current_seqlock + 2, std::memory_order_release);

        header_->head_sequence.store(seq + 1, std::memory_order_release);
        header_->futex_bell.fetch_add(1, std::memory_order_release);
        IpcFutex::wake_all(header_->futex_bell);
    }

private:
    VariableMemoryHeader* header_;
    EnvelopeSlot<Meta>* slots_;
    std::byte* heap_start_;
};

template <typename Meta>
class VariableListener {
public:
    explicit VariableListener(std::span<std::byte> memory) {
        header_ = reinterpret_cast<VariableMemoryHeader*>(memory.data());
        slots_ = reinterpret_cast<EnvelopeSlot<Meta>*>(memory.data() + sizeof(VariableMemoryHeader));
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

        if (current_head - local_sequence_ > header_->slot_capacity) {
            local_sequence_ = current_head - header_->slot_capacity;
        }

        auto& slot = slots_[local_sequence_ % header_->slot_capacity];

        while (true) {
            uint64_t seq1 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 % 2 != 0) { std::this_thread::yield(); continue; }

            Meta meta = slot.metadata;
            uint32_t size = slot.payload_size;
            std::byte* payload_addr = slot.payload_ptr.get();

            uint64_t seq2 = slot.seqlock.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                std::span<const std::byte> data_view(payload_addr, size);
                process_func(meta, data_view);
                break;
            }
        }
        local_sequence_++;
    }
private:
    VariableMemoryHeader* header_;
    EnvelopeSlot<Meta>* slots_;
    uint64_t local_sequence_{0};
};