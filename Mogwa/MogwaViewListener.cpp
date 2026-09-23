// MogwaViewListner.cpp: MogwaViewListner 구현 파일
//

#include "pch.h"
#include "MogwaView.h"
#include "L2Webview/L2WebviewController.h"
#include "Investment/TradeManager.h"
#include "Investment/KISJson.hpp"
#include "lib/string.h"
#include "CommonMessages.h"

namespace {
    using subscriptions = std::vector<std::pair<std::string, std::string>>;

    void post_webview_script(HWND parent, std::wstring function_name, boost::json::object payload)
    {
        auto message = std::make_unique<webview_script_message>();
        message->function_name = std::move(function_name);
        message->payload = std::move(payload);
        if (IsWindow(parent) && PostMessage(parent, WM_EXECUTE_WEBVIEW_SCRIPT, 0,
            reinterpret_cast<LPARAM>(message.get()))) {
            message.release();
        }
    }

    subscriptions parse_subscriptions(const boost::json::value& parsed)
    {
        subscriptions result;
        if (!parsed.is_array()) return result;
        for (const auto& value : parsed.as_array()) {
            if (!value.is_object()) continue;
            const auto& item = value.as_object();
            const auto* ticker = item.if_contains("ticker");
            const auto* exchange = item.if_contains("exchange");
            if (!ticker || !ticker->is_string()) continue;
            result.emplace_back(std::string(ticker->as_string()),
                exchange && exchange->is_string() ? std::string(exchange->as_string()) : "NASD");
        }
        return result;
    }

    void refresh_dashboard(const std::shared_ptr<TradeManager>& manager, HWND parent)
    {
        const bool initialized = manager->initialize();

        boost::json::object status;
        status["credentials_missing"] = !manager->hasKisCredentials();
        status["openai_key_configured"] = manager->hasOpenAiApiKey();
        status["message"] = !manager->hasKisCredentials()
            ? "한국투자증권 인증정보가 없습니다. API 설정에서 입력해 주세요."
            : manager->getLastError();
        post_webview_script(parent, L"load_runtime_status", std::move(status));

        if (!initialized || !manager->updateBalance()) {
            const std::string error_message = manager->getLastError().empty()
                ? "한국투자증권 데이터를 불러오지 못했습니다."
                : manager->getLastError();

            boost::json::object error;
            error["comment"] = error_message;
            post_webview_script(parent, L"load_advice", std::move(error));

            // Do not leave the status spinner at "실시간 연결 대기" when the
            // prerequisite REST requests failed before the WebSocket could start.
            boost::json::object stream_state;
            stream_state["state"] = static_cast<int>(stream_status::failed);
            stream_state["message"] = error_message;
            post_webview_script(parent, L"load_stream_status", std::move(stream_state));
            return;
        }

        // Exchange-rate lookup is useful for KRW conversion, but it must not block
        // positions or realtime prices. Keep the last known rate (if any) and let
        // the rest of the dashboard start even when this auxiliary request fails.
        const bool exchange_loaded = manager->updateExchangeRate();
        const std::string exchange_error = exchange_loaded ? std::string{} : manager->getLastError();

        boost::json::object exchange;
        exchange["rate"] = manager->getExchangeRate();
        post_webview_script(parent, L"load_exchange_rate", std::move(exchange));
        post_webview_script(parent, L"load_balance", kis_json::to_json(manager->getBalance()));

        // Start realtime streaming immediately after the balance/subscription list is ready.
        // Previously requestChatGPTAdvice() ran first, so a slow AI request left the UI at
        // "실시간 연결 대기" even though the WebSocket had not been started yet.
        if (manager->hasKisCredentials() && !manager->runMarketStream()) {
            boost::json::object stream_state;
            stream_state["state"] = static_cast<int>(stream_status::failed);
            stream_state["message"] = "구독할 종목이 없거나 실시간 연결을 시작하지 못했습니다.";
            post_webview_script(parent, L"load_stream_status", std::move(stream_state));
        }

        std::string comment;
        if (!manager->hasKisCredentials()) {
            comment = "한국투자증권 인증정보를 입력하면 실제 보유 데이터를 불러올 수 있습니다.";
        }
        else if (!manager->requestChatGPTAdvice(comment) && comment.empty()) {
            comment = "AI 분석을 불러오지 못했습니다. 잠시 후 다시 시도해 주세요.";
        }
        if (!exchange_error.empty()) {
            const std::string warning = "환율을 갱신하지 못했습니다: " + exchange_error;
            comment = comment.empty() ? warning : warning + "\n\n" + comment;
        }
        boost::json::object advice;
        advice["comment"] = comment;
        post_webview_script(parent, L"load_advice", std::move(advice));
    }
}

CMogwaView::webview_control::webview_control(CMogwaView* p)
    : _parent(p)
    , _manager(std::make_shared<TradeManager>(p->GetSafeHwnd()))
    , _operation_mutex(std::make_shared<std::mutex>())
{
}

CMogwaView::webview_control::~webview_control()
{
}

//////////////////////////////////////////////////////////////////////////////////

void CMogwaView::webview_control::OnWebviewMessageReceive(const std::wstring& message)
{
    if (message.starts_with(webview_message::show_native_message)) {
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::show_native_message.size()));
            const boost::json::object input = boost::json::parse(payload).as_object();

            const auto read_string = [&input](const char* key, const char* fallback) -> std::string {
                const auto* value = input.if_contains(key);
                return value && value->is_string() ? std::string(value->as_string()) : std::string(fallback);
            };

            const std::wstring title = m1::string::string_to_wstring(
                read_string("title", "Mogwa"));
            const std::wstring body = m1::string::string_to_wstring(
                read_string("message", "요청을 처리하는 중 오류가 발생했습니다."));
            const std::string icon = read_string("icon", "warning");

            UINT flags = MB_OK | MB_TASKMODAL;
            if (icon == "error") flags |= MB_ICONERROR;
            else if (icon == "info") flags |= MB_ICONINFORMATION;
            else flags |= MB_ICONWARNING;

            ::MessageBoxW(_parent->GetSafeHwnd(), body.c_str(), title.c_str(), flags);
        }
        catch (const std::exception& error) {
            TRACE(L"[WebView] native message parse failed: %hs\n", error.what());
            ::MessageBoxW(_parent->GetSafeHwnd(),
                L"요청을 처리하는 중 오류가 발생했습니다.",
                L"Mogwa", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        }
        return;
    }

    else if (message == webview_message::retry_kis_stream) {
        const auto manager = _manager;
        const auto operation_mutex = _operation_mutex;
        const HWND parent = _parent->GetSafeHwnd();
        std::thread([manager, operation_mutex, parent]() {
            std::lock_guard lock(*operation_mutex);
            if (!manager || !manager->restartMarketStream()) {
                boost::json::object status;
                status["state"] = 3;
                status["message"] = manager && !manager->hasKisCredentials()
                    ? "저장된 한국투자증권 인증정보가 없습니다."
                    : "구독할 보유종목이 없거나 실시간 연결을 시작하지 못했습니다.";
                post_webview_script(parent, L"load_stream_status", std::move(status));
            }
        }).detach();
        return;
    }

    else if (message == webview_message::clear_kis_credentials) {
        const auto manager = _manager;
        const auto operation_mutex = _operation_mutex;
        const HWND parent = _parent->GetSafeHwnd();
        std::thread([manager, operation_mutex, parent]() {
            std::lock_guard lock(*operation_mutex);
            const bool cleared = manager && manager->clearKisCredentials();
            boost::json::object result;
            result["success"] = cleared;
            result["message"] = cleared
                ? "저장된 한국투자증권 인증정보를 삭제했습니다."
                : (manager ? manager->getLastError() : "설정 관리자를 사용할 수 없습니다.");
            post_webview_script(parent, L"load_credentials_result", std::move(result));
            if (cleared) refresh_dashboard(manager, parent);
        }).detach();
        return;
    }

    else if (message == webview_message::clear_openai_api_key) {
        const bool cleared = _manager && _manager->clearOpenAiApiKey();
        boost::json::object result;
        result["success"] = cleared;
        result["configured"] = _manager && _manager->hasOpenAiApiKey();
        result["message"] = cleared
            ? "저장된 OpenAI API Key를 삭제했습니다."
            : (_manager ? _manager->getLastError() : "설정 관리자를 사용할 수 없습니다.");
        _parent->_view->fn_javascript(L"load_openai_key_result", result);
        return;
    }

    else if (message.starts_with(webview_message::save_openai_api_key)) {
        boost::json::object result;
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::save_openai_api_key.size()));
            const boost::json::object input = boost::json::parse(payload).as_object();
            const auto* value = input.if_contains("api_key");
            const std::string api_key = value && value->is_string()
                ? std::string(value->as_string())
                : std::string{};

            const bool saved = _manager && _manager->saveOpenAiApiKey(api_key);
            result["success"] = saved;
            result["configured"] = _manager && _manager->hasOpenAiApiKey();
            result["message"] = saved
                ? "OpenAI API Key를 안전하게 저장했습니다."
                : (_manager ? _manager->getLastError() : "설정 관리자를 사용할 수 없습니다.");
        }
        catch (const std::exception&) {
            result["success"] = false;
            result["configured"] = _manager && _manager->hasOpenAiApiKey();
            result["message"] = "OpenAI API Key 형식을 확인해 주세요.";
        }

        _parent->_view->fn_javascript(L"load_openai_key_result", result);
        return;
    }

    else if (message.starts_with(webview_message::sync_manual_holdings)) {
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::sync_manual_holdings.size()));
            auto items = parse_subscriptions(boost::json::parse(payload));
            const auto manager = _manager;
            const auto operation_mutex = _operation_mutex;
            std::thread([manager, operation_mutex, items = std::move(items)]() {
                std::lock_guard lock(*operation_mutex);
                if (manager) manager->setManualSubscriptions(items);
            }).detach();
        }
        catch (const std::exception& error) {
            TRACE(L"[TradeManager] manual subscription parse failed: %hs\n", error.what());
        }
        return;
    }

    else if (message.starts_with(webview_message::sync_realtime_watchlist)) {
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::sync_realtime_watchlist.size()));
            auto items = parse_subscriptions(boost::json::parse(payload));
            const auto manager = _manager;
            const auto operation_mutex = _operation_mutex;
            std::thread([manager, operation_mutex, items = std::move(items)]() {
                std::lock_guard lock(*operation_mutex);
                if (manager) manager->setWatchSubscriptions(items);
            }).detach();
        }
        catch (const std::exception& error) {
            TRACE(L"[TradeManager] realtime watchlist parse failed: %hs\n", error.what());
        }
        return;
    }


    else if (message.starts_with(webview_message::load_portfolio_history)) {
        boost::json::object response;
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::load_portfolio_history.size()));
            const boost::json::value parsed = boost::json::parse(payload);
            std::vector<TradeManager::portfolio_holding> holdings;
            if (parsed.is_array()) {
                for (const auto& value : parsed.as_array()) {
                    if (!value.is_object()) continue;
                    const auto& item = value.as_object();
                    const auto* ticker = item.if_contains("ticker");
                    const auto* exchange = item.if_contains("exchange");
                    const auto* quantity = item.if_contains("quantity");
                    if (!ticker || !ticker->is_string() || !quantity) continue;

                    double qty = 0.0;
                    if (quantity->is_double()) qty = quantity->as_double();
                    else if (quantity->is_int64()) qty = static_cast<double>(quantity->as_int64());
                    else if (quantity->is_uint64()) qty = static_cast<double>(quantity->as_uint64());
                    else if (quantity->is_string()) {
                        try { qty = std::stod(std::string(quantity->as_string())); } catch (...) {}
                    }
                    if (qty <= 0) continue;

                    TradeManager::portfolio_holding holding;
                    holding.ticker = std::string(ticker->as_string());
                    holding.exchange = exchange && exchange->is_string() ? std::string(exchange->as_string()) : "NASD";
                    holding.quantity = qty;
                    if (const auto* name = item.if_contains("name"); name && name->is_string()) holding.name = std::string(name->as_string());
                    if (const auto* current = item.if_contains("current_price")) {
                        if (current->is_double()) holding.current_price = current->as_double();
                        else if (current->is_int64()) holding.current_price = static_cast<double>(current->as_int64());
                        else if (current->is_uint64()) holding.current_price = static_cast<double>(current->as_uint64());
                    }
                    holdings.push_back(std::move(holding));
                }
            }

            const bool started = _manager && _manager->requestPortfolioHistory(std::move(holdings));
            if (!started) {
                response["success"] = false;
                response["message"] = _manager && !_manager->hasKisCredentials()
                    ? "한국투자증권 인증정보가 없어 과거 분봉을 조회할 수 없습니다."
                    : "과거 분봉 조회가 이미 진행 중이거나 보유 종목이 없습니다.";
                response["candles"] = boost::json::array{};
                _parent->_view->fn_javascript(L"load_portfolio_history", response);
            }
        }
        catch (const std::exception& error) {
            response["success"] = false;
            response["message"] = "과거 분봉 요청 형식을 확인해 주세요.";
            response["candles"] = boost::json::array{};
            TRACE(L"[TradeManager] portfolio history request parse failed: %hs\n", error.what());
            _parent->_view->fn_javascript(L"load_portfolio_history", response);
        }
        return;
    }

    else if (message.starts_with(webview_message::load_performance_history)) {
        boost::json::object response;
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::load_performance_history.size()));
            const boost::json::object input = boost::json::parse(payload).as_object();
            const auto read_string = [&input](const char* key) -> std::string {
                const auto* value = input.if_contains(key);
                return value && value->is_string() ? std::string(value->as_string()) : std::string{};
            };

            std::vector<TradeManager::portfolio_holding> holdings;
            const auto* items = input.if_contains("holdings");
            if (items && items->is_array()) {
                for (const auto& value : items->as_array()) {
                    if (!value.is_object()) continue;
                    const auto& item = value.as_object();
                    const auto* ticker = item.if_contains("ticker");
                    const auto* quantity = item.if_contains("quantity");
                    if (!ticker || !ticker->is_string() || !quantity) continue;

                    double qty = 0.0;
                    if (quantity->is_double()) qty = quantity->as_double();
                    else if (quantity->is_int64()) qty = static_cast<double>(quantity->as_int64());
                    else if (quantity->is_uint64()) qty = static_cast<double>(quantity->as_uint64());
                    if (qty <= 0) continue;

                    TradeManager::portfolio_holding holding;
                    holding.ticker = std::string(ticker->as_string());
                    if (const auto* exchange = item.if_contains("exchange"); exchange && exchange->is_string()) holding.exchange = std::string(exchange->as_string());
                    else holding.exchange = "NASD";
                    if (const auto* name = item.if_contains("name"); name && name->is_string()) holding.name = std::string(name->as_string());
                    holding.quantity = qty;
                    if (const auto* price = item.if_contains("current_price")) {
                        if (price->is_double()) holding.current_price = price->as_double();
                        else if (price->is_int64()) holding.current_price = static_cast<double>(price->as_int64());
                        else if (price->is_uint64()) holding.current_price = static_cast<double>(price->as_uint64());
                    }
                    holdings.push_back(std::move(holding));
                }
            }

            const bool started = _manager && _manager->requestPortfolioPerformance(
                std::move(holdings), read_string("start_date"), read_string("end_date"));
            if (!started) {
                response["success"] = false;
                response["message"] = _manager && !_manager->hasKisCredentials()
                    ? "한국투자증권 인증정보가 없어 성과 데이터를 조회할 수 없습니다."
                    : "성과 데이터 조회가 이미 진행 중이거나 요청 기간을 확인해 주세요.";
                response["series"] = boost::json::array{};
                _parent->_view->fn_javascript(L"load_performance_history", response);
            }
        }
        catch (const std::exception& error) {
            response["success"] = false;
            response["message"] = "성과 데이터 요청 형식을 확인해 주세요.";
            response["series"] = boost::json::array{};
            TRACE(L"[TradeManager] performance history parse failed: %hs\n", error.what());
            _parent->_view->fn_javascript(L"load_performance_history", response);
        }
        return;
    }

    else if (message.starts_with(webview_message::request_realtime_quote)) {
        boost::json::object response;
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::request_realtime_quote.size()));
            const boost::json::object input = boost::json::parse(payload).as_object();
            const auto* ticker = input.if_contains("ticker");
            const auto* exchange = input.if_contains("exchange");
            const std::string ticker_text = ticker && ticker->is_string() ? std::string(ticker->as_string()) : std::string{};
            const std::string exchange_text = exchange && exchange->is_string() ? std::string(exchange->as_string()) : "NASD";
            const bool started = _manager && _manager->requestRealtimeQuote(ticker_text, exchange_text);
            if (!started) {
                response["success"] = false;
                response["ticker"] = ticker_text;
                response["exchange"] = exchange_text;
                response["message"] = _manager && !_manager->hasKisCredentials()
                    ? "한국투자증권 인증정보가 없어 시세를 조회할 수 없습니다."
                    : "다른 시세 조회가 진행 중이거나 티커를 확인해 주세요.";
                _parent->_view->fn_javascript(L"load_realtime_quote", response);
            }
        }
        catch (const std::exception& error) {
            response["success"] = false;
            response["message"] = "실시간 시세 요청 형식을 확인해 주세요.";
            TRACE(L"[TradeManager] realtime quote parse failed: %hs\n", error.what());
            _parent->_view->fn_javascript(L"load_realtime_quote", response);
        }
        return;
    }

    else if (message.starts_with(webview_message::save_kis_credentials)) {
        try {
            const std::string payload = m1::string::wstring_to_string(
                message.substr(webview_message::save_kis_credentials.size()));
            const boost::json::object input = boost::json::parse(payload).as_object();
            const auto read_string = [&input](const char* key) -> std::string {
                const auto* value = input.if_contains(key);
                return value && value->is_string() ? std::string(value->as_string()) : std::string{};
            };

            const std::string app_key = read_string("app_key");
            const std::string app_secret = read_string("app_secret");
            const std::string account_number = read_string("account_number");
            const std::string product_code = read_string("product_code");
            const auto manager = _manager;
            const auto operation_mutex = _operation_mutex;
            const HWND parent = _parent->GetSafeHwnd();
            std::thread([manager, operation_mutex, parent, app_key, app_secret, account_number, product_code]() {
                std::lock_guard lock(*operation_mutex);
                const bool saved = manager && manager->saveKisCredentials(
                    app_key, app_secret, account_number, product_code);
                boost::json::object result;
                result["success"] = saved;
                result["message"] = saved
                    ? "인증정보를 안전하게 저장했습니다. 한국투자증권 API에 연결합니다."
                    : (manager ? manager->getLastError() : "설정 관리자를 사용할 수 없습니다.");
                post_webview_script(parent, L"load_credentials_result", std::move(result));
                if (saved) refresh_dashboard(manager, parent);
            }).detach();
        }
        catch (const std::exception&) {
            boost::json::object result;
            result["success"] = false;
            result["message"] = "인증정보 형식을 확인해 주세요.";
            _parent->_view->fn_javascript(L"load_credentials_result", result);
        }
        return;
    }

    else if (message.starts_with(scheme::document_load)) {
        if (!_manager) return;
        subscriptions manual_items;
        subscriptions watch_items;
        try {
            const size_t separator = message.find(L'\n');
            if (separator != std::wstring::npos && separator + 1 < message.size()) {
                const std::string payload = m1::string::wstring_to_string(message.substr(separator + 1));
                const boost::json::value parsed = boost::json::parse(payload);
                if (parsed.is_object()) {
                    const auto& input = parsed.as_object();
                    if (const auto* manual = input.if_contains("manual")) {
                        manual_items = parse_subscriptions(*manual);
                    }
                    if (const auto* watch = input.if_contains("watch")) {
                        watch_items = parse_subscriptions(*watch);
                    }
                }
            }
        }
        catch (const std::exception& error) {
            TRACE(L"[TradeManager] startup subscription parse failed: %hs\n", error.what());
        }

        const auto manager = _manager;
        const auto operation_mutex = _operation_mutex;
        const HWND parent = _parent->GetSafeHwnd();
        std::thread([manager, operation_mutex, parent,
            manual_items = std::move(manual_items), watch_items = std::move(watch_items)]() {
            std::lock_guard lock(*operation_mutex);
            manager->stopMarketStream();
            manager->setManualSubscriptions(manual_items, false);
            manager->setWatchSubscriptions(watch_items, false);
            refresh_dashboard(manager, parent);
        }).detach();
    }
}
