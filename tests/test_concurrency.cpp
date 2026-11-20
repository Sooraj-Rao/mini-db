#include "storage.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "minidb_concurrency_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".wal", ec);

    try {
        minidb::Storage db(path);
        constexpr int thread_count = 8;
        constexpr int keys_per_thread = 150;
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < thread_count; ++t) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < keys_per_thread; ++i) {
                    const std::string key = "thread-" + std::to_string(t) + "-key-" + std::to_string(i);
                    const std::string value = "value-" + std::to_string(t) + "-" + std::to_string(i);
                    try {
                        db.set(key, value);
                        const auto actual = db.get(key);
                        if (!actual || *actual != value) ++failures;
                    } catch (...) { ++failures; }
                }
            });
        }
        for (auto& worker : workers) worker.join();

        const auto expected = static_cast<std::size_t>(thread_count * keys_per_thread);
        if (failures.load() != 0 || db.size() != expected) {
            std::cerr << "FAIL: concurrent operations had " << failures << " failures; keys=" << db.size() << '\n';
            return 1;
        }
        std::cout << "PASS: " << expected << " keys written and read across " << thread_count << " threads\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".wal", ec);
    return 0;
}
