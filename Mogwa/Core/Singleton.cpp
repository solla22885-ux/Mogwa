// L2WebviewController.h: L2WebviewController 구현 파일.
//

#include "pch.h"
#include "L2WebviewController.h"

L2WebviewController::L2WebviewController(L2Webview* parent)
{
    _parent = parent;
}

L2WebviewController::~L2WebviewController()
{
}


////////////////////////////////////////////////////////////////////////////////

bool L2WebviewController::createController(HWND parent, const ControllerOptions& option)
{

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

        if (SUCCEEDED(_controller->get_CoreWebView2(&_webview))) {
            _webview->Navigate(_options.url.data());
        }
    }
    return S_OK;
}

////////////////////////////////////////////////////////////////////////////////

void L2WebviewController::OnSize(UINT nType, int cx, int cy)
{
    if (_controller) {
        RECT rect = { 0, 0, cx, cy };
        _controller->put_Bounds(rect);
    }
}
