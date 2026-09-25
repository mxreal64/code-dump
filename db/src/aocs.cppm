export module stridedb.aocs;

import std;

export namespace StrideDB::AOCS {

    constexpr std::size_t SECTOR_SIZE = 4096;
    constexpr std::uint32_t MAGIC = 0x54524944; // "TRID"
    constexpr std::size_t ROWS_PER_STRIDE = 32768;

    // Packed structural layout taking up exactly 1 full sector
    struct alignas(SECTOR_SIZE) StrideHeader {
        std::uint32_t magic_number{MAGIC};
        std::uint32_t column_count{0};
        std::uint32_t total_stride_sectors{0};
        
        // Byte-aligned delta compression trackers
        std::uint64_t timestamp_anchor{0};
        std::uint32_t timestamp_compressed_bytes{0};

        std::array<std::uint32_t, 16> compressed_sizes{};
        std::array<std::uint32_t, 16> column_sectors{};

        std::array<std::uint8_t, SECTOR_SIZE - (sizeof(std::uint32_t) * 3) - sizeof(std::uint64_t) - (sizeof(std::uint32_t) * 33)> padding{};
    };

    [[nodiscard]] constexpr std::size_t bytes_to_sectors(std::size_t bytes) noexcept {
        return (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    }

    [[nodiscard]] constexpr std::size_t pad_to_sector(std::size_t bytes) noexcept {
        return bytes_to_sectors(bytes) * SECTOR_SIZE;
    }

    template <typename... ColumnTypes>
    class TableSchema {
    public:
        static constexpr std::size_t col_count = sizeof...(ColumnTypes);

        [[nodiscard]] static std::vector<std::uint8_t> build_aligned_stride(
            const std::tuple<std::vector<ColumnTypes>...>& raw_columns) 
        {
            StrideHeader header{};
            header.column_count = static_cast<std::uint32_t>(col_count);

            std::vector<std::uint8_t> aligned_disk_buffer;
            aligned_disk_buffer.resize(SECTOR_SIZE);

            std::size_t current_column_idx = 0;

            auto process_column = [&](const auto& raw_vector) {
                std::size_t raw_bytes = raw_vector.size() * sizeof(typename std::decay_t<decltype(raw_vector)>::value_type);
                
                std::size_t compressed_bytes = raw_bytes; // Un-truncated floats pass straight through
                std::size_t padded_bytes = pad_to_sector(compressed_bytes);
                std::size_t sector_allocation = bytes_to_sectors(compressed_bytes);

                header.compressed_sizes[current_column_idx] = static_cast<std::uint32_t>(compressed_bytes);
                header.column_sectors[current_column_idx] = static_cast<std::uint32_t>(sector_allocation);

                std::size_t old_size = aligned_disk_buffer.size();
                aligned_disk_buffer.resize(old_size + padded_bytes);
                std::fill(aligned_disk_buffer.begin() + old_size, aligned_disk_buffer.end(), 0);

                std::span<const std::uint8_t> source_span(reinterpret_cast<const std::uint8_t*>(raw_vector.data()), compressed_bytes);
                std::ranges::copy(source_span, aligned_disk_buffer.begin() + old_size);

                current_column_idx++;
            };

            std::apply([&](const auto&... args) { (process_column(args), ...); }, raw_columns);

            std::size_t total_sectors = 1;
            for(std::size_t i = 0; i < col_count; ++i) {
                total_sectors += header.column_sectors[i];
            }
            header.total_stride_sectors = static_cast<std::uint32_t>(total_sectors);

            std::span<const std::uint8_t> header_span(reinterpret_cast<const std::uint8_t*>(&header), sizeof(StrideHeader));
            std::ranges::copy(header_span, aligned_disk_buffer.begin());

            return aligned_disk_buffer;
        }
    };
}
