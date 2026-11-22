#ifndef STATEMENT_H
#define STATEMENT_H

// Internal to the db folder: a small RAII wrapper around an SQLite prepared statement.
// Used by Database.cpp and HospitalStore.cpp. Values are always bound with placeholders (?),
// never pasted into the SQL text, so user input cannot change a query (no SQL injection).

#include <sqlite3.h>

#include <string>

namespace db {

// RAII wrapper so every prepared statement is always finalized.
class Statement {
public:
    Statement(sqlite3* connection, const char* sql) {
        prepared = sqlite3_prepare_v2(connection, sql, -1, &handle, nullptr) == SQLITE_OK;
    }
    ~Statement() {
        sqlite3_finalize(handle);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool isPrepared() const {
        return prepared;
    }

    Statement& bind(int index, int value) {
        sqlite3_bind_int(handle, index, value);
        return *this;
    }
    Statement& bind(int index, double value) {
        sqlite3_bind_double(handle, index, value);
        return *this;
    }
    Statement& bind(int index, const std::string& value) {
        sqlite3_bind_text(handle, index, value.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bindNull(int index) {
        sqlite3_bind_null(handle, index);
        return *this;
    }

    int step() {
        return sqlite3_step(handle);
    }

    int columnInt(int column) const {
        return sqlite3_column_int(handle, column);
    }
    long long columnInt64(int column) const {
        return sqlite3_column_int64(handle, column);
    }
    bool columnIsNull(int column) const {
        return sqlite3_column_type(handle, column) == SQLITE_NULL;
    }
    double columnDouble(int column) const {
        return sqlite3_column_double(handle, column);
    }
    // NULL-safe: returns an empty string instead of crashing on a NULL column.
    std::string columnText(int column) const {
        const unsigned char* text = sqlite3_column_text(handle, column);
        return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
    }

private:
    sqlite3_stmt* handle = nullptr;
    bool prepared = false;
};

}  // namespace db

#endif
