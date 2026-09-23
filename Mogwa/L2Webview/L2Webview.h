// L2Webview.h: L2Webview 헤더 파일.
//

#pragma once

#include <wrl.h>
#include <Webview2.h>

#include "Core/Singleton.h"

namespace MsWRL = Microsoft::WRL;

class L2WebviewController;

class L2Webview : public Singleton<L2Webview> {
public:
    L2Webview();
    ~L2Webview();
public:
    struct ControllerOptions {
        std::wstring url;
    };
private:
    std::map<HWND, std::shared_ptr<L2WebviewController>> _map;

    MsWRL::ComPtr<ICoreWebView2Environment> _environment;
    bool _environment_requested = false;
public:
    bool createEnvironment();
    std::shared_ptr<L2WebviewController> createController(HWND parent, const ControllerOptions& option);
private:
    void initializeController(const std::shared_ptr<L2WebviewController>& controller);
    HRESULT OnWebViewEnvironmentCompleted(HRESULT result, ICoreWebView2Environment* env);
};
