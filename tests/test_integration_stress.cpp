#include "storage.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
void remove_database_files(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".wal", ec);
    std::filesystem::remove(path.string() + ".compact.tmp", ec);
    std::filesystem::remove(path.string() + ".compact.bak", ec);
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() / "minidb_m12_integration_stress.db";
    remove_database_files(path);

    constexpr int thread_count = 8;
    constexpr int keys_per_thread = 250;
    constexpr int total_keys = thread_count * keys_per_thread;
    std::atomic<int> failures{0};
    std::atomic<int> completed_operations{0};

    try {
        {
            minidb::Storage db(path);
            std::vector<std::thread> workers;
            workers.reserve(thread_count);

            for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
                workers.emplace_back([&, thread_id] {
                    for (int i = 0; i < keys_per_thread; ++i) {
                        const std::string key = "stress:" + std::to_string(thread_id) + ":" + std::to_string(i);
                        const std::string initial = "value:" + std::to_string(thread_id) + ":" + std::to_string(i);
                        const std::string updated = "updated:" + std::to_string(thread_id) + ":" + std::to_string(i);
                        try {
                            db.set(key, initial);
                            auto first = db.get(key);
                            if (!first || *first != initial) ++failures;

                            if (i % 3 == 0) {
                                db.set(key, updated);
                                auto second = db.get(key);
                                if (!second || *second != updated) ++failures;
                            }

                            if (i % 7 == 0) {
                                db.expire(key, 120);
                                if (db.ttl(key) < 0) ++failures;
                                if (!db.persist(key) || db.ttl(key) != -1) ++failures;
                            }

                            if (i % 5 == 0) {
                                if (!db.remove(key) || db.get(key).has_value()) ++failures;
                            }
                            ++completed_operations;
                        } catch (...) {
                            ++failures;
                        }
                    }
                });
            }
            for (auto& worker : workers) worker.join();

            const auto expected_live = static_cast<std::size_t>(total_keys - ((keys_per_thread + 4) / 5) * thread_count);
            if (failures.load() != 0 || db.size() != expected_live) {
                std::cerr << "FAIL: mixed concurrent workload; failures=" << failures
                          << ", live_keys=" << db.size() << ", expected=" << expected_live << '\n';
                remove_database_files(path);
                return 1;
            }

            const auto before_compaction = db.file_size();
            db.compact();
            const auto after_compaction = db.file_size();
            if (after_compaction > before_compaction) {
                std::cerr << "FAIL: compaction increased file size\n";
                remove_database_files(path);
                return 1;
            }
            std::cout << "PASS: " << completed_operations << " mixed key workloads across "
                      << thread_count << " threads\n";
            std::cout << "PASS: live-key count and compaction invariants\n";
        }

        {
            minidb::Storage reopened(path);
            const auto expected_live = static_cast<std::size_t>(total_keys - ((keys_per_thread + 4) / 5) * thread_count);
            if (reopened.size() != expected_live) {
                std::cerr << "FAIL: restart live-key count mismatch: " << reopened.size()
                          << " expected " << expected_live << '\n';
                remove_database_files(path);
                return 1;
            }
            for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
                for (int i = 0; i < keys_per_thread; ++i) {
                    const std::string key = "stress:" + std::to_string(thread_id) + ":" + std::to_string(i);
                    const bool deleted = (i % 5 == 0);
                    auto value = reopened.get(key);
                    if (deleted) {
                        if (value) ++failures;
                    } else {
                        const std::string expected = (i % 3 == 0)
                            ? "updated:" + std::to_string(thread_id) + ":" + std::to_string(i)
                            : "value:" + std::to_string(thread_id) + ":" + std::to_string(i);
                        if (!value || *value != expected) ++failures;
                    }
                }
            }
            if (failures.load() != 0) {
                std::cerr << "FAIL: restart verification found " << failures << " mismatches\n";
                remove_database_files(path);
                return 1;
            }
            std::cout << "PASS: all surviving values verified after compaction and restart\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: integration stress test threw: " << e.what() << '\n';
        remove_database_files(path);
        return 1;
    }

    remove_database_files(path);
    std::cout << "MiniDB integration stress test passed.\n";
    return 0;
}
