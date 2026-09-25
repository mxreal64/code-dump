module;
#include <pthread.h>   // Native Linux affinity controls
#include <immintrin.h> // For _mm_pause()

export module stridedb.scheduler;

import std;
import stridedb.memory;
import stridedb.queue;

export namespace StrideDB::Scheduler {

    // The unified task packet (Fits into a single 32-byte cache line slot)
    struct QueryTask {
        std::size_t arena_idx;
        int task_type;          // 0 = Unconditional Sum, 1 = Vectorized Predicate Filter Sum
        std::int32_t match_id;  // The filter key value
    };

    struct alignas(64) WorkerDiagnostics {
        std::atomic<std::uint64_t> total_strides_processed{0};
    };

    inline void pin_thread_to_core(int core_id) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        pthread_t current_thread = pthread_self();
        [[maybe_unused]] int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    }

    class PipelineCoordinator {
    private:
        std::array<Memory::HugePageArena, 16> arenas{};
        
        // Upgraded to hold structured query task packets lock-free
        Queue::LockFreeSPSCQueue<QueryTask, 16> work_queue{};
        Queue::LockFreeSPSCQueue<std::size_t, 16> free_queue{};

        std::atomic<bool> execution_halt{false};
        std::vector<std::thread> worker_pool{};
        alignas(64) WorkerDiagnostics stats{};

    public:
        PipelineCoordinator() noexcept {
            for (std::size_t i = 0; i < 16; ++i) {
                arenas[i].allocate(2 * 1024 * 1024);
                free_queue.push(i); 
            }
        }

        ~PipelineCoordinator() noexcept {
            halt_pipeline();
            for (auto& worker : worker_pool) {
                if (worker.joinable()) worker.join();
            }
        }

        void halt_pipeline() noexcept {
            execution_halt.store(true, std::memory_order_release);
        }

        [[nodiscard]] std::optional<std::size_t> acquire_free_arena_stage() noexcept {
            return free_queue.pop();
        }

        void dispatch_task_to_workers(QueryTask task) noexcept {
            while (!work_queue.push(task)) {
                _mm_pause(); 
            }
        }

        void release_processed_arena(std::size_t arena_idx) noexcept {
            while (!free_queue.push(arena_idx)) {
                _mm_pause();
            }
        }

        [[nodiscard]] void* get_arena_ptr(std::size_t idx) noexcept {
            return arenas[idx].get();
        }

        // Spawns persistent worker loops exactly once
        void initialize_worker_pool(int core_id, auto avx2_compute_lambda) noexcept {
            if (!worker_pool.empty()) return; // Protection: Never double-spawn threads!

            worker_pool.emplace_back([this, core_id, avx2_compute_lambda]() {
                pin_thread_to_core(core_id);

                while (!execution_halt.load(std::memory_order_relaxed)) {
                    auto task_packet = work_queue.pop();
                    if (!task_packet) {
                        _mm_pause(); 
                        continue;
                    }

                    QueryTask active_task = *task_packet;
                    void* huge_page_memory_ptr = arenas[active_task.arena_idx].get();

                    // Delegate execution parameters to the centralized lambda core router
                    avx2_compute_lambda(huge_page_memory_ptr, active_task.task_type, active_task.match_id);

                    stats.total_strides_processed.fetch_add(1, std::memory_order_relaxed);
                    release_processed_arena(active_task.arena_idx);
                }
            });
        }
    };
}
