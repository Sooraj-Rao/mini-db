#include "storage.hpp"
#include "c_engine/storage_engine.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minidb {
namespace {
constexpr std::size_t MAX_FIELD_SIZE = 16 * 1024 * 1024;
constexpr char TTL_MAGIC[] = "MDBTTL01";
constexpr std::size_t TTL_HEADER_SIZE = 16;

std::uint64_t now_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
void validate_record(const std::string& key, const std::string& value) {
    if (key.empty()) throw std::invalid_argument("Key cannot be empty");
    if (key.size() > MAX_FIELD_SIZE || value.size() > MAX_FIELD_SIZE - TTL_HEADER_SIZE)
        throw std::length_error("Key or value exceeds the 16 MiB limit");
}
void throw_engine_error(const char* prefix, const char* error) {
    throw std::runtime_error(std::string(prefix) + ": " + (error[0] ? error : "unknown C storage engine error"));
}
std::string encode_entry(const std::string& value, std::optional<std::uint64_t> expires_at) {
    if (!expires_at) return value;
    std::string encoded(TTL_MAGIC, sizeof(TTL_MAGIC) - 1);
    for (unsigned i = 0; i < 8; ++i) encoded.push_back(static_cast<char>((*expires_at >> (i * 8)) & 0xff));
    encoded += value;
    return encoded;
}
Storage::Entry decode_entry(const char* data, std::size_t length) {
    if (length >= TTL_HEADER_SIZE && std::string(data, sizeof(TTL_MAGIC) - 1) == TTL_MAGIC) {
        std::uint64_t expiry = 0;
        for (unsigned i = 0; i < 8; ++i)
            expiry |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[8 + i])) << (i * 8);
        return Storage::Entry{std::string(data + TTL_HEADER_SIZE, length - TTL_HEADER_SIZE), expiry};
    }
    return Storage::Entry{std::string(data, length), std::nullopt};
}
int rebuild_callback(std::uint8_t operation, const char* key, std::size_t key_length,
                     const char* value, std::size_t value_length, void* user_data) {
    auto* index = static_cast<std::unordered_map<std::string, Storage::Entry>*>(user_data);
    std::string k(key, key_length);
    if (operation == static_cast<std::uint8_t>(Operation::Set)) {
        index->insert_or_assign(std::move(k), decode_entry(value, value_length));
    } else if (operation == static_cast<std::uint8_t>(Operation::Delete)) index->erase(k);
    else return 0;
    return 1;
}
} // namespace

Storage::Storage(std::filesystem::path file_path) : file_path_(std::move(file_path)) {
    if (file_path_.has_parent_path()) std::filesystem::create_directories(file_path_.parent_path());
    char error[256]{};
    if (!mdb_store_open(&engine_, file_path_.string().c_str(), error, sizeof(error)))
        throw_engine_error("Unable to initialize C storage engine", error);
    try { rebuild_index(); sweep_expired(); }
    catch (...) { mdb_store_close(engine_); engine_ = nullptr; throw; }
}
Storage::~Storage() { if (engine_) mdb_store_close(engine_); }
void Storage::rebuild_index() {
    index_.clear(); char error[256]{};
    if (!mdb_store_scan(engine_, rebuild_callback, &index_, error, sizeof(error)))
        throw_engine_error("Unable to rebuild in-memory index", error);
}
void Storage::commit_records(const std::vector<Record>& records) {
    if (records.empty()) return;
    std::vector<mdb_record> raw; raw.reserve(records.size());
    for (const auto& r : records) raw.push_back(mdb_record{static_cast<std::uint8_t>(r.operation), r.key.data(), r.key.size(), r.value.data(), r.value.size()});
    char error[256]{};
    if (!mdb_store_append_batch(engine_, raw.data(), raw.size(), error, sizeof(error)))
        throw_engine_error("C storage engine write failed", error);
    for (const auto& r : records) {
        if (r.operation == Operation::Set) index_[r.key] = decode_entry(r.value.data(), r.value.size());
        else index_.erase(r.key);
    }
}
void Storage::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_); validate_record(key, value);
    commit_records({Record{Operation::Set, key, value}});
}
void Storage::set_with_ttl(const std::string& key, const std::string& value, std::uint64_t seconds) {
    if (!seconds) throw std::invalid_argument("TTL must be greater than zero");
    std::lock_guard<std::mutex> lock(mutex_); validate_record(key, value);
    const auto now = now_seconds();
    if (seconds > std::numeric_limits<std::uint64_t>::max() - now) throw std::overflow_error("TTL is too large");
    commit_records({Record{Operation::Set, key, encode_entry(value, now + seconds)}});
}
void Storage::set_many(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::lock_guard<std::mutex> lock(mutex_); std::vector<Record> records; records.reserve(entries.size());
    for (const auto& [key, value] : entries) { validate_record(key, value); records.push_back({Operation::Set, key, value}); }
    commit_records(records);
}
bool Storage::expire_if_needed(const std::string& key, std::uint64_t now) {
    auto it = index_.find(key);
    if (it == index_.end() || !it->second.expires_at || *it->second.expires_at > now) return false;
    commit_records({Record{Operation::Delete, key, ""}}); return true;
}
std::optional<std::string> Storage::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_); expire_if_needed(key, now_seconds());
    auto it = index_.find(key); if (it == index_.end()) return std::nullopt; return it->second.value;
}
bool Storage::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_); if (key.empty()) throw std::invalid_argument("Key cannot be empty");
    expire_if_needed(key, now_seconds()); if (!index_.contains(key)) return false;
    commit_records({Record{Operation::Delete, key, ""}}); return true;
}
bool Storage::expire(const std::string& key, std::uint64_t seconds) {
    if (!seconds) throw std::invalid_argument("TTL must be greater than zero");
    std::lock_guard<std::mutex> lock(mutex_); expire_if_needed(key, now_seconds());
    auto it = index_.find(key); if (it == index_.end()) return false;
    const auto now = now_seconds(); if (seconds > std::numeric_limits<std::uint64_t>::max() - now) throw std::overflow_error("TTL is too large");
    commit_records({Record{Operation::Set, key, encode_entry(it->second.value, now + seconds)}}); return true;
}
std::int64_t Storage::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_); expire_if_needed(key, now_seconds());
    auto it = index_.find(key); if (it == index_.end()) return -2; if (!it->second.expires_at) return -1;
    const auto remaining = *it->second.expires_at - now_seconds();
    return remaining > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(remaining);
}
bool Storage::persist(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_); expire_if_needed(key, now_seconds());
    auto it = index_.find(key); if (it == index_.end() || !it->second.expires_at) return false;
    commit_records({Record{Operation::Set, key, it->second.value}}); return true;
}
std::size_t Storage::sweep_expired() {
    std::lock_guard<std::mutex> lock(mutex_); const auto now = now_seconds();
    std::vector<Record> deletes;
    for (const auto& [key, entry] : index_) if (entry.expires_at && *entry.expires_at <= now) deletes.push_back({Operation::Delete, key, ""});
    const auto count = deletes.size(); commit_records(deletes); return count;
}
std::size_t Storage::size() { sweep_expired(); std::lock_guard<std::mutex> lock(mutex_); return index_.size(); }
std::uintmax_t Storage::file_size() const { std::lock_guard<std::mutex> lock(mutex_); std::uint64_t n=0; if(!mdb_store_file_size(engine_,&n)) throw std::runtime_error("Unable to read database file size"); return static_cast<std::uintmax_t>(n); }
std::uintmax_t Storage::wal_size() const { std::lock_guard<std::mutex> lock(mutex_); std::uint64_t n=0; if(!mdb_store_wal_size(engine_,&n)) throw std::runtime_error("Unable to read WAL file size"); return static_cast<std::uintmax_t>(n); }
void Storage::compact() {
    std::lock_guard<std::mutex> lock(mutex_); const auto now = now_seconds();
    std::vector<Record> live;
    for (auto it = index_.begin(); it != index_.end();) {
        if (it->second.expires_at && *it->second.expires_at <= now) it = index_.erase(it);
        else { live.push_back({Operation::Set, it->first, encode_entry(it->second.value, it->second.expires_at)}); ++it; }
    }
    std::uint64_t pending=0; if(!mdb_store_wal_size(engine_,&pending)) throw std::runtime_error("Unable to read WAL file size");
    if(pending) throw std::runtime_error("Cannot compact while WAL contains pending records; restart to recover first");
    std::vector<mdb_record> raw; raw.reserve(live.size());
    for(const auto& r:live) raw.push_back(mdb_record{static_cast<std::uint8_t>(r.operation),r.key.data(),r.key.size(),r.value.data(),r.value.size()});
    char error[256]{}; if(!mdb_store_compact(engine_,raw.data(),raw.size(),error,sizeof(error))) throw_engine_error("C storage engine compaction failed",error);
}
} // namespace minidb
