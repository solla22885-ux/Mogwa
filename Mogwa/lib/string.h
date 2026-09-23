// string.h: string 헤더 파일.
//

#pragma once

namespace m1 {
    namespace string {
        std::string wstring_to_string(std::wstring_view wstr);
        std::wstring string_to_wstring(std::string_view str);

        std::vector<std::string> split(const std::string& s, char delim);
    }
}