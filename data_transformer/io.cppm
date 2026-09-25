module;

#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

export module data_transformer.io;

import data_transformer.types;
import data_transformer.spmc;
import std;

export namespace transformer::io {

    using transformer::types::JsonChunk;
    using transformer::types::LOGICAL_CHUNK_SIZE;
    using transformer::types::CACHE_LINE_SIZE;

    constexpr std::size_t IO_RING_DEPTH = 16; 
    constexpr std::size_t BUFFER_POOL_SIZE = 4; 

    struct alignas(CACHE_LINE_SIZE) IoEngineState {
        struct io_uring ring;
        int file_fd{-1};
        std::uint64_t file_size{0};
        std::uint64_t current_offset{0};
        
        std::uint8_t* raw_buffer_pool{nullptr};
        std::size_t pool_total_bytes{0};

        alignas(CACHE_LINE_SIZE) std::uint32_t active_buffer_mask{0}; 
    };

    /**
     * Allocates hardware-backed hugepages and initializes kernel SQPOLL bypass rings.
     */
    inline int initialize(IoEngineState& state, const char* filepath) noexcept {
        state.file_fd = open(filepath, O_RDONLY | O_DIRECT);
        if (state.file_fd < 0) return -errno;

        struct stat st;
        if (fstat(state.file_fd, &st) < 0) {
            close(state.file_fd);
            return -errno;
        }
        state.file_size = st.st_size;

        state.pool_total_bytes = LOGICAL_CHUNK_SIZE * BUFFER_POOL_SIZE;
        
        void* mem = mmap(nullptr, state.pool_total_bytes, PROT_READ | PROT_WRITE, 
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            close(state.file_fd);
            return -errno;
        }
        state.raw_buffer_pool = static_cast<std::uint8_t*>(mem);
        madvise(state.raw_buffer_pool, state.pool_total_bytes, MADV_HUGEPAGE);

        for (std::size_t i = 0; i < state.pool_total_bytes; i += 4096) {
            state.raw_buffer_pool[i] = 0;
        }

        struct io_uring_params params{};
        params.flags = IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000; 

        int ring_res = io_uring_queue_init_params(IO_RING_DEPTH, &state.ring, &params);
        if (ring_res < 0) {
            munmap(state.raw_buffer_pool, state.pool_total_bytes);
            close(state.file_fd);
            return ring_res; 
        }

        struct iovec iov{ .iov_base = state.raw_buffer_pool, .iov_len = state.pool_total_bytes };
        int reg_res = io_uring_register_buffers(&state.ring, &iov, 1);
        if (reg_res < 0) {
            io_uring_queue_exit(&state.ring);
            munmap(state.raw_buffer_pool, state.pool_total_bytes);
            close(state.file_fd);
            return reg_res;
        }

        state.active_buffer_mask = 0;
        return 0; 
    }

    /**
     * Fully unrolled asynchronous streaming loop using explicit batch reaping.
     */
    inline void stream_file(IoEngineState& io_state, transformer::spmc::SpmcQueueState& queue_state) noexcept {
        std::uint64_t chunk_counter = 0;
        std::size_t active_inflight_requests = 0;

        while (io_state.current_offset < io_state.file_size || active_inflight_requests > 0) {
            
            // Step 1: Submit new reads up to the limit of our 4 buffers
            while (io_state.current_offset < io_state.file_size && active_inflight_requests < IO_RING_DEPTH) {
                std::uint32_t pool_index = chunk_counter % BUFFER_POOL_SIZE;

                std::uint32_t current_mask = __atomic_load_n(&io_state.active_buffer_mask, __ATOMIC_ACQUIRE);
                if ((current_mask & (1U << pool_index)) != 0) {
                    break; 
                }

                std::size_t bytes_to_read = std::min(LOGICAL_CHUNK_SIZE, static_cast<std::size_t>(io_state.file_size - io_state.current_offset));
                struct io_uring_sqe* sqe = io_uring_get_sqe(&io_state.ring);
                if (!sqe) break;

                std::uint8_t* target_buffer = io_state.raw_buffer_pool + (pool_index * LOGICAL_CHUNK_SIZE);
                io_uring_prep_read_fixed(sqe, io_state.file_fd, target_buffer, bytes_to_read, io_state.current_offset, 0);
                
                std::uint64_t embedded_tag = (chunk_counter << 32) | static_cast<std::uint64_t>(pool_index);
                io_uring_sqe_set_data64(sqe, embedded_tag);

                __atomic_fetch_or(&io_state.active_buffer_mask, (1U << pool_index), __ATOMIC_RELEASE);
                io_state.current_offset += bytes_to_read;
                chunk_counter++;
                active_inflight_requests++;
            }

            io_uring_submit(&io_state.ring);

            struct io_uring_cqe* cqe{nullptr};
            unsigned cq_head_ptr;
            unsigned reaped_count = 0;

            if (active_inflight_requests == BUFFER_POOL_SIZE || (io_state.current_offset >= io_state.file_size && active_inflight_requests > 0)) {
                io_uring_wait_cqe(&io_state.ring, &cqe);
            }

            io_uring_for_each_cqe(&io_state.ring, cq_head_ptr, cqe) {
                reaped_count++;
                if (cqe->res >= 0) {
                    std::uint64_t recovered_tag = io_uring_cqe_get_data64(cqe);
                    
                    std::uint32_t completed_pool_index = static_cast<std::uint32_t>(recovered_tag & 0xFFFFFFFFU);
                    std::uint64_t chronological_chunk_id = recovered_tag >> 32;
                    
                    std::uint8_t* completed_buffer = io_state.raw_buffer_pool + (completed_pool_index * LOGICAL_CHUNK_SIZE);
                    
                    JsonChunk parsed_chunk{
                        .buffer_ptr = completed_buffer,
                        .size = static_cast<std::size_t>(cqe->res),
                        .chunk_id = chronological_chunk_id 
                    };
                    if (!transformer::spmc::push(queue_state, parsed_chunk)) {
                        reaped_count--; 
                        break;
                    }
                    // FIXED: Removed the premature bitmask clearing line here!
                    // This bit is now legally owned by the worker threads and cleared only after parsing.
                }
            }

            if (reaped_count > 0) {
                io_uring_cq_advance(&io_state.ring, reaped_count);
                active_inflight_requests = (active_inflight_requests >= reaped_count) ? (active_inflight_requests - reaped_count) : 0;
            }

            asm volatile("pause" ::: "memory");
        }
    }

    inline void shutdown(IoEngineState& state) noexcept {
        if (state.raw_buffer_pool) {
            io_uring_unregister_buffers(&state.ring);
            munmap(state.raw_buffer_pool, state.pool_total_bytes);
        }
        io_uring_queue_exit(&state.ring);
        if (state.file_fd >= 0) close(state.file_fd);
    }

} // namespace transformer::io
