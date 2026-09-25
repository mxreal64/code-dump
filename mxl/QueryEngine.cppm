
module;

#include <immintrin.h>

extern "C" {
    int open(const char* pathname, int flags) noexcept;
    void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long offset) noexcept;
    int munmap(void* addr, unsigned long length) noexcept;
    int close(int fd) noexcept;
    long lseek(int fd, long offset, int whence) noexcept;
}

export module QueryEngine;

import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The vectorized query engine demands a native x86_64 hardware execution pipeline."
#endif

constexpr int MX_O_RDONLY   = 0x0000;
constexpr int MX_PROT_READ  = 0x01;
constexpr int MX_MAP_SHARED = 0x01;
constexpr int MX_SEEK_END   = 0x0002;

export struct QueryMatch {
    std::uint64_t timestamp;
    std::uint8_t level;
    std::string_view message;
};

export class VectorizedQueryEngine {
private:
    int file_fd_{-1};
    std::byte* mapped_file_{nullptr};
    std::size_t file_size_{0};

    static constexpr std::size_t align_block(std::size_t value) noexcept {
        return (value + 7) & ~static_cast<std::size_t>(7);
    }

public:
    VectorizedQueryEngine() noexcept = default;
    
    ~VectorizedQueryEngine() {
        if (mapped_file_ != nullptr) ::munmap(mapped_file_, file_size_);
        if (file_fd_ >= 0) ::close(file_fd_);
    }

    VectorizedQueryEngine(const VectorizedQueryEngine&) = delete;
    VectorizedQueryEngine& operator=(const VectorizedQueryEngine&) = delete;

    bool load_storage_file(const char* filepath) noexcept {
        file_fd_ = ::open(filepath, MX_O_RDONLY);
        if (file_fd_ < 0) [[unlikely]] return false;

        long sz = ::lseek(file_fd_, 0, MX_SEEK_END);
        if (sz <= 0) return false;
        file_size_ = static_cast<std::size_t>(sz);

        mapped_file_ = static_cast<std::byte*>(::mmap(nullptr, file_size_, MX_PROT_READ, MX_MAP_SHARED, file_fd_, 0));
        return mapped_file_ != reinterpret_cast<void*>(-1);
    }

    template <typename MatchHandler>
    std::size_t execute_level_filter(std::uint8_t target_level, std::size_t rows_per_group, std::size_t total_row_groups, MatchHandler&& handler) noexcept {
        if (mapped_file_ == nullptr || rows_per_group == 0 || total_row_groups == 0) return 0;

        std::size_t timestamp_bytes = align_block(rows_per_group * sizeof(std::uint64_t));
        std::size_t level_bytes     = align_block(rows_per_group * sizeof(std::uint8_t));
        std::size_t offset_bytes    = align_block(rows_per_group * sizeof(std::uint32_t));
        std::size_t length_bytes    = align_block(rows_per_group * sizeof(std::uint32_t));

        std::size_t match_count = 0;
        __m256i target_vec = _mm256_set1_epi8(static_cast<char>(target_level));

        std::byte* block_cursor = mapped_file_;

        // Loop through each distinct row group layout written by ColumnarStorage
        for (std::size_t g = 0; g < total_row_groups; ++g) {
            const auto* timestamps = reinterpret_cast<const std::uint64_t*>(block_cursor);
            const auto* levels     = reinterpret_cast<const std::uint8_t*>(block_cursor + timestamp_bytes);
            const auto* offsets    = reinterpret_cast<const std::uint32_t*>(block_cursor + timestamp_bytes + level_bytes);
            const auto* lengths    = reinterpret_cast<const std::uint32_t*>(block_cursor + timestamp_bytes + level_bytes + offset_bytes);
            
            const char* payload_base = reinterpret_cast<const char*>(
                block_cursor + timestamp_bytes + level_bytes + offset_bytes + length_bytes
            );

            // Compute the exact total payload string storage size dynamically from the last row index maps
            std::size_t payload_bytes = offsets[rows_per_group - 1] + lengths[rows_per_group - 1];
            std::size_t total_block_bytes = timestamp_bytes + level_bytes + offset_bytes + length_bytes + payload_bytes;

            std::size_t i = 0;
            
            if (rows_per_group >= 32) {
                std::size_t vector_limit = rows_per_group - 32;
                for (; i <= vector_limit; i += 32) {
                    const void* address_ptr = &levels[i];
                    __m256i level_vec = _mm256_loadu_si256(static_cast<const __m256i*>(address_ptr));
                    __m256i cmp_mask = _mm256_cmpeq_epi8(level_vec, target_vec);
                    std::uint32_t bitmask = static_cast<std::uint32_t>(_mm256_movemask_epi8(cmp_mask));

                    if (bitmask == 0) continue;

                    while (bitmask != 0) {
                        int relative_idx = __builtin_ctz(bitmask);
                        std::size_t local_idx = i + relative_idx;

                        std::string_view message_view(payload_base + offsets[local_idx], lengths[local_idx]);
                        handler(timestamps[local_idx], levels[local_idx], message_view);
                        
                        ++match_count;
                        bitmask &= (bitmask - 1); 
                    }
                }
            }

            // Clean residual serial loops
            for (; i < rows_per_group; ++i) {
                if (levels[i] == target_level) {
                    std::string_view message_view(payload_base + offsets[i], lengths[i]);
                    handler(timestamps[i], levels[i], message_view);
                    ++match_count;
                }
            }

            // Step cursor straight past this completed RowGroup allocation block
            block_cursor += total_block_bytes;
            if (static_cast<std::size_t>(block_cursor - mapped_file_) >= file_size_) break;
        }

        return match_count;
    }

    template <typename MatchHandler>
    [[nodiscard]] auto async_scan(std::uint8_t target_level, std::size_t rows_per_group, std::size_t total_row_groups, MatchHandler&& handler) noexcept {
        return [this, target_level, rows_per_group, total_row_groups, h = std::forward<MatchHandler>(handler)]() mutable noexcept {
            return this->execute_level_filter(target_level, rows_per_group, total_row_groups, h);
        };
    }
};
