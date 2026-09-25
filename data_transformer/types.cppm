export module data_transformer.types;

import std;

export namespace transformer::types {

    constexpr std::size_t CACHE_LINE_SIZE = 64;
    constexpr std::size_t HUGEPAGE_2MB = 2 * 1024 * 1024;
    constexpr std::size_t LOGICAL_CHUNK_SIZE = 64 * 1024 * 1024; 

    constexpr std::size_t MAX_COLUMNS_PER_SCHEMA = 16;         
    constexpr std::size_t MAX_ROWS_PER_ROW_GROUP = 512'000;    

    enum class ColumnType : std::uint8_t {
        INT64       = 0,
        FLOAT64     = 1,
        STRING_VIEW = 2
    };

    // Runtime metadata token used to route hashes dynamically
    struct SchemaFieldDefinition {
        std::uint32_t key_hash{0};
        ColumnType type{ColumnType::INT64};
        std::uint32_t target_column_index{0};
    };

    struct alignas(CACHE_LINE_SIZE) JsonChunk {
        std::uint8_t* buffer_ptr{nullptr};
        std::size_t size{0};
        std::uint64_t chunk_id{0};
    };

    struct alignas(CACHE_LINE_SIZE) ColumnMemorySlice {
        std::uint8_t* raw_data_buffer{nullptr};
        std::size_t bytes_written{0};
        std::size_t max_capacity_bytes{0};
        std::size_t element_count{0};
        ColumnType type{ColumnType::INT64};
    };

    struct alignas(CACHE_LINE_SIZE) RowGroupMemoryBuffer {
        ColumnMemorySlice columns[MAX_COLUMNS_PER_SCHEMA];
        std::size_t active_column_count{0};
        std::size_t total_rows_accumulated{0};
    };

} // namespace transformer::types
