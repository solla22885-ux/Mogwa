// sql_lite.h: sql_lite 헤더 파일.
//

#pragma once

struct sqlite3;

namespace m1 {
namespace sql_lite {

    enum {
        TYPE_TEXT = 0,
        TYPE_INT,
        TYPE_INT64,
        TYPE_DOUBLE,
        TYPE_BLOB,
        TYPE_NULL
    };

    sqlite3* create_or_open(const std::string& path);
    bool close(sqlite3* db);
    void finalize_all_stmts(sqlite3* db);

    bool execute_sql(sqlite3* db, const char* sql, std::string& out_error);
    bool execute_stmt(sqlite3* db, const char* sql, const std::vector<std::pair<int, std::string>>& values, std::string& out_error);
    bool execute_get_stmt(sqlite3* db, const char* sql, const std::vector<int>& types, std::vector<std::string>& out_values, std::string& out_error);
}
}
