#include "pch.h"
#include "L2Webview.h"
#include "L2WebviewController.h"

L2Webview::L2Webview()
{   
}

L2Webview::~L2Webview()
{
    for (const auto& [hwnd, controller] : _map) {
        if (controller) {
            controller->destroy();
        }
    }
    _map.clear();
    _environment.Reset();
}

////////////////////////////////////////////////////////////////////////////////

bool L2Webview::createEnvironment()
{
    if (_environment || _environment_requested) {
        return false;
    }

    _environment_requested = true;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        MsRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [self = this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                return self->OnWebViewEnvironmentCompleted(result, env);
        }).Get());

    if (FAILED(result)) {
        _environment_requested = false;
        return false;
    }
    return true;
}

std::shared_ptr<L2WebviewController> L2Webview::createController(HWND parent, const ControllerOptions& option)
{
    if (!IsWindow(parent) || _map.find(parent) != _map.end()) {
        return nullptr;
    }

    std::shared_ptr webviewController = std::make_shared<L2WebviewController>(parent);
    webviewController->setOptions(option);
    _map.emplace(parent, webviewController);

    if (_environment) {
        initializeController(webviewController);
    }

    return webviewController;
}

void L2Webview::initializeController(const std::shared_ptr<L2WebviewController>& controller)
{
    if (!_environment || !controller) {
        return;
    }

    _environment->CreateCoreWebView2Controller(controller->getParentWindow(),
        MsRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [controller] (HRESULT result, ICoreWebView2Controller* native_controller) -> HRESULT {
                return controller->OnWebViewControllerCompleted(result, native_controller);
            }
        ).Get());
}

////////////////////////////////////////////////////////////////////////////////

HRESULT L2Webview::OnWebViewEnvironmentCompleted(HRESULT result, ICoreWebView2Environment* env)
{
    if (FAILED(result)) {
        _environment_requested = false;
        return result;
    }
    _environment = env;
    for (const auto& [window, controller] : _map) {
        initializeController(controller);
    }
    return S_OK;
}

////////////////////////////////////////////////////////////////////////////////
