module;
#include <parquet/arrow/writer.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/builder.h>

export module data_transformer.parquet;
import data_transformer.types;
import data_transformer.simd;
import std;

export namespace transformer::parquet {
    using namespace transformer::types;

    struct ParquetMetadataTracker {
        std::shared_ptr<arrow::io::FileOutputStream> file_stream;
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<::parquet::arrow::FileWriter> file_writer;
        bool is_initialized{false};
    };

    inline bool initialize_output(ParquetMetadataTracker& tracker, const char* out_filepath) noexcept {
        auto result = arrow::io::FileOutputStream::Open(out_filepath);
        if (!result.ok()) {
            std::println(std::cerr, "Arrow File IO Open Error: {}", result.status().ToString());
            return false;
        }
        tracker.file_stream = result.ValueOrDie();

        arrow::FieldVector fields;
        for (std::size_t i = 0; i < transformer::simd::global_schema_field_count; ++i) {
            const auto& field = transformer::simd::global_schema_lookup[i];
            std::string col_name = "col_" + std::to_string(i);
            
            if (field.type == ColumnType::INT64) {
                fields.push_back(arrow::field(col_name, arrow::int64()));
            } else if (field.type == ColumnType::FLOAT64) {
                fields.push_back(arrow::field(col_name, arrow::float64()));
            } else if (field.type == ColumnType::STRING_VIEW) {
                fields.push_back(arrow::field(col_name, arrow::utf8()));
            }
        }
        tracker.schema = std::make_shared<arrow::Schema>(fields);

        ::parquet::WriterProperties::Builder props_builder;
        props_builder.compression(::parquet::Compression::ZSTD);
        props_builder.compression_level(1);
        auto properties = props_builder.build();

        auto writer_result = ::parquet::arrow::FileWriter::Open(
            *tracker.schema, arrow::default_memory_pool(), 
            tracker.file_stream, properties
        );
        
        if (!writer_result.ok()) {
            std::println(std::cerr, "Arrow Writer Open Error: {}", writer_result.status().ToString());
            return false;
        }
        tracker.file_writer = std::move(writer_result.ValueOrDie());
        tracker.is_initialized = true;
        return true;
    }

    inline void parallel_flush_row_group(ParquetMetadataTracker& tracker, 
                                         RowGroupMemoryBuffer& buffer) noexcept {
        if (!tracker.is_initialized || buffer.total_rows_accumulated == 0) return;

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        
        for (std::size_t col_idx = 0; col_idx < buffer.active_column_count; ++col_idx) {
            ColumnMemorySlice& column = buffer.columns[col_idx];
            
            if (column.type == ColumnType::INT64) {
                arrow::Int64Builder builder;
                auto status = builder.AppendValues(reinterpret_cast<const int64_t*>(column.raw_data_buffer), column.element_count);
                if (!status.ok()) {
                    std::println(std::cerr, "Col {} INT64 Append Fail: {}", col_idx, status.ToString());
                }
                std::shared_ptr<arrow::Array> arr;
                (void)builder.Finish(&arr);
                arrays.push_back(arr);
            } 
            else if (column.type == ColumnType::FLOAT64) {
                arrow::DoubleBuilder builder;
                auto status = builder.AppendValues(reinterpret_cast<const double*>(column.raw_data_buffer), column.element_count);
                if (!status.ok()) {
                    std::println(std::cerr, "Col {} FLOAT64 Append Fail: {}", col_idx, status.ToString());
                }
                std::shared_ptr<arrow::Array> arr;
                (void)builder.Finish(&arr);
                arrays.push_back(arr);
            }
            else if (column.type == ColumnType::STRING_VIEW) {
                arrow::StringBuilder builder;
                std::size_t offset = 0;
                for (std::size_t row = 0; row < column.element_count; ++row) {
                    std::uint32_t len = *reinterpret_cast<const std::uint32_t*>(column.raw_data_buffer + offset);
                    const char* str_ptr = reinterpret_cast<const char*>(column.raw_data_buffer + offset + sizeof(std::uint32_t));
                    auto status = builder.Append(str_ptr, static_cast<int32_t>(len));
                    if (!status.ok()) {
                        std::println(std::cerr, "Col {} STR Append Row {} Fail: {}", col_idx, row, status.ToString());
                    }
                    offset += sizeof(std::uint32_t) + len;
                }
                std::shared_ptr<arrow::Array> arr;
                (void)builder.Finish(&arr);
                arrays.push_back(arr);
            }

            column.bytes_written = 0;
            column.element_count = 0;
        }

        auto table = arrow::Table::Make(tracker.schema, arrays, static_cast<int64_t>(buffer.total_rows_accumulated));
        auto write_status = tracker.file_writer->WriteTable(*table, static_cast<int64_t>(buffer.total_rows_accumulated));
        
        if (!write_status.ok()) {
            std::println(std::cerr, "Arrow WriteTable Fatal Failure: {}", write_status.ToString());
        }
        buffer.total_rows_accumulated = 0;
    }

    inline void finalize_parquet_file(ParquetMetadataTracker& tracker) noexcept {
        if (tracker.file_writer) {
            auto status = tracker.file_writer->Close();
            if (!status.ok()) std::println(std::cerr, "Arrow Writer Close Error: {}", status.ToString());
        }
        if (tracker.file_stream) {
            auto status = tracker.file_stream->Close();
            if (!status.ok()) std::println(std::cerr, "Arrow Stream Close Error: {}", status.ToString());
        }
    }
} // namespace transformer::parquet
