module;
#include <fcntl.h>     // For O_RDONLY, O_WRONLY, O_CREAT
#include <unistd.h>    // For close, write, lseek system routines
#include <immintrin.h> // For AVX2 intrinsics and _mm_pause()
#include <cstring>     // For std::memcpy, std::memset
#include <cstdlib>     // For posix_memalign, free

export module stridedb;

import std;
import stridedb.aocs;
import stridedb.reader;
import stridedb.io_engine;
import stridedb.scheduler;
import stridedb.query_engine;

export namespace StrideDB {

    struct EngineConfig {
        std::string database_filepath{"test_stride.aocs"};
        std::uint16_t execution_core_id{1}; 
    };

    template <typename... SchemaTypes>
    class [[nodiscard]] ClientFrontend {
    private:
        EngineConfig config{};
        Query::EngineRouter router{};
        Scheduler::PipelineCoordinator scheduler{};
        IO::AsyncIOEngine io_reader{};
        bool system_online{false};

        static double internal_avx2_sum(const double* data, std::size_t count) noexcept {
            [[assume(reinterpret_cast<std::uintptr_t>(data) % 32 == 0)]];

            __m256d acc0 = _mm256_setzero_pd();
            __m256d acc1 = _mm256_setzero_pd();
            __m256d acc2 = _mm256_setzero_pd();
            __m256d acc3 = _mm256_setzero_pd();

            std::size_t i = 0;
            for (; i + 15 < count; i += 16) {
                acc0 = _mm256_add_pd(acc0, _mm256_load_pd(&data[i + 0]));
                acc1 = _mm256_add_pd(acc1, _mm256_load_pd(&data[i + 4]));
                acc2 = _mm256_add_pd(acc2, _mm256_load_pd(&data[i + 8]));
                acc3 = _mm256_add_pd(acc3, _mm256_load_pd(&data[i + 12]));
            }

            __m256d sum_vec = _mm256_add_pd(_mm256_add_pd(acc0, acc1), _mm256_add_pd(acc2, acc3));

            // FIX 1: Correct 4-element destination array allocation
            alignas(32) double buffer[4];
            _mm256_store_pd(buffer, sum_vec);
            double final_sum = buffer[0] + buffer[1] + buffer[2] + buffer[3];

            for (; i < count; ++i) {
                final_sum += data[i];
            }

            return final_sum;
        }

    public:
        explicit ClientFrontend(EngineConfig cfg) noexcept : config(std::move(cfg)) {}

        ClientFrontend(const ClientFrontend&) = delete;
        ClientFrontend& operator=(const ClientFrontend&) = delete;
        ClientFrontend(ClientFrontend&&) noexcept = default;

        alignas(64) std::atomic<double> total_accumulation_bridge{0.0};
        alignas(64) std::atomic<std::int64_t> pending_strides{0};

        [[nodiscard]] bool connect() noexcept {
            if (!io_reader.open_file(config.database_filepath, O_RDONLY)) [[unlikely]] {
                return false;
            }
            system_online = true;
            
            // ONE PERSISTENT WORKER POOL SPIN LAYER ON CORE 1
            scheduler.initialize_worker_pool(config.execution_core_id, [this](void* huge_page_ptr, int task_type, std::int32_t match_id) {
                
                auto sum_span = Reader::StrideDeserializer<SchemaTypes...>::template extract_column_vector<2>(huge_page_ptr);
                const double* sum_data = sum_span.data();
                constexpr std::size_t count = AOCS::ROWS_PER_STRIDE;
                double local_stride_sum = 0.0;

                if (task_type == 0) {
                    local_stride_sum = internal_avx2_sum(sum_data, count);
                } 
                else if (task_type == 1) {
                    auto filter_span = Reader::StrideDeserializer<SchemaTypes...>::template extract_column_vector<1>(huge_page_ptr);
                    const std::int32_t* filter_data = filter_span.data();

                    std::size_t i = 0;
                    for (; i + 7 < count; i += 8) {
                        if (filter_data[i + 0] == match_id) local_stride_sum += sum_data[i + 0];
                        if (filter_data[i + 1] == match_id) local_stride_sum += sum_data[i + 1];
                        if (filter_data[i + 2] == match_id) local_stride_sum += sum_data[i + 2];
                        if (filter_data[i + 3] == match_id) local_stride_sum += sum_data[i + 3];
                        if (filter_data[i + 4] == match_id) local_stride_sum += sum_data[i + 4];
                        if (filter_data[i + 5] == match_id) local_stride_sum += sum_data[i + 5];
                        if (filter_data[i + 6] == match_id) local_stride_sum += sum_data[i + 6];
                        if (filter_data[i + 7] == match_id) local_stride_sum += sum_data[i + 7];
                    }
                    for (; i < count; ++i) {
                        if (filter_data[i] == match_id) local_stride_sum += sum_data[i];
                    }
                }
                
                double current = total_accumulation_bridge.load(std::memory_order_relaxed);
                while (!total_accumulation_bridge.compare_exchange_strong(current, current + local_stride_sum, 
                                                                        std::memory_order_release, 
                                                                        std::memory_order_relaxed)) 
                {
                    _mm_pause();
                }
                
                pending_strides.fetch_sub(1, std::memory_order_release);
            });

            return true;
        }

        bool append_stride(std::uint64_t min_timestamp, const std::tuple<std::vector<SchemaTypes>...>& data_columns) noexcept {
            using TelemetryTable = AOCS::TableSchema<SchemaTypes...>;
            
            std::vector<std::uint8_t> serialized_payload = TelemetryTable::build_aligned_stride(data_columns);
            std::size_t payload_bytes = serialized_payload.size();

            int write_fd = ::open(config.database_filepath.c_str(), O_WRONLY | O_CREAT, 0644);
            if (write_fd < 0) [[unlikely]] return false;
            
            std::uint64_t file_append_offset = ::lseek(write_fd, 0, SEEK_END);
            ::close(write_fd);

            write_fd = ::open(config.database_filepath.c_str(), O_WRONLY | O_DIRECT, 0644);
            if (write_fd < 0) [[unlikely]] return false;
            ::lseek(write_fd, file_append_offset, SEEK_SET);

            void* raw_aligned_mem = nullptr;
            if (::posix_memalign(&raw_aligned_mem, AOCS::SECTOR_SIZE, payload_bytes) != 0) [[unlikely]] {
                ::close(write_fd);
                return false;
            }

            std::memcpy(raw_aligned_mem, serialized_payload.data(), payload_bytes);
            ssize_t written = ::write(write_fd, raw_aligned_mem, payload_bytes);
            ::close(write_fd);
            ::free(raw_aligned_mem);

            if (written < 0) [[unlikely]] return false;

            auto* compiled_header = reinterpret_cast<AOCS::StrideHeader*>(serialized_payload.data());
            
            // FIX 2: Explicit index subscript match for column 2 parameter mappings
            router.register_stride(
                min_timestamp, 
                file_append_offset, 
                compiled_header->total_stride_sectors, 
                compiled_header->column_sectors[2]
            );

            return true;
        }

        // --- RESTORED PUBLIC COORDINATE MANIFEST SYNCHRONIZER ---
        void catalog_stride_offset(std::uint64_t min_timestamp, std::uint64_t file_offset,
                                   std::uint32_t total_secs, const std::array<std::uint32_t, 16>& col_secs) noexcept {
            // FIX 3: Index column array to pass only column 2 size parameters
            router.register_stride(min_timestamp, file_offset, total_secs, col_secs[2]);
        }

        [[nodiscard]] double sum_column(std::uint64_t start_time, std::uint64_t end_time) noexcept {
            if (!system_online) [[unlikely]] return 0.0;
            auto plan = router.plan_range_query(start_time, end_time);
            if (plan.empty()) return 0.0;

            total_accumulation_bridge.store(0.0, std::memory_order_relaxed);
            pending_strides.store(static_cast<std::int64_t>(plan.size()), std::memory_order_release);

            for (const auto& job : plan) {
                std::size_t bytes_to_fetch = job.total_sectors * AOCS::SECTOR_SIZE;
                std::size_t ring_slot = 0;
                while (true) {
                    auto slot = scheduler.acquire_free_arena_stage();
                    if (slot) { ring_slot = *slot; break; }
                    _mm_pause();
                }

                void* huge_page_memory = scheduler.get_arena_ptr(ring_slot);
                io_reader.submit_request(huge_page_memory, bytes_to_fetch, job.physical_file_offset, false, ring_slot);

                while (true) {
                    auto done = io_reader.peek_completion();
                    if (done && *done == ring_slot) break;
                    _mm_pause();
                }

                scheduler.dispatch_task_to_workers(Scheduler::QueryTask{
                    .arena_idx = ring_slot, 
                    .task_type = 0, 
                    .match_id  = 0
                });
            }

            while (pending_strides.load(std::memory_order_acquire) > 0) _mm_pause();
            return total_accumulation_bridge.load(std::memory_order_acquire);
        }

        [[nodiscard]] double filter_and_sum_column(std::uint64_t start_time, std::uint64_t end_time, std::int32_t match_target_id) noexcept {
            if (!system_online) [[unlikely]] return 0.0;
            auto plan = router.plan_range_query(start_time, end_time);
            if (plan.empty()) return 0.0;

            total_accumulation_bridge.store(0.0, std::memory_order_relaxed);
            pending_strides.store(static_cast<std::int64_t>(plan.size()), std::memory_order_release);

            for (const auto& job : plan) {
                std::size_t bytes_to_fetch = job.total_sectors * AOCS::SECTOR_SIZE;
                std::size_t ring_slot = 0;
                while (true) {
                    auto slot = scheduler.acquire_free_arena_stage();
                    if (slot) { 
                        ring_slot = *slot; 
                        break; 
                    }
                    _mm_pause();
                }

                void* huge_page_memory = scheduler.get_arena_ptr(ring_slot);
                io_reader.submit_request(huge_page_memory, bytes_to_fetch, job.physical_file_offset, false, ring_slot);

                while (true) {
                    auto done = io_reader.peek_completion();
                    if (done && *done == ring_slot) break;
                    _mm_pause();
                }

                scheduler.dispatch_task_to_workers(Scheduler::QueryTask{
                    .arena_idx = ring_slot,
                    .task_type = 1,
                    .match_id  = match_target_id
                });
            }

            while (pending_strides.load(std::memory_order_acquire) > 0) {
                _mm_pause();
            }

            return total_accumulation_bridge.load(std::memory_order_acquire);
        }
    };
}
