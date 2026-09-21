#pragma once

#include "model.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace edge {

class SqliteError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    sqlite3* get() const noexcept { return db_; }
    void execute(const char* sql) const;

private:
    sqlite3* db_{};
};

class EdgeStore {
public:
    explicit EdgeStore(const std::filesystem::path& path);

    Event append(const std::string& device_id, std::int64_t timestamp_ms, double value);
    std::vector<Event> pending(std::size_t limit) const;
    void acknowledge(const std::string& event_id);
    EdgeStats stats() const;
    std::int64_t pending_payload_bytes() const;

private:
    Database db_;
};

class ReceiverStore {
public:
    explicit ReceiverStore(const std::filesystem::path& path);

    bool insert_if_new(const Event& event);
    std::int64_t count() const;

private:
    Database db_;
};

}  // namespace edge
