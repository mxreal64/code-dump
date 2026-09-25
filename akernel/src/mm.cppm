// File: src/mm.cppm
export module kernel86.mm;

import kernel86.types;

export namespace kernel86::mm {

    // Removed redundant 'export' keyword
    enum class CapRights : uint8_t {
        None  = 0,
        Read  = 1 << 0,
        Write = 1 << 1,
        Exec  = 1 << 2
    };

    // Removed redundant 'export' keyword
    struct Capability {
        uint32_t slot_index;
        uint32_t generation;
    };

    // Internal tracker for what a capability pointer can actually see
    struct CapSlot {
        uintptr_t physical_address{0};
        size_t page_count{0};
        uint32_t generation{0};
        CapRights rights{CapRights::None};
        bool active{false};
    };

    // Removed redundant 'export' keyword
    class MemoryManager {
    private:
        static constexpr uintptr_t MemoryStart = 0x01000000; // 16MB Mark boundary
        static constexpr size_t TotalPages = 123904;         // 484MB / 4KB pages
        static constexpr size_t BitmapSize = TotalPages / 64; // Tracking array size

        // Page tracking bitmap: 1 = Allocated, 0 = Free
        uint64_t page_bitmap[BitmapSize]{0};

        // Capability space layout (Fixed table size for raw caching)
        static constexpr size_t MaxCapabilities = 512;
        CapSlot cap_table[MaxCapabilities];
        uint32_t generation_counter{1};

        // Internal bitmap bit helpers
        constexpr void SetBit(size_t page_idx) noexcept { page_bitmap[page_idx / 64] |= (1ULL << (page_idx % 64)); }
        constexpr void ClearBit(size_t page_idx) noexcept { page_bitmap[page_idx / 64] &= ~(1ULL << (page_idx % 64)); }
        constexpr bool TestBit(size_t page_idx) const noexcept { return (page_bitmap[page_idx / 64] & (1ULL << (page_idx % 64))) != 0; }

        // Find continuous block of free pages
        int FindFreePages(size_t count) const noexcept {
            size_t continuous = 0;
            for (size_t i = 0; i < TotalPages; ++i) {
                if (!TestBit(i)) {
                    continuous++;
                    if (continuous == count) return static_cast<int>(i - count + 1);
                } else {
                    continuous = 0;
                }
            }
            return -1;
        }

    public:
        constexpr MemoryManager() noexcept = default;

        // Allocate pages and return a type-safe capability token link
        auto AllocateObject(size_t pages, CapRights rights) noexcept -> auto {
            struct Result { Status status; Capability cap; };

            if (pages == 0) return Result{Status::InvalidArgument, {0, 0}};

            int start_page = FindFreePages(pages);
            if (start_page == -1) return Result{Status::OutOfMemory, {0, 0}};

            // Reserve the bits in our L2 cache bitmap
            for (size_t i = 0; i < pages; ++i) {
                SetBit(start_page + i);
            }

            // Calculate exact physical address allocation offset
            uintptr_t phys_addr = MemoryStart + (static_cast<uintptr_t>(start_page) * 4096);

            // Find an empty capability slot to register the hardware address
            for (size_t i = 0; i < MaxCapabilities; ++i) {
                if (!cap_table[i].active) {
                    cap_table[i] = CapSlot{
                        .physical_address = phys_addr,
                        .page_count = pages,
                        .generation = generation_counter++,
                        .rights = rights,
                        .active = true
                    };
                    return Result{Status::Success, Capability{.slot_index = static_cast<uint32_t>(i), .generation = cap_table[i].generation}};
                }
            }

            return Result{Status::OutOfMemory, {0, 0}};
        }

        // Securely translate a capability back to a physical address if token matches
        auto Translate(Capability cap, CapRights required_right) const noexcept -> auto {
            struct Result { Status status; uintptr_t address; };

            if (cap.slot_index >= MaxCapabilities) return Result{Status::InvalidCapability, 0};

            const auto& slot = cap_table[cap.slot_index];
            if (!slot.active || slot.generation != cap.generation) {
                return Result{Status::InvalidCapability, 0};
            }

            // Enforce secure bitwise rights validation check
            if ((static_cast<uint8_t>(slot.rights) & static_cast<uint8_t>(required_right)) != static_cast<uint8_t>(required_right)) {
                return Result{Status::InvalidCapability, 0};
            }

            return Result{Status::Success, slot.physical_address};
        }
    };
}
