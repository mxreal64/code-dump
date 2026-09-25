export module StreamAggregator;
import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "This architecture demands a hardware-native x86_64 instruction pipeline."
#endif

export struct alignas(16) LogRecord {
    std::uint32_t length;
    std::uint32_t flags;
    std::atomic<std::uint32_t> status;

    LogRecord() noexcept = default;
};

export template <std::size_t BufferCapacity>
class HighThroughputLogAggregator {
private:
    static_assert((BufferCapacity & (BufferCapacity - 1)) == 0, "Capacity configuration must be a power of two.");
    static constexpr std::size_t Mask = BufferCapacity - 1;
    
    static constexpr std::uint32_t FlagWrap    = 0x1;
    static constexpr std::uint32_t StatusReady = 0x2;

    alignas(64) std::byte storage_[BufferCapacity];
    alignas(64) std::atomic<std::uint64_t> write_head_{0};
    alignas(64) std::atomic<std::uint64_t> commit_head_{0};
    alignas(64) std::atomic<std::uint64_t> read_head_{0};

    static constexpr std::uint64_t align_up(std::uint64_t value) noexcept {
        return (value + 15) & ~15;
    }

public:
    HighThroughputLogAggregator() noexcept = default;
    ~HighThroughputLogAggregator() = default;

    bool append(const void* data, std::uint32_t length) noexcept {
        const std::uint64_t record_space = sizeof(LogRecord) + length;
        const std::uint64_t total_needed = align_up(record_space);

        if (total_needed > BufferCapacity) [[unlikely]] return false;
        std::uint64_t current_write = write_head_.load(std::memory_order_relaxed);

        while (true) {
            std::uint64_t current_read = read_head_.load(std::memory_order_acquire);
            if ((current_write - current_read) + total_needed > BufferCapacity) [[unlikely]] return false;

            std::uint64_t write_idx = current_write & Mask;
            std::uint64_t next_write = current_write + total_needed;
            bool wrap_needed = (write_idx + total_needed > BufferCapacity);
            std::uint32_t space_to_end = 0;

            if (wrap_needed) {
                space_to_end = static_cast<std::uint32_t>(BufferCapacity - write_idx);
                if ((current_write - current_read) + space_to_end + total_needed > BufferCapacity) [[unlikely]] return false;
                next_write = current_write + space_to_end + total_needed;
            }

            if (write_head_.compare_exchange_weak(current_write, next_write, std::memory_order_relaxed, std::memory_order_relaxed)) {
                std::uint64_t start_ticket = current_write;

                if (wrap_needed) {
                    auto* wrap_rec_ptr = ::new (static_cast<void*>(&storage_[write_idx])) LogRecord();
                    wrap_rec_ptr->length = 0;
                    wrap_rec_ptr->flags = FlagWrap;
                    wrap_rec_ptr->status.store(StatusReady, std::memory_order_release);
                    current_write += space_to_end;
                    write_idx = 0;
                }

                auto* data_rec_ptr = ::new (static_cast<void*>(&storage_[write_idx])) LogRecord();
                data_rec_ptr->length = length;
                data_rec_ptr->flags = 0;

                std::memcpy(&storage_[write_idx + sizeof(LogRecord)], data, length);
                
                data_rec_ptr->status.store(StatusReady, std::memory_order_release);

                std::uint64_t current_commit = commit_head_.load(std::memory_order_acquire);
                
                while (current_commit < write_head_.load(std::memory_order_relaxed)) {
                    std::uint64_t commit_idx = current_commit & Mask;
                    auto* commit_rec_ptr = reinterpret_cast<LogRecord*>(&storage_[commit_idx]);

                    if (commit_rec_ptr->status.load(std::memory_order_acquire) != StatusReady) {
                        break;
                    }

                    std::uint64_t step = 0;
                    if (commit_rec_ptr->flags & FlagWrap) {
                        step = BufferCapacity - commit_idx;
                    } else {
                        step = align_up(sizeof(LogRecord) + commit_rec_ptr->length);
                    }

                    if (commit_head_.compare_exchange_weak(current_commit, current_commit + step, 
                                                           std::memory_order_release, std::memory_order_acquire)) {
                        current_commit += step;
                    }
                }

                return true;
            }
        }
    }

    template <typename FlushHandler>
    std::size_t consume_batch(FlushHandler&& handler) noexcept {
        std::uint64_t current_read = read_head_.load(std::memory_order_relaxed);
        std::uint64_t current_commit = commit_head_.load(std::memory_order_acquire);

        if (current_read == current_commit) return 0;
        std::size_t processed_bytes = 0;

        while (current_read < current_commit) {
            std::uint64_t read_idx = current_read & Mask;
            auto* rec_ptr = reinterpret_cast<LogRecord*>(&storage_[read_idx]);

            if (rec_ptr->status.load(std::memory_order_acquire) != StatusReady) break; 

            if (rec_ptr->flags & FlagWrap) [[unlikely]] {
                std::uint32_t skip = static_cast<std::uint32_t>(BufferCapacity - read_idx);
                rec_ptr->status.store(0, std::memory_order_release);
                current_read += skip;
                processed_bytes += skip;
                continue;
            }

            std::forward<FlushHandler>(handler)(&storage_[read_idx + sizeof(LogRecord)], rec_ptr->length);
            std::uint64_t step = align_up(sizeof(LogRecord) + rec_ptr->length);
            
            // Clean out the slot status for future producer passes
            rec_ptr->status.store(0, std::memory_order_release);
            current_read += step;
            processed_bytes += step;
        }

        read_head_.store(current_read, std::memory_order_release);
        return processed_bytes;
    }

    auto async_consume() noexcept {
        return [this]() noexcept {
            return this->consume_batch([](const std::byte*, std::uint32_t) noexcept {});
        };
    }
};
