
export module LogWatcher;

import std;
import LogShipper;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "This performance tracking engine requires a native x86_64 instruction pipeline."
#endif

extern "C" {
    int inotify_init1(int flags) noexcept;
    int inotify_add_watch(int fd, const char* pathname, unsigned int mask) noexcept;
    int open(const char* pathname, int flags) noexcept;
    long lseek(int fd, long offset, int whence) noexcept;
    void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long offset) noexcept;
    int munmap(void* addr, unsigned long length) noexcept;
    int close(int fd) noexcept;
    long read(int fd, void* buf, unsigned long count) noexcept;
    
    int epoll_create1(int flags) noexcept;
    int epoll_ctl(int epfd, int op, int fd, void* event) noexcept;
    int epoll_wait(int epfd, void* events, int maxevents, int timeout) noexcept;
}

constexpr int IN_MODIFY      = 0x00000002;
constexpr int IN_NONBLOCK    = 00004000;
constexpr int O_RDONLY       = 00000000;
constexpr int PROT_READ      = 0x1;
constexpr int MAP_SHARED     = 0x01;
constexpr int SEEK_END       = 2;
const void* const MX_MAP_FAILED = (void*)(-1);

constexpr int EPOLLIN        = 0x001;
constexpr int EPOLLET        = 1u << 31; 
constexpr int EPOLL_CTL_ADD  = 1;

union epoll_data {
    void* ptr;
    int fd;
    std::uint32_t u32;
    std::uint64_t u64;
};

struct epoll_event {
    std::uint32_t events;
    epoll_data data;
} __attribute__((packed, aligned(4))); // Matches kernel structure layouts exactly

struct inotify_event {
    int wd;
    std::uint32_t mask;
    std::uint32_t cookie;
    std::uint32_t len;
    char name[]; // Fixed to standard flexible array token
};

export class ZeroAllocLogWatcher {
private:
    int inotify_fd_{-1};
    int epoll_fd_{-1};
    int watch_descriptor_{-1};
    int log_file_fd_{-1};
    
    std::byte* mmap_ptr_{nullptr};
    std::size_t last_known_size_{0};
    std::size_t current_mmap_capacity_{0};

    BareMetalShipper& shipper_;

    void sync_memory_map() noexcept {
        long target_size = ::lseek(log_file_fd_, 0, SEEK_END);
        if (target_size <= static_cast<long>(last_known_size_)) return;

        auto exact_file_size = static_cast<std::size_t>(target_size);

        if (exact_file_size > current_mmap_capacity_) {
            if (mmap_ptr_ != nullptr) {
                ::munmap(mmap_ptr_, current_mmap_capacity_);
            }
            void* new_map = ::mmap(nullptr, exact_file_size, PROT_READ, MAP_SHARED, log_file_fd_, 0);
            if (new_map == MX_MAP_FAILED) [[unlikely]] return;
            
            mmap_ptr_ = static_cast<std::byte*>(new_map);
            current_mmap_capacity_ = exact_file_size;
        }

        std::size_t new_bytes_count = exact_file_size - last_known_size_;
        std::string_view fresh_log_window(reinterpret_cast<const char*>(mmap_ptr_ + last_known_size_), new_bytes_count);

        std::size_t line_start = 0;
        while (line_start < fresh_log_window.length()) {
            std::size_t newline_pos = fresh_log_window.find('\n', line_start);
            if (newline_pos == std::string_view::npos) break; 

            std::size_t line_len = newline_pos - line_start;
            if (line_len > 0) {
                shipper_.ship_log(fresh_log_window.data() + line_start, static_cast<std::uint32_t>(line_len));
            }
            line_start = newline_pos + 1;
        }
        last_known_size_ += line_start;
    }

public:
    explicit ZeroAllocLogWatcher(BareMetalShipper& shipper) noexcept 
        : shipper_(shipper) 
    {
        inotify_fd_ = ::inotify_init1(IN_NONBLOCK);
        epoll_fd_ = ::epoll_create1(0);
    }

    ~ZeroAllocLogWatcher() {
        if (mmap_ptr_ != nullptr) ::munmap(mmap_ptr_, current_mmap_capacity_);
        if (log_file_fd_ >= 0) ::close(log_file_fd_);
        if (inotify_fd_ >= 0) ::close(inotify_fd_);
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    bool initialize_target_file(const char* filepath) noexcept {
        log_file_fd_ = ::open(filepath, O_RDONLY);
        if (log_file_fd_ < 0) [[unlikely]] return false;

        long initial_size = ::lseek(log_file_fd_, 0, SEEK_END);
        last_known_size_ = static_cast<std::size_t>(initial_size);

        watch_descriptor_ = ::inotify_add_watch(inotify_fd_, filepath, IN_MODIFY);
        if (watch_descriptor_ < 0) return false;

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = inotify_fd_;
        
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, inotify_fd_, &ev) == 0;
    }

    std::size_t wait_and_process_events(int timeout_ms) noexcept {
        std::array<epoll_event, 8> event_window;
        int active_fds = ::epoll_wait(epoll_fd_, event_window.data(), 8, timeout_ms);
        if (active_fds <= 0) return 0;

        // Proper alignment parameters prevent unaligned pointer hardware crashes
        alignas(alignof(inotify_event)) std::array<std::byte, 4096> event_buffer;
        std::size_t total_processed = 0;

        while (true) {
            long read_bytes = ::read(inotify_fd_, event_buffer.data(), event_buffer.size());
            if (read_bytes <= 0) break;

            std::size_t progression = 0;
            while (progression < static_cast<std::size_t>(read_bytes)) {
                auto* event = reinterpret_cast<inotify_event*>(&event_buffer[progression]);
                if (event->mask & IN_MODIFY) {
                    sync_memory_map();
                    ++total_processed;
                }
                progression += sizeof(inotify_event) + event->len;
            }
        }
        return total_processed;
    }
};
