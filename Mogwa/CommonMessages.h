// chat_gpt_fn.h : 헤더 파일.
//

#pragma once
#include <Windows.h>
#include <boost/json.hpp>
#include <string>

///////////////////////////////////////////////////////////////////////////////

constexpr UINT WM_REICVE_STOCK_REAL_TIME_PRICE = WM_APP + 1;
constexpr UINT WM_RECEIVE_STREAM_STATUS = WM_APP + 2;
constexpr UINT WM_RECEIVE_PORTFOLIO_HISTORY = WM_APP + 3;
constexpr UINT WM_RECEIVE_PERFORMANCE_HISTORY = WM_APP + 4;
constexpr UINT WM_RECEIVE_QUOTE_SNAPSHOT = WM_APP + 5;
constexpr UINT WM_EXECUTE_WEBVIEW_SCRIPT = WM_APP + 6;

struct webview_script_message {
    std::wstring function_name;
    boost::json::object payload;
};

enum class stream_status : WPARAM {
    connecting = 0,
    connected = 1,
    disconnected = 2,
    failed = 3,
};
