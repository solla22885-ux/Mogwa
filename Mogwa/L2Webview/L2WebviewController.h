// L2WebviewController.h: L2WebviewController 헤더 파일.
//

#pragma once

#include <wrl.h>
#include <Webview2.h>
#include <boost/json.hpp>

#include "L2Webview.h"
#include "L2WebviewListener.h"


namespace MsRL = Microsoft::WRL;

namespace scheme {
    inline constexpr std::wstring_view internal = L"apps://internal/";
}

class L2WebviewController {
public:
    L2WebviewController(HWND hwnd);
    ~L2WebviewController();
private:
    HWND _parentHWND;
    L2Webview::ControllerOptions _options;

    MsRL::ComPtr<ICoreWebView2> _webview;
    MsRL::ComPtr<ICoreWebView2Controller> _controller;
    std::shared_ptr<L2WebviewListener> _listener;

    EventRegistrationToken _webMessageReceivedToken{};
public:
    bool destroy();
    void setListener(std::shared_ptr<L2WebviewListener> listener) { _listener = listener; }
    void setOptions(const L2Webview::ControllerOptions& option) { _options = option; }
    void setBounds(RECT rect);
    void openDevTools();
    HWND getParentWindow() const { return _parentHWND; }

    void fn_javascript(std::wstring func, boost::json::object param);
    void fn_javascript(std::wstring func, std::wstring param1);
public:
    HRESULT OnWebViewControllerCompleted(HRESULT result, ICoreWebView2Controller* controller);
    HRESULT OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args);
};
