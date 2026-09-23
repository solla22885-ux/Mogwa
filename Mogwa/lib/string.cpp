// string.cpp: string 구현 파일.
//

#include "pch.h"
#include "string.h"

namespace m1 {
    namespace string {

        std::string wstring_to_string(std::wstring_view wstr)
        {
            if (wstr.empty()) return {};
            
            int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};
            
            std::string result(size, '\0');

            WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        std::wstring string_to_wstring(std::string_view str)
        {
            if (str.empty()) return {};
            
            int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), static_cast<int>(str.size()), nullptr, 0 );

            if (size <= 0) return {};

            std::wstring result(size, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), static_cast<int>(str.size()), result.data(), size);
            return result;
        }

        std::vector<std::string> split(const std::string& s, char delim) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == delim) {
                    out.push_back(cur);
                    cur.clear();
                }
                else {
                    cur.push_back(c);
                }
            }
            out.push_back(cur);
            return out;
        }

    }
}