// DBManager.h: DBManager 헤더 파일.
//

#pragma once

#include "Investment/KISDomain.h"

struct sqlite3;

class DBManager {
public:
    DBManager();
    ~DBManager();
private:
    sqlite3* _db;
    std::string _path;
public:
    void setPath(std::string path) { _path = path; }
    bool initialize();
    bool saveToken(const kis_domain::information_token& token);
    bool loadToken(kis_domain::information_token& outToken);
private:
    bool ensureTokenSchema(std::string& error);
};
