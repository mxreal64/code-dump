
module;

#include <immintrin.h>

export module ColumnarStorage;

import std;
import StreamAggregator;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The columnar storage engine requires a native x86_64 instruction pipeline."
#endif

extern "C" {
    int open(const char* pathname, int flags, unsigned int mode) noexcept;
    int fcntl(int fd, int cmd, long arg) noexcept; 
    long writev(int fd, const void* iov, int iovcnt) noexcept; 
    int close(int fd) noexcept;
}

constexpr int O_WRONLY    = 0x0001; // Decimal 1
constexpr int O_CREAT     = 0x0040; // Octal 00000100 -> Hex 0x40
constexpr int O_TRUNC     = 0x0200; // Octal 00001000 -> Hex 0x200
constexpr int O_DIRECT    = 0x4000; // Octal 00040000 -> Hex 0x4000
constexpr int F_SETFL     = 4;        

constexpr unsigned int MODE_644 = 0444; // Matches standard system user read/write layouts

struct iovec {
    void* iov_base;
    std::size_t iov_len;
};

export template <std::size_t RowGroupCapacity>
class ColumnarStorageEngine {
private:
    int file_descriptor_{-1};
    
    // Strict 4096-byte hardware virtual page alignment blocks
    alignas(4096) std::array<std::uint64_t, RowGroupCapacity> timestamp_column_{};
    alignas(4096) std::array<std::uint8_t, RowGroupCapacity>  level_column_{};
    
    // Zero-Copy arrays tracking memory locations natively inside the source aggregators
    alignas(4096) std::array<const std::byte*, RowGroupCapacity> payload_pointers_{};
    alignas(4096) std::array<std::uint32_t, RowGroupCapacity>    length_column_{}; 
    
    std::size_t current_row_count_{0};

    static constexpr std::size_t align_block(std::size_t value) noexcept {
        return (value + 4095) & ~static_cast<std::size_t>(4095); 
    }

    [[nodiscard]] static constexpr std::uint8_t parse_log_level_fast(char initial_char) noexcept {
        switch (initial_char) {
            case 'I': return 0; // INFO
            case 'W': return 1; // WARN
            case 'E': return 2; // ERROR
            case 'D': return 3; // DEBUG
            default:  return 0;
        }
    }

    // Fast AVX2 register matching scanner sweeps 32 bytes concurrently
    [[nodiscard]] static std::size_t find_space_avx2(const char* data, std::size_t length) noexcept {
        if (length < 32) {
            std::string_view sv(data, length);
            return sv.find(' ');
        }
        
        __m256i spaces = _mm256_set1_epi8(' ');
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
        
        __m256i cmp = _mm256_cmpeq_epi8(chunk, spaces);
        int mask = _mm256_movemask_epi8(cmp);
        
        if (mask != 0) {
            return __builtin_ctz(mask); 
        }
        return std::string_view(data, length).find(' ');
    }

public:
    ColumnarStorageEngine() noexcept = default;
    
    ~ColumnarStorageEngine() {
        if (file_descriptor_ >= 0) ::close(file_descriptor_);
    }

    // Split-pass file constructor handles creation safely across standard file systems
    bool create_storage_file(const char* filepath) noexcept {
        // Pass 1: Attempt direct un-cached O_DIRECT execution first
        file_descriptor_ = ::open(filepath, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        if (file_descriptor_ >= 0) {
            std::println("[+] Bare-Metal O_DIRECT disk bypass initialized successfully.");
            return true;
        }

        // Pass 2: Clean fallback pass completely strips out O_DIRECT using correct kernel hex tokens
        file_descriptor_ = ::open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (file_descriptor_ < 0) [[unlikely]] {
            std::println(std::cerr, "[-] Fatal Error: File system denied raw write access entirely.");
            return false;
        }

        std::println("[!] Filesystem constraints active. Safely falling back to synchronized page caching mode.");
        return true;
    }



    void ingest_record_to_columns(const std::byte* record_payload, std::uint32_t length) noexcept {
        if (current_row_count_ >= RowGroupCapacity) [[unlikely]] {
            flush_column_block_to_disk();
        }

        const char* raw_chars = reinterpret_cast<const char*>(record_payload);
        std::size_t delimiter_pos = find_space_avx2(raw_chars, length);
		// uint_8 for speed
        std::uint8_t numeric_level = 0; 
        const std::byte* final_payload_ptr = record_payload;
        std::uint32_t final_length = length;

        if (delimiter_pos != std::string_view::npos) {
            numeric_level = parse_log_level_fast(*raw_chars);
            final_payload_ptr = record_payload + delimiter_pos + 1;
            final_length = length - static_cast<std::uint32_t>(delimiter_pos + 1);
        }

        std::uint32_t tsc_aux;
        std::uint64_t hardware_timestamp = __builtin_ia32_rdtscp(&tsc_aux);

        timestamp_column_[current_row_count_] = hardware_timestamp;
        level_column_[current_row_count_]     = numeric_level;
        payload_pointers_[current_row_count_] = final_payload_ptr;
        length_column_[current_row_count_]    = final_length;

        ++current_row_count_;
    }

    void flush_column_block_to_disk() noexcept {
        if (current_row_count_ == 0 || file_descriptor_ < 0) return;
		// bytes alignment logic
        std::size_t timestamp_bytes = align_block(current_row_count_ * sizeof(std::uint64_t));
        std::size_t level_bytes     = align_block(current_row_count_ * sizeof(std::uint8_t));
        std::size_t pointer_bytes   = align_block(current_row_count_ * sizeof(const std::byte*));
        std::size_t length_bytes    = align_block(current_row_count_ * sizeof(std::uint32_t));

        std::vector<iovec> storage_vectors;
        storage_vectors.reserve(4 + current_row_count_);
        // vector_logic
        storage_vectors.push_back({ timestamp_column_.data(), timestamp_bytes });
        storage_vectors.push_back({ level_column_.data(),     level_bytes     });
        storage_vectors.push_back({ payload_pointers_.data(), pointer_bytes   });
        storage_vectors.push_back({ length_column_.data(),    length_bytes    });

        for (std::size_t i = 0; i < current_row_count_; ++i) {
            storage_vectors.push_back({ const_cast<std::byte*>(payload_pointers_[i]), length_column_[i] });
        }

        iovec* active_vec_ptr = storage_vectors.data();
        int active_entries = static_cast<int>(storage_vectors.size());
		// long used indtead of auto for safety
        long written = ::writev(file_descriptor_, active_vec_ptr, active_entries);
        (void)written; 

        current_row_count_ = 0;
    }

    auto async_ingest(const std::byte* payload, std::uint32_t length) noexcept {
        return [this, payload, length]() noexcept {
            this->ingest_record_to_columns(payload, length);
        };
    }
};
