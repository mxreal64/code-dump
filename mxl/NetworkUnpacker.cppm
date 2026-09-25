
export module NetworkUnpacker;

import std;
import StreamAggregator;
import KernelBypassIO; // Import the bypass module to handle persistent iovec definitions natively

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The network unpacking engine requires a native x86_64 hardware execution loop."
#endif

#pragma pack(push, 1)
struct ProtocolHeader {
    std::uint32_t magic{0};
    std::uint32_t length{0};
    std::uint32_t flags{0};
};
#pragma pack(pop)

export enum class ProcessingResult : std::uint8_t {
    Success,
    IncompleteHeader,
    IncompletePayload,
    InvalidMagicMarker,
    AggregatorFullBlock
};

export template <std::size_t RingCapacity>
class ConnectionContext {
private:
    static_assert((RingCapacity & (RingCapacity - 1)) == 0, "Network ring capacity must be a power of two.");
    static constexpr std::size_t Mask = RingCapacity - 1;

    int socket_fd_{-1};
    
    alignas(64) std::array<std::byte, RingCapacity> network_ring_{};
    std::uint64_t head_{0};
    std::uint64_t tail_{0};

    // Persistent, heap-allocated I/O descriptor tracking field
    // Slices stay completely alive for the duration of the context lifecycle
    iovec persistent_io_vector_{};

public:
    explicit ConnectionContext(int fd) noexcept : socket_fd_(fd) {}
    ~ConnectionContext() = default;

    ConnectionContext(const ConnectionContext&) = delete;
    ConnectionContext& operator=(const ConnectionContext&) = delete;

    [[nodiscard]] int get_fd() const noexcept { return socket_fd_; }
    [[nodiscard]] std::byte* get_write_ptr() noexcept { return &network_ring_[tail_ & Mask]; }
    [[nodiscard]] std::size_t get_available_space() const noexcept { return RingCapacity - (tail_ - head_); }
    
    void advance_tail(std::size_t bytes_written) noexcept { tail_ += bytes_written; }

    // FIXED: Strict contiguous chunk calculation completely shields glibc heap boundaries from out-of-bounds kernel overwrites
    [[nodiscard]] iovec* get_persistent_iovec() noexcept {
        std::size_t tail_idx = tail_ & Mask;
        
        // Calculate maximum physical space left before hitting the hard wrap-around wall of the vector array
        std::size_t contiguous_space = RingCapacity - tail_idx;
        
        // Total logical remaining tracking capacity inside the ring
        std::size_t total_logical_space = RingCapacity - (tail_ - head_);
        
        // Clamp the allowed write window securely so io_uring can never spill past our allocation frames
        persistent_io_vector_.iov_base = &network_ring_[tail_idx];
        persistent_io_vector_.iov_len  = std::min(contiguous_space, total_logical_space);
        
        return &persistent_io_vector_;
    }

    void consolidate_buffer() noexcept {
        if (head_ == tail_) {
            head_ = 0;
            tail_ = 0;
        } else if (head_ > RingCapacity && (head_ & Mask) == 0) {
            // Safe alignment normalization phase to prevent monotonic index overflow creep
            std::size_t bytes_active = tail_ - head_;
            std::size_t head_offset = head_ & Mask;
            std::size_t tail_offset = tail_ & Mask;

            if (head_offset < tail_offset) {
                std::memmove(network_ring_.data(), network_ring_.data() + head_offset, bytes_active);
            } else {
                // Wrap around structural memory normalization segment loop
                alignas(64) std::array<std::byte, RingCapacity> temp_buffer;
                std::size_t first_chunk = RingCapacity - head_offset;
                std::memcpy(temp_buffer.data(), network_ring_.data() + head_offset, first_chunk);
                std::memcpy(temp_buffer.data() + first_chunk, network_ring_.data(), tail_offset);
                std::memcpy(network_ring_.data(), temp_buffer.data(), bytes_active);
            }
            head_ = 0;
            tail_ = bytes_active;
        }
    }

    template <std::size_t AggregatorCapacity>
    ProcessingResult process_stream_buffer(HighThroughputLogAggregator<AggregatorCapacity>& aggregator) noexcept {
        constexpr std::size_t HeaderSize = sizeof(ProtocolHeader);

        while (true) {
            std::size_t active_bytes = tail_ - head_;
            if (active_bytes < HeaderSize) {
                return ProcessingResult::IncompleteHeader;
            }

            // Align a zero-copy pointer layout safely over the ring frame memory area
            alignas(alignof(ProtocolHeader)) ProtocolHeader header;
            std::size_t head_idx = head_ & Mask;

            if (head_idx + HeaderSize <= RingCapacity) {
                std::memcpy(&header, &network_ring_[head_idx], HeaderSize);
            } else {
                std::size_t partial_len = RingCapacity - head_idx;
                std::memcpy(&header, &network_ring_[head_idx], partial_len);
                std::memcpy(reinterpret_cast<std::byte*>(&header) + partial_len, network_ring_.data(), HeaderSize - partial_len);
            }

            if (header.magic != 0x4D58524C) [[unlikely]] {
                return ProcessingResult::InvalidMagicMarker;
            }

            if (active_bytes < HeaderSize + header.length) {
                return ProcessingResult::IncompletePayload;
            }

            std::size_t payload_start_idx = (head_ + HeaderSize) & Mask;
            head_ += HeaderSize;

            // Direct injection slice pass: completely drops standard library vector deep copies
            if (payload_start_idx + header.length <= RingCapacity) {
                // HOT-PATH: Data is contiguous. Pass the direct ring memory address pointer
                bool success = aggregator.append(&network_ring_[payload_start_idx], header.length);
                if (!success) [[unlikely]] {
                    head_ -= HeaderSize; // Revert head index mapping back to handle loop backpressure block safely
                    return ProcessingResult::AggregatorFullBlock;
                }
            } else {
                // FALLBACK-PATH: Fragmented ring packet normalization pass
                std::size_t chunk1_len = RingCapacity - payload_start_idx;
                std::size_t chunk2_len = header.length - chunk1_len;

                alignas(64) std::array<std::byte, 4096> contiguous_payload_arena;
                if (header.length <= contiguous_payload_arena.size()) {
                    std::memcpy(contiguous_payload_arena.data(), &network_ring_[payload_start_idx], chunk1_len);
                    std::memcpy(contiguous_payload_arena.data() + chunk1_len, network_ring_.data(), chunk2_len);
                    
                    // Pass the allocated heap/stack fallback arena memory block safely
                    bool success = aggregator.append(contiguous_payload_arena.data(), header.length);
                    if (!success) [[unlikely]] {
                        head_ -= HeaderSize;
                        return ProcessingResult::AggregatorFullBlock;
                    }
                }
            }

            head_ += header.length;
        }
        return ProcessingResult::Success;
    }
};
