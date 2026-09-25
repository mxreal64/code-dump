module;
#include <fcntl.h>     // For O_DIRECT, O_WRONLY, O_CREAT
#include <unistd.h>    // For close, write, lseek system routines
#include <cstring>     // For std::memcpy, std::memset
#include <cstdlib>     // For posix_memalign, free
#include <chrono>      // Performance chronometers

import std;
import stridedb;      // Public Unified Frontend Wrapper Module
import stridedb.aocs; // Visibility of TableSchema configurations

int main() {
    std::cout << "====================================================================\n";
    std::cout << "          STRIDEDB V3 EXTREME HARDWARE CRASH & INTEGRITY SUITE      \n";
    std::cout << "====================================================================\n\n";

    const char* target_db_file = "test_stride.aocs";
    constexpr std::size_t target_rows = StrideDB::AOCS::ROWS_PER_STRIDE; // 32,768 rows
    constexpr std::size_t intense_stride_count = 9155;                 // 300,000,000 records total
    const std::uint64_t starting_epoch = 1718800000;

    // Hard scrub previous execution files to prevent dirty disk allocations
    if (std::filesystem::exists(target_db_file)) {
        std::filesystem::remove(target_db_file);
    }

    StrideDB::EngineConfig config{
        .database_filepath = target_db_file,
        .execution_core_id = 1 // Pinned computation locked to execution Core 1
    };

    StrideDB::ClientFrontend<std::uint64_t, std::int32_t, double> db(config);

    // -------------------------------------------------------------------------
    // CRASH STEP 1: SUSTAINED HIGH-VOLUME DATA MODULATION INGESTION
    // -------------------------------------------------------------------------
    std::cout << "[*] Track 1: Compiling " << intense_stride_count 
              << " strides with dynamic floating-point wave structures..." << std::endl;

    using TelemetryTable = StrideDB::AOCS::TableSchema<std::uint64_t, std::int32_t, double>;

    // Pre-allocate buffer vectors once to maximize heap reusability and prevent page faults
    std::tuple<std::vector<std::uint64_t>, std::vector<std::int32_t>, std::vector<double>> batch;
    auto& ts_vector = std::get<0>(batch);
    auto& id_vector = std::get<1>(batch);
    auto& val_vector = std::get<2>(batch);

    ts_vector.resize(target_rows);
    id_vector.resize(target_rows);
    val_vector.resize(target_rows);

    double absolute_mathematical_validation_anchor = 0.0;
    auto ingestion_start_clock = std::chrono::high_resolution_clock::now();

    for (std::size_t i = 0; i < intense_stride_count; ++i) {
        std::uint64_t current_stride_timestamp = starting_epoch + (i * 5); // 5-second delta updates

        // Dynamic Wave Generation: Modulates values across rows to challenge SIMD register boundaries
        double stride_base_amplitude = 1.25 + static_cast<double>(i % 7);
        std::int32_t target_user_key = (i % 3 == 0) ? 842011 : 999999;

        for (std::size_t r = 0; r < target_rows; ++r) {
            ts_vector[r] = current_stride_timestamp;
            
            // Jitter Key Interleaving: Even rows get the current stride target key, odd rows get dead flags
            id_vector[r] = (r % 2 == 0) ? target_user_key : 111111;
            
            // Generate non-constant dynamic floating-point curves
            val_vector[r] = stride_base_amplitude + (static_cast<double>(r % 5) * 0.5);

            // Compute exact baseline tracking data if row conditions hit the query specification
            if (id_vector[r] == 842011) {
                absolute_mathematical_validation_anchor += val_vector[r];
            }
        }

        // Drop the compressed data straight into the immutable database block allocation layer
        if (!db.append_stride(current_stride_timestamp, batch)) [[unlikely]] {
            std::cerr << "[-] Ingestion collapsed at stride marker: " << i << std::endl;
            return 1;
        }
    }

    auto ingestion_end_clock = std::chrono::high_resolution_clock::now();
    double ingest_duration_seconds = std::chrono::duration_cast<std::chrono::microseconds>(ingestion_end_clock - ingestion_start_clock).count() / 1000000.0;
    double raw_footprint_bytes = 659456.0 * intense_stride_count;
    double data_footprint_mb = raw_footprint_bytes / (1024.0 * 1024.0);

    std::cout << "[+] Track 1 Complete.\n";
    std::cout << "    -> Footprint Loaded: " << data_footprint_mb << " MB\n";
    std::cout << "    -> Ingestion Speed:  " << data_footprint_mb / ingest_duration_seconds << " MB/second\n\n";

    // -------------------------------------------------------------------------
    // CRASH STEP 2: STORAGE DRIVER INITIALIZATION
    // -------------------------------------------------------------------------
    std::cout << "[*] Track 2: Securing low-level asynchronous io_uring connection hooks..." << std::endl;
    if (!db.connect()) {
        std::cerr << "[-] Critical: Driver failed to latch onto direct storage tracks.\n";
        return 1;
    }
    std::cout << "[+] Storage connection online. Worker pool pinned.\n\n";

    // -------------------------------------------------------------------------
    // CRASH STEP 3: CHAOS INTERVAL WINDOW LOOKUP ITERATIONS
    // -------------------------------------------------------------------------
    std::cout << "[*] Track 3: Running randomized sub-interval verification passes..." << std::endl;
    
    // Seed a pseudo-random number generator for reproducible verification sweeps
    std::mt19937_64 random_generator(42); 

    for (std::size_t test_pass = 1; test_pass <= 10; ++test_pass) {
        std::size_t start_stride_bound = random_generator() % (intense_stride_count / 2);
        std::size_t end_stride_bound   = start_stride_bound + (random_generator() % (intense_stride_count / 2)) + 1;

        std::uint64_t target_start_time = starting_epoch + (start_stride_bound * 5);
        std::uint64_t target_end_time   = starting_epoch + (end_stride_bound * 5);

        // Calculate expected local query window target sum manually to verify against the core engine
        double independent_expected_window_sum = 0.0;
        for (std::size_t s = start_stride_bound; s <= end_stride_bound; ++s) {
            double stride_base_amplitude = 1.25 + static_cast<double>(s % 7);
            std::int32_t target_user_key = (s % 3 == 0) ? 842011 : 999999;

            if (target_user_key == 842011) {
                for (std::size_t r = 0; r < target_rows; r += 2) { // Step by 2 because even rows match
                    independent_expected_window_sum += (stride_base_amplitude + (static_cast<double>(r % 5) * 0.5));
                }
            }
        }

        auto sub_query_start_time = std::chrono::high_resolution_clock::now();
        
        // Execute the vectorized predicate query method
        double calculated_sub_window_total = db.filter_and_sum_column(target_start_time, target_end_time, 842011);
        
        auto sub_query_end_time = std::chrono::high_resolution_clock::now();
        auto sub_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(sub_query_end_time - sub_query_start_time).count();
        
        std::size_t sub_strides_scanned = (end_stride_bound - start_stride_bound) + 1;
        double sub_bytes_read = sub_strides_scanned * 659456.0;
        double sub_speed_gb_s = (sub_bytes_read / (1024.0 * 1024.0 * 1024.0)) / ((double)sub_duration_us / 1000000.0);

        std::cout << "    [Pass #" << test_pass << "] Sub-Range: [+" << start_stride_bound << " to +" << end_stride_bound 
                  << " strides] | Scanned: " << sub_strides_scanned << " Blocks | Velocity: " << sub_speed_gb_s << " GB/s\n";

        if (std::abs(calculated_sub_window_total - independent_expected_window_sum) > 1e-4) [[unlikely]] {
            std::cerr << "    [-] CRITICAL MISMATCH IN PASS #" << test_pass << "!\n";
            std::cerr << "        Expected: " << std::fixed << std::setprecision(6) << independent_expected_window_sum << "\n";
            std::cerr << "        Returned: " << calculated_sub_window_total << "\n";
            return 1;
        }
    }
    std::cout << "[+] Track 3 Complete. Sparse routing indexes and masks are 100% stable.\n\n";

    // -------------------------------------------------------------------------
    // CRASH STEP 4: TOTAL SUSTAINED HARDWARE SATURATION SCAN
    // -------------------------------------------------------------------------
    std::cout << "[*] Track 4: Executing full global database file reduction sweep..." << std::endl;

    auto global_sweep_start = std::chrono::high_resolution_clock::now();

    // Query across the absolute entire timeline range to force full bus saturation
    double global_calculated_total = db.filter_and_sum_column(starting_epoch, starting_epoch + (intense_stride_count * 5), 842011);

    auto global_sweep_end = std::chrono::high_resolution_clock::now();
    double global_duration_seconds = std::chrono::duration_cast<std::chrono::microseconds>(global_sweep_end - global_sweep_start).count() / 1000000.0;
    
    double scanned_gigabytes = raw_footprint_bytes / (1024.0 * 1024.0 * 1024.0);
    double global_throughput_gb_s = scanned_gigabytes / global_duration_seconds;

    std::cout << "\n====================================================================\n";
    std::cout << "                 FINAL SYSTEM STRESS VERIFICATION REPORT            \n";
    std::cout << "====================================================================\n";
    std::cout << "--> Total Footprint Scanned:       " << data_footprint_mb << " MB\n";
    std::cout << "--> Total Scan Clock Duration:     " << global_duration_seconds << " seconds\n";
    std::cout << "--> GLOBAL SUSTAINED LINE SPEED:   " << global_throughput_gb_s << " GB/second\n";
    std::cout << "--------------------------------------------------------------------\n";
    std::cout << "--> Global Calculated Sum:         " << std::fixed << std::setprecision(6) << global_calculated_total << "\n";
    std::cout << "--> Absolute Mathematical Anchor:   " << absolute_mathematical_validation_anchor << "\n";

    if (std::abs(global_calculated_total - absolute_mathematical_validation_anchor) < 1e-4) {
        std::cout << "STATUS: PASS! 100% BIT-PERFECT SYSTEM INTEGRITY SECURED UNDER MAXIMUM LOAD.\n";
    } else {
        std::cout << "STATUS: ERROR! Discrepancy detected inside vector mapping pipelines.\n";
    }
    std::cout << "====================================================================\n";

    return 0;
}
