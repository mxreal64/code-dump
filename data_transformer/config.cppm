module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

export module data_transformer.config;

import data_transformer.types;
import data_transformer.simd;
import std;

export namespace transformer::config {

    using namespace transformer::types;

    /**
     * Loads schema.json and builds the global runtime schema mapping layout in memory.
     * Returns: 0 on success, or a negative integer corresponding to the failure type.
     */
    inline int parse_runtime_schema(const char* config_path) noexcept {
        int fd = open(config_path, O_RDONLY);
        if (fd < 0) return -1;

        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            return -2;
        }

        // FIXED: Protects against zero/negative sizing and clears the [-Wstringop-overflow] warning
        if (st.st_size <= 0) {
            close(fd);
            return -9;
        }

        const std::size_t file_size = static_cast<std::size_t>(st.st_size);
        auto buffer = std::make_unique<char[]>(file_size);
        
        if (read(fd, buffer.get(), file_size) != static_cast<ssize_t>(file_size)) {
            close(fd);
            return -3;
        }
        close(fd);

        std::size_t cursor = 0;
        std::size_t field_idx = 0;
        
        constexpr std::uint32_t HASH_TYPE_INT   = transformer::simd::hash_key_compile_time("INT64");
        constexpr std::uint32_t HASH_TYPE_FLOAT = transformer::simd::hash_key_compile_time("FLOAT64");
        constexpr std::uint32_t HASH_TYPE_STR   = transformer::simd::hash_key_compile_time("STRING_VIEW");

        while (cursor < file_size) {
            if (buffer[cursor] == '"') {
                std::size_t key_start = cursor + 1;
                std::size_t key_end = key_start;
                
                // SAFE BOUNDS CHECK: Make sure key_end never hits or passes file_size
                while (key_end < file_size && buffer[key_end] != '"') key_end++;
                if (key_end >= file_size) return -4; // Corrupted JSON shape
                
                std::uint32_t key_hash = transformer::simd::hash_key_runtime(
                    reinterpret_cast<const std::uint8_t*>(&buffer[key_start]), key_end - key_start
                );
                
                cursor = key_end + 1;
                
                // SAFE BOUNDS CHECK: Walk ahead ensuring we don't shoot out of the block
                while (cursor < file_size && buffer[cursor] != ':') cursor++;
                if (cursor >= file_size) return -5;
                
                while (cursor < file_size && buffer[cursor] != '"') cursor++;
                if (cursor >= file_size) return -6;
                
                std::size_t type_start = cursor + 1;
                std::size_t type_end = type_start;
                
                while (type_end < file_size && buffer[type_end] != '"') type_end++;
                if (type_end >= file_size) return -7;
                
                std::uint32_t type_hash = transformer::simd::hash_key_runtime(
                    reinterpret_cast<const std::uint8_t*>(&buffer[type_start]), type_end - type_start
                );

                ColumnType col_type = ColumnType::INT64;
                if (type_hash == HASH_TYPE_INT)        col_type = ColumnType::INT64;
                else if (type_hash == HASH_TYPE_FLOAT) col_type = ColumnType::FLOAT64;
                else if (type_hash == HASH_TYPE_STR)   col_type = ColumnType::STRING_VIEW;

                // CRITICAL BOUNDS GUARD: Check capacity BEFORE writing to the array index
                if (field_idx >= MAX_COLUMNS_PER_SCHEMA) [[unlikely]] return -8;

                transformer::simd::global_schema_lookup[field_idx] = {
                    .key_hash = key_hash,
                    .type = col_type,
                    .target_column_index = static_cast<std::uint32_t>(field_idx)
                };
                
                field_idx++;
                cursor = type_end + 1;
                continue; // Skip the bottom increment step since we're manually jumping ahead
            }
            cursor++;
        }
        
        transformer::simd::global_schema_field_count = field_idx;
        return 0;
    }

} // namespace transformer::config
