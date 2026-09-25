export module ch86t.queue;

import <array>;
import <atomic>;
import <string>;

export namespace ch86t::queue {

    template<typename T, size_t Capacity>
    class SPSCQueue {
        static_assert((Capacity & (Capacity - 1)) == 0, "Queue capacity must be a power of 2.");
    private:
        std::array<T, Capacity> buffer;
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};
        static constexpr size_t Mask = Capacity - 1;

    public:
        SPSCQueue() = default;

        bool push(const T& item) {
            const size_t current_tail = tail.load(std::memory_order_relaxed);
            const size_t current_head = head.load(std::memory_order_acquire);
            if ((current_tail - current_head) >= Capacity) return false;
            buffer[current_tail & Mask] = item;
            tail.store(current_tail + 1, std::memory_order_release);
            return true;
        }

        bool pop(T& value) {
            const size_t current_head = head.load(std::memory_order_relaxed);
            const size_t current_tail = tail.load(std::memory_order_acquire);
            if (current_head == current_tail) return false;
            value = std::move(buffer[current_head & Mask]);
            head.store(current_head + 1, std::memory_order_release);
            return true;
        }
    };
}
