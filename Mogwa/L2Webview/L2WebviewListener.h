// L2WebviewListener.h: L2WebviewListener 헤더 파일.
//

#pragma once

class L2WebviewListener {
public:
    virtual ~L2WebviewListener() = default;
public:
    virtual void OnWebviewMessageReceive(const std::wstring& message) = 0;
};