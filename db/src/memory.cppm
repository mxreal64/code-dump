module;

#include <sys/mman.h> // For mmap, MAP_HUGETLB
#include <unistd.h>

export module stridedb.memory;

import std;

export namespace StrideDB::Memory {

    constexpr std::size_t HUGEPAGE_2MB = 2 * 1024 * 1024; // 2097152 bytes

    class [[nodiscard]] HugePageArena {
    private:
        void* raw_ptr{nullptr};
        std::size_t allocated_size{0};

    public:
        HugePageArena() noexcept = default;

        // Prevent copying to avoid double-free memory corruption
        HugePageArena(const HugePageArena&) = delete;
        HugePageArena& operator=(const HugePageArena&) = delete;

        HugePageArena(HugePageArena&& other) noexcept 
            : raw_ptr(other.raw_ptr), allocated_size(other.allocated_size) {
            other.raw_ptr = nullptr;
            other.allocated_size = 0;
        }

        ~HugePageArena() noexcept {
            if (raw_ptr) [[likely]] {
                ::munmap(raw_ptr, allocated_size);
            }
        }

        // Allocates memory rounded up to the nearest 2MB hardware boundary
        bool allocate(std::size_t size_bytes) noexcept {
            // Round up size to a multiple of 2MB
            allocated_size = (size_bytes + HUGEPAGE_2MB - 1) & ~(HUGEPAGE_2MB - 1);

            // Request anonymous huge pages from the kernel
            raw_ptr = ::mmap(nullptr, allocated_size, 
                             PROT_READ | PROT_WRITE, 
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, 
                             -1, 0);

            if (raw_ptr == MAP_FAILED) [[unlikely]] {
                raw_ptr = nullptr;
                allocated_size = 0;
                return false;
            }
            return true;
        }

        [[nodiscard]] void* get() noexcept { return raw_ptr; }
        [[nodiscard]] const void* get() const noexcept { return raw_ptr; }
        [[nodiscard]] std::size_t size() const noexcept { return allocated_size; }
    };
}
