#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct mdb_store;

namespace minidb {

enum class Operation : std::uint8_t { Set = 1, Delete = 2 };

class Storage {
public:
    struct Record { Operation operation; std::string key; std::string value; };
    struct Entry { std::string value; std::optional<std::uint64_t> expires_at; };
    explicit Storage(std::filesystem::path file_path);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;

    void set(const std::string& key, const std::string& value);
    void set_with_ttl(const std::string& key, const std::string& value, std::uint64_t seconds);
    void set_many(const std::vector<std::pair<std::string, std::string>>& entries);
    [[nodiscard]] std::optional<std::string> get(const std::string& key);
    bool remove(const std::string& key);
    bool expire(const std::string& key, std::uint64_t seconds);
    // -2 = missing, -1 = no expiration, >=0 = seconds remaining.
    [[nodiscard]] std::int64_t ttl(const std::string& key);
    bool persist(const std::string& key);
    std::size_t sweep_expired();
    [[nodiscard]] std::size_t size();
    [[nodiscard]] std::uintmax_t file_size() const;
    [[nodiscard]] std::uintmax_t wal_size() const;
    void compact();

private:
    void commit_records(const std::vector<Record>& records);
    void rebuild_index();
    bool expire_if_needed(const std::string& key, std::uint64_t now);

    std::filesystem::path file_path_;
    mdb_store* engine_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> index_;
};

} // namespace minidb
