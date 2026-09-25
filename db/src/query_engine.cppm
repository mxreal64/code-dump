export module stridedb.query_engine;

import std;
import stridedb.aocs; // Access our SECTOR_SIZE and MAGIC markers

export namespace StrideDB::Query {

    // A single lightweight index entry (Fits perfectly into 32 bytes)
    struct alignas(32) IndexEntry {
        std::uint64_t min_timestamp;       // First timestamp in the stride
        std::uint64_t physical_file_offset;// Where the stride starts on disk
        std::uint32_t total_sectors;       // Total sectors for this entire stride
        std::uint32_t col2_sectors;        // Exact sectors taken by Column 2 (for targeted reading)
    };

    class EngineRouter {
    private:
        // A flat, contiguous vector of our sparse index blocks for maximum L1/L2 cache locality
        std::vector<IndexEntry> sparse_index{};

    public:
        EngineRouter() noexcept = default;

        // Register a newly written stride into our query brain
        void register_stride(std::uint64_t timestamp, std::uint64_t file_offset, 
                             std::uint32_t total_secs, std::uint32_t col2_secs) noexcept {
            sparse_index.push_back({
                .min_timestamp = timestamp,
                .physical_file_offset = file_offset,
                .total_sectors = total_secs,
                .col2_sectors = col2_secs
            });
        }

        // THE QUERY PLANNER: Find the exact physical file constraints for a time window
        [[nodiscard]] std::vector<IndexEntry> plan_range_query(std::uint64_t start_time, std::uint64_t end_time) const noexcept {
            std::vector<IndexEntry> matched_strides{};

            if (sparse_index.empty()) return matched_strides;

            // Use std::ranges::lower_bound to perform a binary search over the sparse index (O(log N))
            auto it = std::ranges::lower_bound(sparse_index, start_time, {}, &IndexEntry::min_timestamp);

            // FIX: Only backtrack if we overshot the start_time
			if (it != sparse_index.begin() && (it == sparse_index.end() || it->min_timestamp > start_time)) {
    			--it;
			}


            // Collect all physical stride boundaries that fall into the user's query window
            while (it != sparse_index.end() && it->min_timestamp <= end_time) {
                matched_strides.push_back(*it);
                ++it;
            }

            return matched_strides;
        }

        [[nodiscard]] std::size_t total_indexed_strides() const noexcept {
            return sparse_index.size();
        }
    };
}
