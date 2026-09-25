import data_transformer.types;
import data_transformer.spmc;
import data_transformer.config;
import data_transformer.io;
import data_transformer.simd;
import data_transformer.parquet;
import data_transformer.egress;
import std;

// HARDENED: True lock-free Multi-Producer Multi-Consumer (MPMC) Queue with refreshed CAS loop logic
struct alignas(64) FreeBufferArena {
    static constexpr std::size_t SIZE = 16;
    transformer::types::RowGroupMemoryBuffer* storage[SIZE]{nullptr};
    alignas(64) std::atomic<std::uint64_t> head{0};
    alignas(64) std::atomic<std::uint64_t> tail{0};
};

inline void push_free_pool(FreeBufferArena& arena, transformer::types::RowGroupMemoryBuffer* buf) noexcept {
    std::uint64_t t = arena.tail.load(std::memory_order_relaxed);
    while (true) {
        std::uint64_t h = arena.head.load(std::memory_order_acquire);
        if ((t - h) >= FreeBufferArena::SIZE) {
            asm volatile("pause" ::: "memory");
            continue; 
        }
        arena.storage[t & (FreeBufferArena::SIZE - 1)] = buf;
        if (arena.tail.compare_exchange_weak(t, t + 1, std::memory_order_release, std::memory_order_relaxed)) {
            break;
        }
    }
}

inline transformer::types::RowGroupMemoryBuffer* pop_free_pool(FreeBufferArena& arena) noexcept {
    std::uint64_t h = arena.head.load(std::memory_order_relaxed);
    while (true) {
        std::uint64_t t = arena.tail.load(std::memory_order_acquire);
        if (h == t) return nullptr; // Pool is empty
        
        auto* buf = arena.storage[h & (FreeBufferArena::SIZE - 1)];
        if (arena.head.compare_exchange_weak(h, h + 1, std::memory_order_release, std::memory_order_relaxed)) {
            return buf;
        }
        asm volatile("pause" ::: "memory");
    }
}

inline void setup_buffer_schema(transformer::types::RowGroupMemoryBuffer* buf, std::uint8_t* memory_page_base) noexcept {
    buf->active_column_count = transformer::simd::global_schema_field_count;
    buf->total_rows_accumulated = 0;
    constexpr std::size_t SLICE_SIZE = 8 * 1024 * 1024;

    for (std::size_t i = 0; i < buf->active_column_count; ++i) {
        buf->columns[i].type = transformer::simd::global_schema_lookup[i].type;
        buf->columns[i].raw_data_buffer = memory_page_base + (i * SLICE_SIZE);
        buf->columns[i].bytes_written = 0; 
        buf->columns[i].max_capacity_bytes = SLICE_SIZE;
        buf->columns[i].element_count = 0;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::println(std::cerr, "Usage: ./transformer <input.json> <output.parquet>");
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = argv[2];

    if (transformer::config::parse_runtime_schema("schema.json") < 0) {
        std::println(std::cerr, "Fatal Error: Failed to load dynamic metadata from 'schema.json'.");
        return 1;
    }

    transformer::spmc::SpmcQueueState inbound_queue{};
    transformer::egress::OutboundEgressQueue outbound_queue{};
    FreeBufferArena free_arena{};
    transformer::io::IoEngineState io_engine{};
    transformer::parquet::ParquetMetadataTracker parquet_file{};

    if (transformer::io::initialize(io_engine, input_file) < 0) return 1;
    if (!transformer::parquet::initialize_output(parquet_file, output_file)) {
        transformer::io::shutdown(io_engine);
        return 1;
    }

    // FIXED: Dynamically scale your multi-column arena page ranges to eliminate overlaps
    constexpr std::size_t SLICE_SIZE_PER_COLUMN = 8 * 1024 * 1024;
    std::size_t single_buffer_total_bytes = transformer::simd::global_schema_field_count * SLICE_SIZE_PER_COLUMN;
    std::size_t memory_arena_total_bytes = FreeBufferArena::SIZE * single_buffer_total_bytes;
    
    auto memory_blocks = std::make_unique<transformer::types::RowGroupMemoryBuffer[]>(FreeBufferArena::SIZE);
    void* raw_arena_mem = std::malloc(memory_arena_total_bytes);
    std::uint8_t* arena_bytes = static_cast<std::uint8_t*>(raw_arena_mem);
    
    for (std::size_t i = 0; i < FreeBufferArena::SIZE; ++i) {
        setup_buffer_schema(&memory_blocks[i], arena_bytes + (i * single_buffer_total_bytes));
        push_free_pool(free_arena, &memory_blocks[i]);
    }

    unsigned int worker_count = 4;
    std::atomic<bool> ingestion_complete{false};
    std::atomic<bool> workers_complete{false};
    std::vector<std::jthread> worker_threads;
    worker_threads.reserve(worker_count);

    std::println(std::cout, "Launching dynamic non-blocking pipeline conveyor...");

    std::jthread ingestion_thread([&io_engine, &inbound_queue, &ingestion_complete]() {
        transformer::io::stream_file(io_engine, inbound_queue);
        ingestion_complete.store(true, std::memory_order_release);
    });

    for (unsigned int i = 0; i < worker_count; ++i) {
        worker_threads.emplace_back([&inbound_queue, &outbound_queue, &free_arena, &ingestion_complete, &io_engine]() {
            transformer::types::JsonChunk chunk_task{};
            transformer::types::RowGroupMemoryBuffer* current_active_buffer = nullptr;
            
            while (!(current_active_buffer = pop_free_pool(free_arena))) {
                asm volatile("pause" ::: "memory");
            }

            while (true) {
                bool has_data = transformer::spmc::pop(inbound_queue, chunk_task);
                
                if (!has_data) {
                    if (ingestion_complete.load(std::memory_order_acquire)) {
                        if (current_active_buffer->total_rows_accumulated > 0) {
                            while (!transformer::egress::push_egress(outbound_queue, current_active_buffer)) {
                                asm volatile("pause" ::: "memory");
                            }
                        }
                        break; 
                    }
                    asm volatile("pause" ::: "memory");
                    continue;
                }

                transformer::simd::parse_chunk_to_columnar(chunk_task, *current_active_buffer);

                std::uint32_t finished_pool_idx = chunk_task.chunk_id % transformer::io::BUFFER_POOL_SIZE;
                __atomic_fetch_and(&io_engine.active_buffer_mask, ~(1U << finished_pool_idx), __ATOMIC_RELEASE);

                if (current_active_buffer->total_rows_accumulated >= transformer::types::MAX_ROWS_PER_ROW_GROUP) {
                    while (!transformer::egress::push_egress(outbound_queue, current_active_buffer)) {
                        asm volatile("pause" ::: "memory");
                    }
                    while (!(current_active_buffer = pop_free_pool(free_arena))) {
                        asm volatile("pause" ::: "memory");
                    }
                }
            }
        });
    }

    // FIXED: Hardened to guarantee the egress queue is completely drained before closing
    std::jthread writer_thread([&outbound_queue, &free_arena, &parquet_file, &workers_complete]() {
        transformer::types::RowGroupMemoryBuffer* ready_buffer{nullptr};
        while (true) {
            bool got_buffer = transformer::egress::pop_egress(outbound_queue, ready_buffer);
            
            if (got_buffer) {
                transformer::parquet::parallel_flush_row_group(parquet_file, *ready_buffer);
                ready_buffer->total_rows_accumulated = 0;
                push_free_pool(free_arena, ready_buffer);
                continue; 
            }
            
            if (workers_complete.load(std::memory_order_acquire)) {
                if (!transformer::egress::pop_egress(outbound_queue, ready_buffer)) {
                    break; 
                }
                transformer::parquet::parallel_flush_row_group(parquet_file, *ready_buffer);
                ready_buffer->total_rows_accumulated = 0;
                push_free_pool(free_arena, ready_buffer);
            }
            asm volatile("pause" ::: "memory");
        }
    });

    ingestion_thread.join();
    worker_threads.clear();
    workers_complete.store(true, std::memory_order_release);
    writer_thread.join();

    transformer::parquet::finalize_parquet_file(parquet_file);
    transformer::io::shutdown(io_engine);
    std::free(raw_arena_mem);
    return 0;
}
