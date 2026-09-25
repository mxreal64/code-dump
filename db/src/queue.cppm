export module stridedb.queue;

import std;

export namespace StrideDB::Queue {

    template <typename T, std::size_t Capacity = 16>
    class [[nodiscard]] LockFreeSPSCQueue {
    private:
        std::array<T, Capacity> buffer{};
        alignas(64) std::atomic<std::size_t> head{0};
        alignas(64) std::atomic<std::size_t> tail{0};

    public:
        LockFreeSPSCQueue() noexcept = default;

        bool push(const T& item) noexcept {
            const std::size_t current_tail = tail.load(std::memory_order_relaxed);
            const std::size_t current_head = head.load(std::memory_order_acquire);

            if ((current_tail + 1) % Capacity == current_head) {
                return false; 
            }

            buffer[current_tail] = item;
            tail.store((current_tail + 1) % Capacity, std::memory_order_release);
            return true;
        }

        [[nodiscard]] std::optional<T> pop() noexcept {
            const std::size_t current_head = head.load(std::memory_order_relaxed);
            const std::size_t current_tail = tail.load(std::memory_order_acquire);

            if (current_head == current_tail) {
                return std::nullopt; 
            }

            T item = buffer[current_head];
            head.store((current_head + 1) % Capacity, std::memory_order_release);
            return item;
        }
    };
}
