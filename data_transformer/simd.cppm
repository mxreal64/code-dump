module;
#include <immintrin.h>
#include <cstring>
export module data_transformer.simd;
import data_transformer.types;
import std;
export namespace transformer::simd {
    using namespace transformer::types;
    alignas(64) SchemaFieldDefinition global_schema_lookup[MAX_COLUMNS_PER_SCHEMA];
    alignas(64) std::size_t global_schema_field_count = 0;
    inline std::uint32_t compute_prefix_xor(std::uint32_t mask) noexcept {
        mask ^= mask << 1;
        mask ^= mask << 2;
        mask ^= mask << 4;
        mask ^= mask << 8;
        mask ^= mask << 16;
        return mask;
    }
    inline std::uint32_t find_structural_bits(const std::uint8_t* block, char target) noexcept {
        const __m256i target_vec = _mm256_set1_epi8(target);
        const __m256i data_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block));
        const __m256i cmp_result = _mm256_cmpeq_epi8(data_vec, target_vec);
        return static_cast<std::uint32_t>(_mm256_movemask_epi8(cmp_result));
    }
    inline std::uint32_t hash_key_runtime(const std::uint8_t* data, std::size_t len) noexcept {
        std::uint32_t hash = 2166136261U;
        for (std::size_t i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 16777619U;
        }
        return hash;
    }
    template<std::size_t N>
    inline constexpr std::uint32_t hash_key_compile_time(const char (&str)[N]) noexcept {
        std::uint32_t hash = 2166136261U;
        for (std::size_t i = 0; i < N - 1; ++i) {
            hash ^= static_cast<std::uint8_t>(str[i]);
            hash *= 16777619U;
        }
        return hash;
    }
    inline std::int64_t parse_int64(const std::uint8_t* ptr, const std::uint8_t* end) noexcept {
        std::int64_t val = 0;
        while (ptr < end && (*ptr == ' ' || *ptr == ':')) ptr++;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            val = val * 10 + (*ptr - '0');
            ptr++;
        }
        return val;
    }
    inline double parse_double(const std::uint8_t* ptr, const std::uint8_t* end) noexcept {
        while (ptr < end && (*ptr == ' ' || *ptr == ':')) ptr++;
        double value = 0.0;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            value = value * 10.0 + (*ptr - '0');
            ptr++;
        }
        if (ptr < end && *ptr == '.') {
            ptr++;
            double weight = 0.1;
            while (ptr < end && *ptr >= '0' && *ptr <= '9') {
                value += (*ptr - '0') * weight;
                weight *= 0.1;
                ptr++;
            }
        }
        return value;
    }
    inline void append_string_to_column(ColumnMemorySlice& slice, const std::uint8_t* str_ptr, std::uint32_t len) noexcept {
        const std::size_t required_bytes = sizeof(std::uint32_t) + len;
        if (slice.bytes_written + required_bytes > slice.max_capacity_bytes) [[unlikely]] return;
        std::memcpy(slice.raw_data_buffer + slice.bytes_written, &len, sizeof(std::uint32_t));
        slice.bytes_written += sizeof(std::uint32_t);
        std::memcpy(slice.raw_data_buffer + slice.bytes_written, str_ptr, len);
        slice.bytes_written += len;
        slice.element_count++;
    }
    inline void append_int64_t_to_column(ColumnMemorySlice& slice, std::int64_t value) noexcept {
        if (slice.bytes_written + sizeof(std::int64_t) > slice.max_capacity_bytes) [[unlikely]] return;
        std::memcpy(slice.raw_data_buffer + slice.bytes_written, &value, sizeof(std::int64_t));
        slice.bytes_written += sizeof(std::int64_t);
        slice.element_count++;
    }
    inline void append_double_to_column(ColumnMemorySlice& slice, double value) noexcept {
        if (slice.bytes_written + sizeof(double) > slice.max_capacity_bytes) [[unlikely]] return;
        std::memcpy(slice.raw_data_buffer + slice.bytes_written, &value, sizeof(double));
        slice.bytes_written += sizeof(double);
        slice.element_count++;
    }
    inline void process_single_row(const std::uint8_t* row_ptr, std::size_t row_len, RowGroupMemoryBuffer& target_buffer) noexcept {
        if (row_len < 5) return;
        std::size_t i = 0;
        while (i < row_len) {
            if (row_ptr[i] == '"') {
                std::size_t key_start = i + 1;
                std::size_t key_end = key_start;
                while (key_end < row_len && row_ptr[key_end] != '"') key_end++;
                std::uint32_t extracted_key_hash = hash_key_runtime(row_ptr + key_start, key_end - key_start);
                i = key_end + 1;
                while (i < row_len && row_ptr[i] != ':') i++;
                if (i >= row_len) break;
                i++;
                while (i < row_len && row_ptr[i] == ' ') i++;
                for (std::size_t lookup_idx = 0; lookup_idx < global_schema_field_count; ++lookup_idx) {
                    const auto& field = global_schema_lookup[lookup_idx];
                    if (field.key_hash == extracted_key_hash) [[likely]] {
                        auto& col = target_buffer.columns[field.target_column_index];
                        if (field.type == ColumnType::INT64) {
                            std::size_t val_end = i;
                            while (val_end < row_len && row_ptr[val_end] >= '0' && row_ptr[val_end] <= '9') val_end++;
                            append_int64_t_to_column(col, parse_int64(row_ptr + i, row_ptr + val_end));
                            i = val_end;
                        } 
                        else if (field.type == ColumnType::FLOAT64) {
                            std::size_t val_end = i;
                            while (val_end < row_len && ((row_ptr[val_end] >= '0' && row_ptr[val_end] <= '9') || row_ptr[val_end] == '.')) val_end++;
                            append_double_to_column(col, parse_double(row_ptr + i, row_ptr + val_end));
                            i = val_end;
                        } 
                        else if (field.type == ColumnType::STRING_VIEW) {
                            if (row_ptr[i] == '"') {
                                std::size_t val_start = i + 1;
                                std::size_t val_end = val_start;
                                while (val_end < row_len && row_ptr[val_end] != '"') val_end++;
                                append_string_to_column(col, row_ptr + val_start, val_end - val_start);
                                i = val_end + 1;
                            }
                        }
                        break;
                    }
                }
            }
            i++;
        }
    }
    inline void parse_chunk_to_columnar(const JsonChunk& chunk, RowGroupMemoryBuffer& target_buffer) noexcept {
        const std::uint8_t* const base_ptr = chunk.buffer_ptr;
        const std::size_t chunk_length = chunk.size;
        std::size_t current_index = 0;
        std::size_t last_row_start_index = 0;
        std::uint32_t prev_in_string_mask = 0;
        while (current_index + 32 <= chunk_length) {
            const std::uint8_t* current_block = base_ptr + current_index;
            std::uint32_t quote_mask   = find_structural_bits(current_block, '"');
            std::uint32_t newline_mask = find_structural_bits(current_block, '\n');
            std::uint32_t quote_prefix_xor = compute_prefix_xor(quote_mask);
            std::uint32_t in_string_mask = quote_prefix_xor ^ prev_in_string_mask;
            std::uint32_t last_bit_state = (in_string_mask >> 31) & 1U;
            prev_in_string_mask = 0U - last_bit_state;
            newline_mask &= ~in_string_mask;
            if (newline_mask == 0) {
                current_index += 32;
                continue;
            }
            while (newline_mask != 0) {
                std::uint32_t relative_offset = static_cast<std::uint32_t>(__builtin_ctz(newline_mask));
                std::size_t absolute_newline_pos = current_index + relative_offset;
                std::size_t current_row_len = absolute_newline_pos - last_row_start_index;
                process_single_row(base_ptr + last_row_start_index, current_row_len, target_buffer);
                target_buffer.total_rows_accumulated++;
                last_row_start_index = absolute_newline_pos + 1;
                std::uint32_t next_mask = newline_mask & (newline_mask - 1);
                if (next_mask >= newline_mask) [[unlikely]] break;
                newline_mask = next_mask;
            }
            current_index += 32;
        }
        while (current_index < chunk_length) {
            if (base_ptr[current_index] == '\n') {
                std::size_t current_row_len = current_index - last_row_start_index;
                process_single_row(base_ptr + last_row_start_index, current_row_len, target_buffer);
                target_buffer.total_rows_accumulated++;
                last_row_start_index = current_index + 1;
            }
            current_index++;
        }
    }
}
