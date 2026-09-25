export module data_transformer.spmc;

import data_transformer.types;
import std;

export namespace transformer::spmc {

    using transformer::types::JsonChunk;
    using transformer::types::CACHE_LINE_SIZE;

    constexpr std::size_t RING_CAPACITY = 512;
    constexpr std::size_t INDEX_MASK = RING_CAPACITY - 1;

    struct alignas(CACHE_LINE_SIZE) SpmcQueueState {
        alignas(CACHE_LINE_SIZE) JsonChunk ring[RING_CAPACITY];
        alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> head{0}; 
        alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> tail{0}; 
    };

    /**
     * Pushes a completed I/O memory chunk into the ring buffer state.
     * Only ONE thread (the io_uring ingest thread) can ever execute this function.
     */
    inline bool push(SpmcQueueState& state, const JsonChunk& chunk) noexcept {
        const std::uint64_t current_tail = state.tail.load(std::memory_order_relaxed);
        const std::uint64_t current_head = state.head.load(std::memory_order_acquire);

        if ((current_tail - current_head) >= RING_CAPACITY) {
            return false;
        }

        state.ring[current_tail & INDEX_MASK] = chunk;
        state.tail.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * Pops a chunk out of the ring buffer state for active worker thread processing.
     * Executed concurrently by multiple worker thread cores simultaneously.
     */
    inline bool pop(SpmcQueueState& state, JsonChunk& out_chunk) noexcept {
        // Changed to acquire to guarantee synchronization with the push release step
        std::uint64_t current_head = state.head.load(std::memory_order_acquire);

        while (true) {
            const std::uint64_t current_tail = state.tail.load(std::memory_order_acquire);

            if (current_head == current_tail) {
                return false;
            }

            // Speculative read keeps the memory lines pre-fetched in your CPU cache core
            JsonChunk speculative_chunk = state.ring[current_head & INDEX_MASK];

            // Atomic CAS. Changed success to acquire/release (acq_rel) to create a barrier
            // that blocks compiler and hardware reordering around the read operation.
            if (state.head.compare_exchange_weak(current_head, current_head + 1,
                                                 std::memory_order_acq_rel,   
                                                 std::memory_order_acquire)) { 
                
                // LOCK SECURED: Re-read the ring array index directly from the synchronized slot.
                // If another thread modified it before your CAS won, this pulls the correct new data.
                out_chunk = state.ring[current_head & INDEX_MASK];
                return true; 
            }

            // Pure x86 native execution hint. Relieves pipeline strain on the execution core.
            asm volatile("pause" ::: "memory");
        }
    }
} // namespace transformer::spmc
