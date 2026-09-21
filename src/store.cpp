#include "store.hpp"

#include <memory>
#include <sstream>

namespace edge {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw SqliteError(sqlite3_errmsg(db));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

    void step_done() const {
        if (sqlite3_step(statement_) != SQLITE_DONE) {
            throw SqliteError(sqlite3_errmsg(db_));
        }
    }

private:
    sqlite3* db_{};
    sqlite3_stmt* statement_{};
};

void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw SqliteError("failed to bind text parameter");
    }
}

void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        throw SqliteError("failed to bind integer parameter");
    }
}

void bind_double(sqlite3_stmt* statement, int index, double value) {
    if (sqlite3_bind_double(statement, index, value) != SQLITE_OK) {
        throw SqliteError("failed to bind floating point parameter");
    }
}

std::int64_t scalar_int64(sqlite3* db, const char* sql) {
    Statement statement(db, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw SqliteError(sqlite3_errmsg(db));
    }
    return sqlite3_column_int64(statement.get(), 0);
}

}  // namespace

Database::Database(const std::filesystem::path& path) {
    const auto path_string = path.string();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path_string.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate SQLite handle";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw SqliteError(message);
    }
    sqlite3_busy_timeout(db_, 5000);
    execute("PRAGMA journal_mode=WAL;");
    execute("PRAGMA synchronous=FULL;");
    execute("PRAGMA foreign_keys=ON;");
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::execute(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw SqliteError(message);
    }
}

EdgeStore::EdgeStore(const std::filesystem::path& path) : db_(path) {
    db_.execute(
        "CREATE TABLE IF NOT EXISTS edge_events("
        "event_id TEXT PRIMARY KEY,"
        "device_id TEXT NOT NULL,"
        "sequence INTEGER NOT NULL,"
        "timestamp_ms INTEGER NOT NULL,"
        "value REAL NOT NULL,"
        "acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0,1)));"
        "CREATE INDEX IF NOT EXISTS idx_edge_pending ON edge_events(acknowledged, sequence);"
        "CREATE TABLE IF NOT EXISTS edge_meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO edge_meta(key, value) VALUES('next_sequence', 1);"
        "INSERT OR IGNORE INTO edge_meta(key, value) "
        "SELECT 'acknowledged_total', COUNT(*) FROM edge_events WHERE acknowledged=1;"
        "DELETE FROM edge_events WHERE acknowledged=1;");
}

Event EdgeStore::append(const std::string& device_id, std::int64_t timestamp_ms, double value) {
    // 序号分配和事件插入必须在同一个事务中。进程在任意位置崩溃时，
    // 两项修改要么一起提交，要么一起回滚，避免“序号前进但事件不存在”。
    db_.execute("BEGIN IMMEDIATE;");
    try {
        const auto sequence = scalar_int64(
            db_.get(), "SELECT value FROM edge_meta WHERE key='next_sequence';");
        Event event{
            device_id + ":" + std::to_string(sequence),
            device_id,
            sequence,
            timestamp_ms,
            value,
        };

        Statement insert(
            db_.get(),
            "INSERT INTO edge_events(event_id, device_id, sequence, timestamp_ms, value) "
            "VALUES(?, ?, ?, ?, ?);");
        bind_text(insert.get(), 1, event.id);
        bind_text(insert.get(), 2, event.device_id);
        bind_int64(insert.get(), 3, event.sequence);
        bind_int64(insert.get(), 4, event.timestamp_ms);
        bind_double(insert.get(), 5, event.value);
        insert.step_done();

        db_.execute("UPDATE edge_meta SET value=value+1 WHERE key='next_sequence';");
        db_.execute("COMMIT;");
        return event;
    } catch (...) {
        try {
            db_.execute("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
}

std::vector<Event> EdgeStore::pending(std::size_t limit) const {
    Statement statement(
        db_.get(),
        "SELECT event_id, device_id, sequence, timestamp_ms, value "
        "FROM edge_events WHERE acknowledged=0 ORDER BY sequence LIMIT ?;");
    bind_int64(statement.get(), 1, static_cast<std::int64_t>(limit));

    std::vector<Event> events;
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw SqliteError(sqlite3_errmsg(db_.get()));
        }
        events.push_back(Event{
            reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1)),
            sqlite3_column_int64(statement.get(), 2),
            sqlite3_column_int64(statement.get(), 3),
            sqlite3_column_double(statement.get(), 4),
        });
    }
    return events;
}

void EdgeStore::acknowledge(const std::string& event_id) {
    // ACK 状态和事件删除放在同一个事务里。删除可以让本地数据库长期保持为
    // “待发送队列”，累计确认数则单独保存在 meta 表中。
    db_.execute("BEGIN IMMEDIATE;");
    try {
        Statement statement(
            db_.get(), "DELETE FROM edge_events WHERE acknowledged=0 AND event_id=?;");
        bind_text(statement.get(), 1, event_id);
        statement.step_done();
        if (sqlite3_changes(db_.get()) == 1) {
            db_.execute(
                "UPDATE edge_meta SET value=value+1 WHERE key='acknowledged_total';");
        }
        db_.execute("COMMIT;");
    } catch (...) {
        try {
            db_.execute("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
}

EdgeStats EdgeStore::stats() const {
    Statement statement(
        db_.get(),
        "SELECT "
        "(SELECT value-1 FROM edge_meta WHERE key='next_sequence'),"
        "(SELECT value FROM edge_meta WHERE key='acknowledged_total'),"
        "(SELECT COUNT(*) FROM edge_events WHERE acknowledged=0);");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw SqliteError(sqlite3_errmsg(db_.get()));
    }
    return EdgeStats{
        sqlite3_column_int64(statement.get(), 0),
        sqlite3_column_int64(statement.get(), 1),
        sqlite3_column_int64(statement.get(), 2),
    };
}

std::int64_t EdgeStore::pending_payload_bytes() const {
    // SQLite 行本身还有页和索引开销，因此这里是用于背压决策的保守估算值，
    // 不是操作系统看到的物理文件大小。
    return scalar_int64(
        db_.get(),
        "SELECT COALESCE(SUM(length(event_id)+length(device_id)+40),0) "
        "FROM edge_events WHERE acknowledged=0;");
}

ReceiverStore::ReceiverStore(const std::filesystem::path& path) : db_(path) {
    db_.execute(
        "CREATE TABLE IF NOT EXISTS received_events("
        "event_id TEXT PRIMARY KEY,"
        "device_id TEXT NOT NULL,"
        "sequence INTEGER NOT NULL,"
        "timestamp_ms INTEGER NOT NULL,"
        "value REAL NOT NULL,"
        "received_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_received_device_sequence "
        "ON received_events(device_id, sequence);");
}

bool ReceiverStore::insert_if_new(const Event& event) {
    // event_id 是幂等键。重复投递返回“未插入”，但上层仍会回复 ACK，
    // 这样客户端最终可以结束重试并把本地事件标记为已确认。
    Statement statement(
        db_.get(),
        "INSERT OR IGNORE INTO received_events"
        "(event_id, device_id, sequence, timestamp_ms, value) VALUES(?, ?, ?, ?, ?);");
    bind_text(statement.get(), 1, event.id);
    bind_text(statement.get(), 2, event.device_id);
    bind_int64(statement.get(), 3, event.sequence);
    bind_int64(statement.get(), 4, event.timestamp_ms);
    bind_double(statement.get(), 5, event.value);
    statement.step_done();
    return sqlite3_changes(db_.get()) == 1;
}

std::int64_t ReceiverStore::count() const {
    return scalar_int64(db_.get(), "SELECT COUNT(*) FROM received_events;");
}

}  // namespace edge
