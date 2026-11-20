#include "storage.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "[PASS] " << message << '\n';
    } else {
        std::cout << "[FAIL] " << message << '\n';
        ++failures;
    }
}

std::filesystem::path test_path(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / "minidb_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / (name + ".db");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
    std::filesystem::remove(path.string() + ".compact.tmp");
    std::filesystem::remove(path.string() + ".compact.bak");
    return path;
}

void test_crud_and_update() {
    const auto path = test_path("crud");
    {
        minidb::Storage db(path);
        db.set("user", "Sooraj");
        check(db.get("user") == std::optional<std::string>("Sooraj"),
              "SET then GET returns the stored value");

        db.set("user", "Sooraj Rao");
        check(db.get("user") == std::optional<std::string>("Sooraj Rao"),
              "SET overwrites an existing key");

        check(db.size() == 1, "Updating a key does not increase live key count");
        check(db.remove("user"), "DELETE returns true for an existing key");
        check(!db.get("user").has_value(), "Deleted key is not found");
        check(!db.remove("user"), "DELETE returns false for a missing key");
    }
    std::filesystem::remove(path);
}

void test_persistence() {
    const auto path = test_path("persistence");
    {
        minidb::Storage db(path);
        db.set("language", "C++20");
        db.set("city", "Mangalore");
    }
    {
        minidb::Storage reopened(path);
        check(reopened.get("language") == std::optional<std::string>("C++20"),
              "Value survives closing and reopening the database");
        check(reopened.get("city") == std::optional<std::string>("Mangalore"),
              "Multiple values survive reopening");
        check(reopened.size() == 2, "Index is rebuilt correctly on startup");
    }
    std::filesystem::remove(path);
}

void test_recovery() {
    const auto path = test_path("recovery");
    {
        minidb::Storage db(path);
        db.set("safe", "record");
    }
    const auto valid_size = std::filesystem::file_size(path);
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const char incomplete[] = {1, 5};
        out.write(incomplete, sizeof(incomplete));
    }
    {
        minidb::Storage recovered(path);
        check(recovered.get("safe") == std::optional<std::string>("record"),
              "Recovery preserves the last complete record");
        check(std::filesystem::file_size(path) == valid_size,
              "Recovery truncates the incomplete trailing bytes");
    }
    std::filesystem::remove(path);
}


void test_batch_writes() {
    const auto path = test_path("batch");
    {
        minidb::Storage db(path);
        db.set_many({
            {"first", "one"},
            {"second", "two"},
            {"first", "updated"}
        });
        check(db.get("first") == std::optional<std::string>("updated"),
              "Batch write applies the latest value for duplicate keys");
        check(db.get("second") == std::optional<std::string>("two"),
              "Batch write stores multiple keys");
        check(db.size() == 2, "Batch write maintains the correct live key count");
    }
    {
        minidb::Storage reopened(path);
        check(reopened.get("first") == std::optional<std::string>("updated"),
              "Batch-written data survives reopening");
        check(reopened.get("second") == std::optional<std::string>("two"),
              "All batch records are persisted");
    }
    std::filesystem::remove(path);
}


void write_u32(std::ostream& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.put(static_cast<char>((value >> (i * 8)) & 0xFF));
}
void write_test_record(std::ostream& out, std::uint8_t op,
                       const std::string& key, const std::string& value) {
    out.put(static_cast<char>(op));
    write_u32(out, static_cast<std::uint32_t>(key.size()));
    write_u32(out, static_cast<std::uint32_t>(value.size()));
    out.write(key.data(), static_cast<std::streamsize>(key.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void test_wal_recovery() {
    const auto path = test_path("wal_recovery");
    // Simulate a crash after the WAL was flushed but before the data log was updated.
    {
        std::ofstream wal(path.string() + ".wal", std::ios::binary | std::ios::trunc);
        write_test_record(wal, 1, "recovered", "from-wal");
    }
    {
        minidb::Storage recovered(path);
        check(recovered.get("recovered") == std::optional<std::string>("from-wal"),
              "Startup replays a complete pending WAL record");
        check(recovered.wal_size() == 0, "WAL is cleared after successful replay");
    }
    {
        minidb::Storage reopened(path);
        check(reopened.get("recovered") == std::optional<std::string>("from-wal"),
              "Replayed WAL data remains persistent after another restart");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
}

void test_wal_truncated_tail() {
    const auto path = test_path("wal_truncated");
    {
        std::ofstream wal(path.string() + ".wal", std::ios::binary | std::ios::trunc);
        write_test_record(wal, 1, "valid", "yes");
        const char partial[] = {1, 5};
        wal.write(partial, sizeof(partial));
    }
    {
        minidb::Storage recovered(path);
        check(recovered.get("valid") == std::optional<std::string>("yes"),
              "Recovery replays complete WAL records before a truncated tail");
        check(recovered.wal_size() == 0, "Truncated WAL is cleared after recovery");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
}

void test_wal_cleared_after_writes() {
    const auto path = test_path("wal_cleared");
    {
        minidb::Storage db(path);
        db.set("one", "1");
        db.set_many({{"two", "2"}, {"three", "3"}});
        check(db.wal_size() == 0, "WAL is empty after successful SET and batch writes");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
}

void test_compaction() {
    const auto path = test_path("compaction");
    {
        minidb::Storage db(path);
        db.set("keep", "old");
        db.set("keep", "new");
        db.set("remove", "temporary");
        db.remove("remove");

        const auto before = db.file_size();
        db.compact();
        const auto after = db.file_size();

        check(after < before, "Compaction reduces file size when obsolete records exist");
        check(db.get("keep") == std::optional<std::string>("new"),
              "Compaction preserves the latest value");
        check(!db.get("remove").has_value(),
              "Compaction does not resurrect deleted keys");
    }
    {
        minidb::Storage reopened(path);
        check(reopened.get("keep") == std::optional<std::string>("new"),
              "Compacted data survives reopening");
        check(!reopened.get("remove").has_value(),
              "Deleted key remains absent after reopening compacted data");
    }
    std::filesystem::remove(path);
}

void test_ttl_operations_and_persistence() {
    const auto path = test_path("ttl");
    {
        minidb::Storage db(path);
        db.set_with_ttl("short", "lived", 2);
        check(db.get("short") == std::optional<std::string>("lived"), "TTL key is readable before expiration");
        check(db.ttl("short") >= 0 && db.ttl("short") <= 2, "TTL reports remaining seconds");
        db.set("permanent", "value");
        check(db.ttl("permanent") == -1, "TTL returns -1 for a persistent key");
        check(db.ttl("missing") == -2, "TTL returns -2 for a missing key");
        check(db.expire("permanent", 30), "EXPIRE adds expiration to an existing key");
        check(db.persist("permanent"), "PERSIST removes an existing expiration");
        check(db.ttl("permanent") == -1, "Persisted key no longer has an expiration");
    }
    {
        minidb::Storage reopened(path);
        check(reopened.ttl("short") >= 0, "Expiration timestamp survives database restart");
        check(reopened.get("permanent") == std::optional<std::string>("value"), "Persistent value survives restart after PERSIST");
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    {
        minidb::Storage reopened(path);
        check(!reopened.get("short"), "Expired key is removed after restart");
        check(reopened.size() == 1, "Expired keys are excluded from live key count");
    }
    std::filesystem::remove(path); std::filesystem::remove(path.string() + ".wal");
}

void test_ttl_compaction() {
    const auto path = test_path("ttl_compaction");
    {
        minidb::Storage db(path);
        db.set_with_ttl("temporary", "keep-expiry", 60);
        db.set("normal", "value");
        db.compact();
    }
    {
        minidb::Storage reopened(path);
        check(reopened.ttl("temporary") >= 0, "Compaction preserves TTL metadata");
        check(reopened.get("temporary") == std::optional<std::string>("keep-expiry"), "Compaction preserves TTL value");
    }
    std::filesystem::remove(path); std::filesystem::remove(path.string() + ".wal");
}

} // namespace

int main() {
    std::cout << "MiniDB automated tests\n\n";
    test_crud_and_update();
    test_persistence();
    test_recovery();
    test_batch_writes();
    test_compaction();
    test_wal_recovery();
    test_wal_truncated_tail();
    test_wal_cleared_after_writes();
    test_ttl_operations_and_persistence();
    test_ttl_compaction();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cout << failures << " test(s) failed.\n";
    return 1;
}
