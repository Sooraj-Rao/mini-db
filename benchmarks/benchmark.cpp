#include "storage.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main(int argc, char* argv[]) {
    std::size_t count = 10000;
    if (argc > 1) {
        try {
            count = static_cast<std::size_t>(std::stoull(argv[1]));
        } catch (...) {
            std::cerr << "Usage: minidb_bench [record_count]\n";
            return 1;
        }
    }
    if (count == 0) {
        std::cerr << "Record count must be greater than zero.\n";
        return 1;
    }

    const std::filesystem::path data_dir("data");
    std::filesystem::create_directories(data_dir);
    const auto individual_path = data_dir / "minidb_benchmark_individual.db";
    const auto batch_path = data_dir / "minidb_benchmark_batch.db";

    for (const auto& path : {individual_path, batch_path}) {
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
        std::filesystem::remove(path.string() + ".compact.tmp");
        std::filesystem::remove(path.string() + ".compact.bak");
    }

    try {
        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            entries.emplace_back("key-" + std::to_string(i),
                                 "value-" + std::to_string(i));
        }

        double individual_write_ms = 0.0;
        {
            minidb::Storage db(individual_path);
            const auto start = Clock::now();
            for (const auto& [key, value] : entries) {
                db.set(key, value);
            }
            individual_write_ms = elapsed_ms(start, Clock::now());
        }

        double batch_write_ms = 0.0;
        std::size_t successful_reads = 0;
        std::uintmax_t size_before_compaction = 0;
        std::uintmax_t size_after_compaction = 0;
        double read_ms = 0.0;
        double compaction_ms = 0.0;

        {
            minidb::Storage db(batch_path);
            const auto write_start = Clock::now();
            db.set_many(entries);
            batch_write_ms = elapsed_ms(write_start, Clock::now());

            const auto read_start = Clock::now();
            for (std::size_t i = 0; i < count; ++i) {
                if (db.get("key-" + std::to_string(i)).has_value()) {
                    ++successful_reads;
                }
            }
            read_ms = elapsed_ms(read_start, Clock::now());

            // Add obsolete versions and tombstones so compaction has real work.
            std::vector<std::pair<std::string, std::string>> updates;
            updates.reserve(count / 2);
            for (std::size_t i = 0; i < count / 2; ++i) {
                updates.emplace_back("key-" + std::to_string(i),
                                     "updated-" + std::to_string(i));
            }
            db.set_many(updates);

            for (std::size_t i = count / 2; i < count; i += 2) {
                db.remove("key-" + std::to_string(i));
            }

            size_before_compaction = db.file_size();
            const auto compact_start = Clock::now();
            db.compact();
            compaction_ms = elapsed_ms(compact_start, Clock::now());
            size_after_compaction = db.file_size();
        }

        std::cout << "MiniDB benchmark\n"
                  << "Records in initial workload: " << count << '\n'
                  << std::fixed << std::setprecision(2)
                  << "\nWrite comparison:\n"
                  << "Individual SET time: " << individual_write_ms << " ms\n"
                  << "Individual SET throughput: "
                  << (count * 1000.0 / individual_write_ms) << " ops/sec\n"
                  << "Batch SET time: " << batch_write_ms << " ms\n"
                  << "Batch SET throughput: "
                  << (count * 1000.0 / batch_write_ms) << " ops/sec\n"
                  << "Batch speedup: "
                  << (individual_write_ms / batch_write_ms) << "x\n"
                  << "\nRead test:\n"
                  << "Successful reads: " << successful_reads << '/' << count << '\n'
                  << "Read time: " << read_ms << " ms\n"
                  << "Read throughput: "
                  << (count * 1000.0 / read_ms) << " ops/sec\n"
                  << "\nCompaction test (after updates and deletes):\n"
                  << "File size before: " << size_before_compaction << " bytes\n"
                  << "File size after: " << size_after_compaction << " bytes\n"
                  << "Bytes saved: "
                  << (size_before_compaction - size_after_compaction) << '\n'
                  << "Compaction time: " << compaction_ms << " ms\n";

        std::filesystem::remove(individual_path);
        std::filesystem::remove(individual_path.string() + ".wal");
        std::filesystem::remove(batch_path);
        std::filesystem::remove(batch_path.string() + ".wal");
        return successful_reads == count ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark error: " << error.what() << '\n';
        std::cerr << "Benchmark database files were left in data/ for inspection.\n";
        return 1;
    }
}
