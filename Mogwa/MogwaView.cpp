// MogwaView.cpp: CMogwaView 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "MogwaDoc.h"
#include "MogwaView.h"
#include "CommonMessages.h"

#include "Core/Singleton.h"
#include "lib/scope_exit.hpp"
#include "lib/string.h"
#include "L2Webview/L2WebviewController.h"
#include <boost/json.hpp>
#include <filesystem>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNCREATE(CMogwaView, CView)

CMogwaView::CMogwaView() noexcept
{
}

CMogwaView::~CMogwaView()
{
}

void CMogwaView::OnDraw(CDC* /*pDC*/)
{
}

BOOL CMogwaView::PreCreateWindow(CREATESTRUCT& cs)
{
    if (!CView::PreCreateWindow(cs)) return FALSE;
    cs.dwExStyle &= ~WS_EX_CLIENTEDGE;
    return TRUE;
}

BOOL CMogwaView::PreTranslateMessage(MSG* pMsg)
{
    if (pMsg->message == WM_KEYDOWN)
    {
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_MENU) & 0x8000) &&
            (GetKeyState(VK_SHIFT) & 0x8000) &&
            (pMsg->wParam == VK_F12))
        {
            if (_view) {
                _view->openDevTools();
            }
            return TRUE;
        }
    }

    return CView::PreTranslateMessage(pMsg);
}

/////////////////////////////////////////////////////////////////////////////

BEGIN_MESSAGE_MAP(CMogwaView, CView)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_MESSAGE(WM_REICVE_STOCK_REAL_TIME_PRICE, OnReceivePriceData)
    ON_MESSAGE(WM_RECEIVE_STREAM_STATUS, OnReceiveStreamStatus)
    ON_MESSAGE(WM_RECEIVE_PORTFOLIO_HISTORY, OnReceivePortfolioHistory)
    ON_MESSAGE(WM_RECEIVE_PERFORMANCE_HISTORY, OnReceivePerformanceHistory)
    ON_MESSAGE(WM_RECEIVE_QUOTE_SNAPSHOT, OnReceiveQuoteSnapshot)
    ON_MESSAGE(WM_EXECUTE_WEBVIEW_SCRIPT, OnExecuteWebviewScript)
END_MESSAGE_MAP()

int CMogwaView::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    __super::OnCreate(lpCreateStruct);

    L2Webview::Instance().createEnvironment();

    _listener = std::make_shared<webview_control>(this);

    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    const std::filesystem::path html_path =
        std::filesystem::path(module_path).parent_path() / L"View" / L"MainScreen.html";

    L2Webview::ControllerOptions options;
    options.url = L"file:///" + html_path.generic_wstring();
    _view = L2Webview::Instance().createController(this->GetSafeHwnd(), options);
    if (_view) {
        _view->setListener(_listener);
    }

    return 0;
}

void CMogwaView::OnSize(UINT nType, int cx, int cy)
{
    __super::OnSize(nType, cx, cy);

    if (_view) {
        RECT rect = { 0, 0, cx, cy };
        _view->setBounds(rect);
    }
}

/////////////////////////////////////////////////////////////////////////////

LRESULT CMogwaView::OnReceivePriceData(WPARAM wParam, LPARAM lParam)
{
    std::string* pStr = reinterpret_cast<std::string*>(lParam); 

    if (pStr) {
        m1::util::scope_exit exit([&pStr]() {
            delete pStr;
        });

        std::vector<std::string> v = m1::string::split(*pStr, L'|');
        if (v.size() != 4) {
            return 0L;
        }

        std::vector<std::string> data = m1::string::split(v[3], L'^');
        constexpr size_t fields_per_trade = 26;
        if (data.size() < fields_per_trade || data.size() % fields_per_trade != 0) {
            return 0L;
        }

        for (size_t offset = 0; offset + fields_per_trade <= data.size(); offset += fields_per_trade) {
            boost::json::object obj;
            obj.emplace("ticker", data[offset + 1]);
            try {
                obj.emplace("local_business_date", data[offset + 3]);
                obj.emplace("local_date", data[offset + 4]);
                obj.emplace("local_time", data[offset + 5]);
                obj.emplace("kr_date", data[offset + 6]);
                obj.emplace("kr_time", data[offset + 7]);
                obj.emplace("open", std::stod(data[offset + 8]));
                obj.emplace("high", std::stod(data[offset + 9]));
                obj.emplace("low", std::stod(data[offset + 10]));
                obj.emplace("price", std::stod(data[offset + 11]));
                obj.emplace("change_sign", data[offset + 12]);
                obj.emplace("change", std::stod(data[offset + 13]));
                obj.emplace("change_rate", std::stod(data[offset + 14]));
                obj.emplace("bid", std::stod(data[offset + 15]));
                obj.emplace("ask", std::stod(data[offset + 16]));
                obj.emplace("tick_volume", std::stod(data[offset + 19]));
                obj.emplace("volume", std::stod(data[offset + 20]));
                obj.emplace("amount", std::stod(data[offset + 21]));
                obj.emplace("strength", std::stod(data[offset + 24]));
                obj.emplace("market", data[offset + 25]);
                if (_view) _view->fn_javascript(L"update_stock_price", obj);
            }
            catch (const std::exception&) {
                TRACE(L"[MogwaView] invalid price payload: %hs\n", data[offset + 11].c_str());
            }
        }
    }
    return 0L;
}

LRESULT CMogwaView::OnReceiveStreamStatus(WPARAM wParam, LPARAM lParam)
{
    std::unique_ptr<std::string> detail(reinterpret_cast<std::string*>(lParam));
    if (!_view) return 0L;
    boost::json::object status;
    status.emplace("state", static_cast<int>(wParam));
    if (detail && !detail->empty()) {
        status.emplace("message", *detail);
    }
    _view->fn_javascript(L"load_stream_status", status);
    return 0L;
}


LRESULT CMogwaView::OnReceivePortfolioHistory(WPARAM, LPARAM lParam)
{
    std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
    if (!_view || !payload || payload->empty()) return 0L;

    boost::system::error_code error;
    const boost::json::value parsed = boost::json::parse(*payload, error);
    if (error || !parsed.is_object()) {
        TRACE(L"[MogwaView] invalid portfolio history payload\n");
        return 0L;
    }

    _view->fn_javascript(L"load_portfolio_history", parsed.as_object());
    return 0L;
}

LRESULT CMogwaView::OnReceivePerformanceHistory(WPARAM, LPARAM lParam)
{
    std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
    if (!_view || !payload || payload->empty()) return 0L;
    boost::system::error_code error;
    const boost::json::value parsed = boost::json::parse(*payload, error);
    if (error || !parsed.is_object()) {
        TRACE(L"[MogwaView] invalid performance history payload\n");
        return 0L;
    }
    _view->fn_javascript(L"load_performance_history", parsed.as_object());
    return 0L;
}

LRESULT CMogwaView::OnReceiveQuoteSnapshot(WPARAM, LPARAM lParam)
{
    std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
    if (!_view || !payload || payload->empty()) return 0L;
    boost::system::error_code error;
    const boost::json::value parsed = boost::json::parse(*payload, error);
    if (error || !parsed.is_object()) {
        TRACE(L"[MogwaView] invalid quote snapshot payload\n");
        return 0L;
    }
    _view->fn_javascript(L"load_realtime_quote", parsed.as_object());
    return 0L;
}

LRESULT CMogwaView::OnExecuteWebviewScript(WPARAM, LPARAM lParam)
{
    std::unique_ptr<webview_script_message> message(
        reinterpret_cast<webview_script_message*>(lParam));
    if (_view && message) {
        _view->fn_javascript(std::move(message->function_name), std::move(message->payload));
    }
    return 0L;
}
