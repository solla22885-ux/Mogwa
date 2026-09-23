// DBManager.cpp: DBManager 구현 파일.
//

#include "pch.h"
#include "DBManager.h"
#include "lib/sql_lite.h"
#include "lib/string.h"

#include <filesystem>

DBManager::DBManager() :
    _db(nullptr)
{
}

DBManager::~DBManager()
{
    if (_db) {
        m1::sql_lite::close(_db);
    }
}

////////////////////////////////////////////////////////////////////////////////

bool DBManager::initialize()
{
    if (_db) {
        return true;
    }

    PWSTR path = nullptr;
    if (_path.empty() && SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) == S_OK) {
        _path = m1::string::wstring_to_string(path) + "\\Mogwa";
        CoTaskMemFree(path);
    }

    if (_path.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(_path, ec);

    if (!ec) {
        _db = m1::sql_lite::create_or_open(_path + "\\localdata.db");
        if (!_db) {
            return false;
        }
        return true;
    }

    return false;
}

bool DBManager::saveToken(const kis_domain::information_token& token)
{
    if (!_db) {
        return false;
    }

    std::string error;
    if (ensureTokenSchema(error) != true) {
        return false;
    }

    const char* sql =
        "REPLACE INTO userinfo (id, access_token, access_token_expired, token_type, expires_in)"
        " VALUES (1, ?, ?, ?, ?);";

    std::vector<std::pair<int, std::string>> values;
    values.push_back(std::make_pair(m1::sql_lite::TYPE_TEXT, token.access_token.data()));
    values.push_back(std::make_pair(m1::sql_lite::TYPE_TEXT, token.access_token_expired.data()));
    values.push_back(std::make_pair(m1::sql_lite::TYPE_TEXT, token.token_type.data()));
    values.push_back(std::make_pair(m1::sql_lite::TYPE_INT, std::to_string(token.expires_in)));

    error.clear();

    bool res = false;
    res = m1::sql_lite::execute_stmt(_db, sql, values, error);
    return res;
}

bool DBManager::loadToken(kis_domain::information_token& outToken)
{
    if (!_db) {
        return false;
    }

    std::string error;
    if (!ensureTokenSchema(error)) {
        return false;
    }

    const char* sql =
        "SELECT access_token, access_token_expired, token_type, expires_in "
        "FROM userinfo WHERE id = 1;";

 
    bool res = false;

    std::vector<int> types{
        m1::sql_lite::TYPE_TEXT,
        m1::sql_lite::TYPE_TEXT,
        m1::sql_lite::TYPE_TEXT,
        m1::sql_lite::TYPE_INT
    };

    std::vector<std::string> values;
    res = m1::sql_lite::execute_get_stmt(_db, sql, types, values, error) && values.size() == 4;

    if (res) {
        outToken.ready = true;
        outToken.access_token = values[0];
        outToken.access_token_expired = values[1];
        outToken.token_type = values[2];
        try {
            outToken.expires_in = std::stoll(values[3]);
        }
        catch (const std::exception&) {
            return false;
        }
    }
    return res;
}

////////////////////////////////////////////////////////////////////////////////

bool DBManager::ensureTokenSchema(std::string& error)
{
    if (!_db) {
        return false;
    }

    static const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS userinfo (
          id INTEGER PRIMARY KEY CHECK(id=1),
          access_token TEXT NOT NULL,
          access_token_expired TEXT NOT NULL,
          token_type TEXT NOT NULL,
          expires_in INTEGER NOT NULL,
          updated_at TEXT DEFAULT (datetime('now','localtime'))
        );)SQL";
    return m1::sql_lite::execute_sql(_db, schema, error);
}
