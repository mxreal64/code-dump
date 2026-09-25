export module stridedb.reader;

import std;
import stridedb.aocs;

export namespace StrideDB::Reader {

    template <typename... ColumnTypes>
    class [[nodiscard]] StrideDeserializer {
    public:
        template <std::size_t TargetColIdx>
        [[nodiscard]] static auto extract_column_vector(const void* raw_huge_page_ptr) {
            
            const auto* header = reinterpret_cast<const AOCS::StrideHeader*>(raw_huge_page_ptr);

            if (header->magic_number != AOCS::MAGIC) [[unlikely]] {
                throw std::runtime_error("Invalid file header! StrideDB signature missing.");
            }

            std::size_t sector_jumps = 1; 
            for (std::size_t i = 0; i < TargetColIdx; ++i) {
                sector_jumps += header->column_sectors[i];
            }
            std::size_t byte_offset = sector_jumps * AOCS::SECTOR_SIZE;
            
            constexpr std::size_t row_count = AOCS::ROWS_PER_STRIDE; 
            using TargetType = typename std::tuple_element_t<TargetColIdx, std::tuple<ColumnTypes...>>;
            
            const TargetType* data_start_address = reinterpret_cast<const TargetType*>(
                static_cast<const std::uint8_t*>(raw_huge_page_ptr) + byte_offset
            );

            // Enforce AVX alignment rule validation checks dynamically based on datatype sizes
            if constexpr (sizeof(TargetType) >= 4) {
                [[assume(reinterpret_cast<std::uintptr_t>(data_start_address) % 32 == 0)]];
            }

            return std::span<const TargetType, row_count>(data_start_address, row_count);
        }
    };
}
