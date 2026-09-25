export module data_transformer.egress;

import data_transformer.types;
import std;

export namespace transformer::egress {

    using transformer::types::RowGroupMemoryBuffer;

    // Fixed capacity power-of-two tracking ring buffer
    constexpr std::size_t CAPACITY = 64;
    constexpr std::size_t MASK = CAPACITY - 1;

    struct alignas(64) OutboundEgressQueue {
        // FIXED: Wrap array entries inside atomics to prevent instruction re-ordering across threads
        std::atomic<RowGroupMemoryBuffer*> ring[CAPACITY]{nullptr};
        
        alignas(64) std::atomic<std::uint64_t> head{0}; 
        alignas(64) std::atomic<std::uint64_t> tail{0}; 
    };

    /**
     * Safe lock-free push. Invoked concurrently across multiple worker cores.
     */
    inline bool push_egress(OutboundEgressQueue& q, RowGroupMemoryBuffer* buffer) noexcept {
        std::uint64_t current_tail = q.tail.load(std::memory_order_relaxed);
        while (true) {
            const std::uint64_t current_head = q.head.load(std::memory_order_acquire);
            if ((current_tail - current_head) >= CAPACITY) {
                return false; 
            }
            // Claim the index sequence ticket safely across multiple producers
            if (q.tail.compare_exchange_weak(current_tail, current_tail + 1, 
                                             std::memory_order_relaxed, 
                                             std::memory_order_relaxed)) {
                // FIXED: Use release semantics to guarantee the buffer data is visible before the pointer lands
                q.ring[current_tail & MASK].store(buffer, std::memory_order_release);
                return true;
            }
            asm volatile("pause" ::: "memory");
        }
    }

    /**
     * Zero-lock pop interface. Executed exclusively by the single writer thread core.
     */
    inline bool pop_egress(OutboundEgressQueue& q, RowGroupMemoryBuffer*& out_buffer) noexcept {
        const std::uint64_t current_head = q.head.load(std::memory_order_relaxed);
        const std::uint64_t current_tail = q.tail.load(std::memory_order_acquire);
        
        if (current_head == current_tail) return false; 
        
        // FIXED: Use acquire memory order semantics to safely pull the producer's written slot
        out_buffer = q.ring[current_head & MASK].load(std::memory_order_acquire);
        if (!out_buffer) {
            // Ticket was claimed by producer but pointer isn't written yet. Back off and wait.
            return false; 
        }
        
        // Clear the slot track cleanly and advance the head pointer fence
        q.ring[current_head & MASK].store(nullptr, std::memory_order_relaxed);
        q.head.store(current_head + 1, std::memory_order_release);
        return true;
    }

} // namespace transformer::egress
