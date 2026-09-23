// L2WebviewController.h: L2WebviewController 구현 파일.
//

#include "pch.h"
#include "L2WebviewController.h"
#include "lib/string.h"

L2WebviewController::L2WebviewController(HWND hwnd)
{
    _parentHWND = hwnd;
}

L2WebviewController::~L2WebviewController()
{
    destroy();
}

////////////////////////////////////////////////////////////////////////////////

HRESULT L2WebviewController::OnWebViewControllerCompleted(HRESULT result, ICoreWebView2Controller* controller)
{
    if (FAILED(result)) {
        return result;
    }

    if (controller) {
        _controller = controller;

        RECT rect;
        GetClientRect(_parentHWND, &rect);
        _controller->put_Bounds(rect);

        if (FAILED(_controller->get_CoreWebView2(&_webview)) || !_webview) {
            return E_FAIL;
        }

        const HRESULT navigate_result = _webview->Navigate(_options.url.c_str());
        if (FAILED(navigate_result)) {
            return navigate_result;
        }

        return _webview->add_WebMessageReceived(
            MsRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [self = this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    return self->OnWebMessageReceived(sender, args);
                }
            ).Get(), &_webMessageReceivedToken
        );
    }
    return E_POINTER;
}

HRESULT L2WebviewController::OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args)
{
    LPWSTR msg = nullptr;
    if (SUCCEEDED(args->TryGetWebMessageAsString(&msg))) {
        std::wstring message(msg);

        if (message.starts_with(scheme::internal)) {
            message = message.substr(scheme::internal.size());
            if (_listener) {
                _listener->OnWebviewMessageReceive(message);
            }
        }
        CoTaskMemFree(msg);
    }

    return S_OK;
}

//////////////////////////////////////////////////////////////////////////////////

bool L2WebviewController::destroy()
{
    if (_webview) {
        _webview->remove_WebMessageReceived(_webMessageReceivedToken);
    }

    if (_controller) {
        _controller->Close();
    }

    _webview.Reset();
    _controller.Reset();
    return true;
}

void L2WebviewController::setBounds(RECT rect)
{
    if (_controller) {
        _controller->put_Bounds(rect);
    }
}

void L2WebviewController::openDevTools()
{
    if (!_webview) {
        return;
    }

    _webview->OpenDevToolsWindow();
}

void L2WebviewController::fn_javascript(std::wstring func, boost::json::object param)
{
    if (!_webview) {
        return;
    }

    std::wstring json = m1::string::string_to_wstring(boost::json::serialize(param));
    std::wstring script = func + L"(" + json + L");";

    _webview->ExecuteScript(script.c_str(),
        MsRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT hr, LPCWSTR result) -> HRESULT {
                return S_OK;
            }
        ).Get()
    );
}

void L2WebviewController::fn_javascript(std::wstring func, std::wstring param1)
{
    if (!_webview) {
        return;
    }

    std::wstring js = func + L"(" + param1 + L");";
    _webview->ExecuteScript(js.c_str(),
        MsRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT hr, LPCWSTR result) -> HRESULT {
                return S_OK;
            }
        ).Get()
    );
}
