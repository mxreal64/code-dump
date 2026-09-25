module;

// Global Module Fragment handles legacy C macro bindings cleanly
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

export module stridedb.io_engine;

import std;

export namespace StrideDB::IO {

    class [[nodiscard]] AsyncIOEngine {
    private:
        io_uring ring{};
        int file_fd{-1};
        bool initialized{false};

    public:
        AsyncIOEngine() noexcept = default;

        // Delete copies to protect raw hardware queue integrity
        AsyncIOEngine(const AsyncIOEngine&) = delete;
        AsyncIOEngine& operator=(const AsyncIOEngine&) = delete;
        
        AsyncIOEngine(AsyncIOEngine&& other) noexcept 
            : ring(other.ring), file_fd(other.file_fd), initialized(other.initialized) 
        {
            other.initialized = false;
            other.file_fd = -1;
        }

        ~AsyncIOEngine() noexcept {
            if (initialized) [[likely]] {
                ::io_uring_queue_exit(&ring);
            }
            if (file_fd != -1) {
                ::close(file_fd);
            }
        }

        // Open target file track with strict O_DIRECT execution rules
        bool open_file(const std::string& path, int flags) noexcept {
            // Force strict caching bypass rules natively
            file_fd = ::open(path.c_str(), flags | O_DIRECT, 0644);
            if (file_fd < 0) [[unlikely]] return false;

            // Initialize the ring with 256 depth slots and Kernel Thread Polling (SQPOLL)
            int ret = ::io_uring_queue_init(256, &ring, IORING_SETUP_SQPOLL);
            if (ret < 0) [[unlikely]] {
                ::close(file_fd);
                file_fd = -1;
                return false;
            }

            initialized = true;
            return true;
        }

        // Push a non-blocking asynchronous hardware operation to the kernel loop
        bool submit_request(void* aligned_buffer, std::size_t bytes, std::uint64_t offset, bool is_write, std::uint64_t request_tag) noexcept {
            if (!initialized) [[unlikely]] return false;

            io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
            if (!sqe) [[unlikely]] return false; // Write backpressure triggered (Queue full)

            if (is_write) {
                ::io_uring_prep_write(sqe, file_fd, aligned_buffer, static_cast<unsigned int>(bytes), offset);
            } else {
                ::io_uring_prep_read(sqe, file_fd, aligned_buffer, static_cast<unsigned int>(bytes), offset);
            }

            // Bind our identifier tag so we know exactly what block chunk finishes later
            ::io_uring_sqe_set_data64(sqe, request_tag);
            
            // Let it rip! (SQPOLL background kernel thread instantly pulls this without a syscall)
            ::io_uring_submit(&ring);
            return true;
        }

        // Poll the completion ring non-blocking (returns our user tag if an I/O task completed)
        [[nodiscard]] std::optional<std::uint64_t> peek_completion() noexcept {
            io_uring_cqe* cqe{nullptr};
            
            int ret = ::io_uring_peek_cqe(&ring, &cqe);
            if (ret == 0 && cqe) {
                std::uint64_t completed_tag = ::io_uring_cqe_get_data64(cqe);
                
                // Release the completion slot back to the ring
                ::io_uring_cqe_seen(&ring, cqe);
                return completed_tag;
            }
            return std::nullopt;
        }
    };
}
