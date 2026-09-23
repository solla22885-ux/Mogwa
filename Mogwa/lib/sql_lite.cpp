#include "pch.h"
#include "sql_lite.h"
#include "sqlite3.h"
#include "scope_exit.hpp"

namespace m1::sql_lite {
    sqlite3* create_or_open(const std::string& path)
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return nullptr;
        }

        std::string error;
        if (!execute_sql(db, "PRAGMA journal_mode=WAL;", error)
            || !execute_sql(db, "PRAGMA synchronous=NORMAL;", error)
            || !execute_sql(db, "PRAGMA busy_timeout=3000;", error)) {
            close(db);
            return nullptr;
        }
        return db;
    }

    bool close(sqlite3* db)
    {
        if (!db) return false;
        finalize_all_stmts(db);
        return sqlite3_close(db) == SQLITE_OK;
    }

    void finalize_all_stmts(sqlite3* db)
    {
        if (!db) return;
        for (sqlite3_stmt* statement = sqlite3_next_stmt(db, nullptr);
            statement != nullptr; statement = sqlite3_next_stmt(db, nullptr)) {
            sqlite3_finalize(statement);
        }
    }

    bool execute_sql(sqlite3* db, const char* sql, std::string& out_error)
    {
        out_error.clear();
        if (!db || !sql) {
            out_error = "invalid database handle or SQL";
            return false;
        }

        char* sqlite_error = nullptr;
        const int result = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
        if (result != SQLITE_OK) {
            out_error = sqlite_error ? sqlite_error : sqlite3_errmsg(db);
            sqlite3_free(sqlite_error);
            return false;
        }
        return true;
    }

    bool execute_stmt(sqlite3* db, const char* sql,
        const std::vector<std::pair<int, std::string>>& values, std::string& out_error)
    {
        out_error.clear();
        if (!db || !sql) {
            out_error = "invalid database handle or SQL";
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        if (result != SQLITE_OK) {
            out_error = sqlite3_errmsg(db);
            return false;
        }
        m1::util::scope_exit finalize([statement]() { sqlite3_finalize(statement); });

        if (static_cast<int>(values.size()) != sqlite3_bind_parameter_count(statement)) {
            out_error = "SQL parameter count mismatch";
            return false;
        }

        try {
            int index = 1;
            for (const auto& [type, value] : values) {
                switch (type) {
                case TYPE_TEXT:
                    result = sqlite3_bind_text(statement, index++, value.c_str(), -1, SQLITE_TRANSIENT);
                    break;
                case TYPE_INT:
                    result = sqlite3_bind_int(statement, index++, std::stoi(value));
                    break;
                case TYPE_INT64:
                    result = sqlite3_bind_int64(statement, index++, std::stoll(value));
                    break;
                case TYPE_DOUBLE:
                    result = sqlite3_bind_double(statement, index++, std::stod(value));
                    break;
                case TYPE_BLOB:
                    result = sqlite3_bind_blob(statement, index++, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
                    break;
                case TYPE_NULL:
                    result = sqlite3_bind_null(statement, index++);
                    break;
                default:
                    out_error = "unsupported SQL parameter type";
                    return false;
                }

                if (result != SQLITE_OK) {
                    out_error = sqlite3_errmsg(db);
                    return false;
                }
            }
        }
        catch (const std::exception& error) {
            out_error = error.what();
            return false;
        }

        result = sqlite3_step(statement);
        if (result != SQLITE_DONE) {
            out_error = sqlite3_errmsg(db);
            return false;
        }
        return true;
    }

    bool execute_get_stmt(sqlite3* db, const char* sql, const std::vector<int>& types,
        std::vector<std::string>& out_values, std::string& out_error)
    {
        out_error.clear();
        out_values.clear();
        if (!db || !sql) {
            out_error = "invalid database handle or SQL";
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        if (result != SQLITE_OK) {
            out_error = sqlite3_errmsg(db);
            return false;
        }
        m1::util::scope_exit finalize([statement]() { sqlite3_finalize(statement); });

        if (static_cast<int>(types.size()) != sqlite3_column_count(statement)) {
            out_error = "result column count mismatch";
            return false;
        }

        result = sqlite3_step(statement);
        if (result == SQLITE_DONE) return true;
        if (result != SQLITE_ROW) {
            out_error = sqlite3_errmsg(db);
            return false;
        }

        int index = 0;
        for (const int type : types) {
            switch (type) {
            case TYPE_TEXT: {
                const unsigned char* value = sqlite3_column_text(statement, index++);
                out_values.emplace_back(value ? reinterpret_cast<const char*>(value) : "");
                break;
            }
            case TYPE_INT:
                out_values.emplace_back(std::to_string(sqlite3_column_int(statement, index++)));
                break;
            case TYPE_INT64:
                out_values.emplace_back(std::to_string(sqlite3_column_int64(statement, index++)));
                break;
            case TYPE_DOUBLE:
                out_values.emplace_back(std::to_string(sqlite3_column_double(statement, index++)));
                break;
            case TYPE_BLOB: {
                const void* value = sqlite3_column_blob(statement, index);
                const int length = sqlite3_column_bytes(statement, index++);
                out_values.emplace_back(value && length > 0
                    ? std::string(reinterpret_cast<const char*>(value), length) : std::string());
                break;
            }
            case TYPE_NULL:
                out_values.emplace_back();
                ++index;
                break;
            default:
                out_error = "unsupported SQL result type";
                return false;
            }
        }
        return true;
    }
}
