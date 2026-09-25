// File: ipc.cppm
export module kernel.ipc;

import kernel.core_types;

export namespace kernel::ipc {

    struct alignas(64) Message {
        uint32_t command;
        uintptr_t payload;
    };

    class alignas(64) CoreQueue {
    private:
        static constexpr size_t Capacity = 128;
        alignas(64) Message buffer[Capacity];
        alignas(64) uint32_t head{0};
        alignas(64) uint32_t tail{0};

    public:
        // Core 0 pushes graphics commands into the L2 cache
        bool Push(Message msg) noexcept {
            uint32_t next = (head + 1) % Capacity;
            if (next == tail) return false; // Full
            buffer[head] = msg;
            // Enforce memory order at compile-time/runtime for the x86-64 C2D
            asm volatile("" ::: "memory");
            head = next;
            return true;
        }

        // Core 1 continuously pops and dumps to the weak integrated graphics
        bool Pop(Message& msg) noexcept {
            if (head == tail) return false; // Empty
            msg = buffer[tail];
            asm volatile("" ::: "memory");
            tail = (tail + 1) % Capacity;
            return true;
        }
    };
}
