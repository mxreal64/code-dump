
module;

#include <sys/syscall.h>
#include <linux/io_uring.h>

export module KernelBypassIO;

import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The io_uring engine demands a native x86_64 hardware instruction execution loop."
#endif

constexpr int PROT_READ  = 0x1;
constexpr int PROT_WRITE = 0x2;
constexpr int MAP_SHARED = 0x01;

extern "C" {
    long syscall(long number, ...) noexcept;
    void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long offset) noexcept;
    int munmap(void* addr, unsigned long length) noexcept;
    int close(int fd) noexcept;
}

export struct iovec {
    void* iov_base;
    std::size_t iov_len;
};

export struct io_completion_result {
    std::uint64_t token;
    std::int32_t  bytes_transferred;
};

export class KernelBypassRingContext {
private:
    int ring_fd_{-1};
    
    // SQ Ring shared memory mapping references using official kernel types
    std::uint32_t* sq_head_{nullptr};
    std::uint32_t* sq_tail_{nullptr};
    std::uint32_t  sq_mask_{0};
    std::uint32_t* sq_array_{nullptr};
    io_uring_sqe*  sqes_{nullptr};
    std::size_t sq_mmap_len_{0};
    std::size_t sqes_mmap_len_{0};

    // CQ Ring shared memory mapping references using official kernel types
    std::uint32_t* cq_head_{nullptr};
    std::uint32_t* cq_tail_{nullptr};
    std::uint32_t  cq_mask_{0};
    io_uring_cqe*  cqes_{nullptr};
    std::size_t    cq_mmap_len_{0};

public:
    KernelBypassRingContext() noexcept = default;
    
    ~KernelBypassRingContext() {
        if (sqes_ != nullptr)    ::munmap(sqes_, sqes_mmap_len_);
        if (sq_head_ != nullptr) ::munmap(sq_head_, sq_mmap_len_);
        if (cq_head_ != nullptr) ::munmap(cq_head_, cq_mmap_len_);
        if (ring_fd_ >= 0)       ::close(ring_fd_);
    }

    bool initialize_bypass_ring(std::uint32_t entries) noexcept {
        io_uring_params params{};
        std::memset(&params, 0, sizeof(io_uring_params));
        
        params.flags = 0;

        long fd = ::syscall(SYS_io_uring_setup, entries, &params);
        if (fd < 0) [[unlikely]] return false;
        ring_fd_ = static_cast<int>(fd);

        sq_mmap_len_   = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
        cq_mmap_len_   = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        sqes_mmap_len_ = params.sq_entries * sizeof(io_uring_sqe);

        void* sq_ptr = ::mmap(nullptr, sq_mmap_len_, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ptr == reinterpret_cast<void*>(-1)) return false;

        std::byte* sq_base = static_cast<std::byte*>(sq_ptr);
        sq_head_  = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.head);
        sq_tail_  = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.tail);
        sq_mask_  = *reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.ring_mask);
        sq_array_ = reinterpret_cast<std::uint32_t*>(sq_base + params.sq_off.array);

        void* sqes_ptr = ::mmap(nullptr, sqes_mmap_len_, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd_, IORING_OFF_SQES);
        if (sqes_ptr == reinterpret_cast<void*>(-1)) return false;
        sqes_ = static_cast<io_uring_sqe*>(sqes_ptr);

        void* cq_ptr = ::mmap(nullptr, cq_mmap_len_, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd_, IORING_OFF_CQ_RING);
        if (cq_ptr == reinterpret_cast<void*>(-1)) return false;

        std::byte* cq_base = static_cast<std::byte*>(cq_ptr);
        cq_head_ = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.head);
        cq_tail_ = reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.tail);
        cq_mask_ = *reinterpret_cast<std::uint32_t*>(cq_base + params.cq_off.ring_mask);
        cqes_    = reinterpret_cast<io_uring_cqe*>(cq_base + params.cq_off.cqes);

        return true;
    }

    bool submit_async_readv(int target_fd, const iovec* io_vectors, std::uint32_t count, std::uint64_t tracking_token) noexcept {
        std::uint32_t current_tail = *sq_tail_;
        std::uint32_t index = current_tail & sq_mask_;

        io_uring_sqe& sqe = sqes_[index];
        std::memset(&sqe, 0, sizeof(io_uring_sqe));
        
        sqe.opcode    = IORING_OP_READV;
        sqe.fd        = target_fd;
        sqe.addr      = reinterpret_cast<std::uint64_t>(io_vectors);
        sqe.len       = count;
        sqe.user_data = tracking_token;

        sq_array_[index] = index;
        
        std::atomic_ref<std::uint32_t>(*sq_tail_).store(current_tail + 1, std::memory_order_release);

        ::syscall(SYS_io_uring_enter, ring_fd_, 1, 0, 0, nullptr);
        return true;
    }

    std::size_t reap_completion_events(std::span<io_completion_result> finished_events_out) noexcept {
        std::uint32_t current_head = *cq_head_;
        std::uint32_t current_tail = std::atomic_ref<std::uint32_t>(*cq_tail_).load(std::memory_order_acquire);
        
        if (current_head == current_tail) return 0; 

        std::size_t processed = 0;
        std::size_t max_reap = finished_events_out.size();

        while (current_head != current_tail && processed < max_reap) {
            std::uint32_t index = current_head & cq_mask_;
            const io_uring_cqe& cqe = cqes_[index];

            // Capture BOTH tracking tag and real byte volume from the kernel descriptor slot
            finished_events_out[processed++] = { cqe.user_data, cqe.res };
            current_head++;
        }

        std::atomic_ref<std::uint32_t>(*cq_head_).store(current_head, std::memory_order_release);
        return processed;
    }
};
