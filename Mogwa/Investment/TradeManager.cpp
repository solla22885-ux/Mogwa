// TradeManager.cpp: TradeManager 구현 파일.

#include "pch.h"
#include "TradeManager.h"
#include "CommonMessages.h"

#include "lib/chat_gpt_fn.h"
#include <chrono>
#include <algorithm>
#include <boost/json.hpp>
#include <map>
#include <unordered_map>
#include <sstream>
#include <iomanip>

namespace {
    using steady_clock = std::chrono::steady_clock;
    constexpr auto balance_cache_ttl = std::chrono::seconds(15);
    constexpr auto exchange_rate_cache_ttl = std::chrono::minutes(30);
    constexpr auto advice_cache_ttl = std::chrono::minutes(30);

    std::string offset_date_key_tm(const std::string& date, int days)
    {
        if (date.size() != 8) return {};
        std::tm tm{};
        std::istringstream input(date);
        input >> std::get_time(&tm, "%Y%m%d");
        if (input.fail()) return {};
        tm.tm_isdst = -1;
        const std::time_t value = std::mktime(&tm);
        if (value == static_cast<std::time_t>(-1)) return {};
        const std::time_t shifted = value + static_cast<std::time_t>(days) * 24 * 60 * 60;
        std::tm out{};
        localtime_s(&out, &shifted);
        std::ostringstream result;
        result << std::put_time(&out, "%Y%m%d");
        return result.str();
    }

    std::string today_key()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        std::ostringstream result;
        result << std::put_time(&local, "%Y%m%d");
        return result.str();
    }
}

TradeManager::TradeManager(HWND parent) :
    _parent(parent),
    _client(std::make_shared<KISClient>()),
    _stream_client(std::make_unique<KISStreamClient>()),
    _db_manager(std::make_shared<DBManager>())
{
}

TradeManager::~TradeManager()
{
    stopMarketStream();
    if (_history_worker.joinable()) _history_worker.join();
    if (_performance_worker.joinable()) _performance_worker.join();
    if (_quote_worker.joinable()) _quote_worker.join();
}


////////////////////////////////////////////////////////////////////////////////

bool TradeManager::initialize(bool force_token_refresh)
{
    _settings = app_config::load();
    _credentials_available = _settings.has_kis_credentials();
    _last_error.clear();

    if (!_credentials_available) {
        return true;
    }

    if (!_client || !_db_manager) {
        _last_error = "필수 서비스가 생성되지 않았습니다.";
        return false;
    }

    if (!_db_manager->initialize()) {
        _last_error = "로컬 토큰 저장소를 초기화하지 못했습니다.";
        return false;
    }

    bool needs_token = true;
    if (!force_token_refresh && _db_manager->loadToken(_token)) {
        std::tm tm = {};
        std::istringstream ss(_token.access_token_expired);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (!ss.fail()) {
            const std::time_t expired_time = std::mktime(&tm);
            const std::time_t now = std::time(nullptr);
            // Renew ahead of expiry to avoid failing a request in flight.
            needs_token = std::difftime(expired_time, now) < 300.0;
        }
    }

    if (needs_token) {
        kis_domain::information_token new_token;
        if (!_client->request_token(_settings.kis_app_key, _settings.kis_app_secret, new_token)) {
            _last_error = "한국투자증권 인증 토큰 발급에 실패했습니다.";
            return false;
        }
        _token = std::move(new_token);
        if (!_db_manager->saveToken(_token)) {
            _last_error = "인증 토큰을 로컬 저장소에 저장하지 못했습니다.";
            return false;
        }
    }
    return true;
}

bool TradeManager::updateBalance()
{
    if (!_credentials_available) {
        _balance = {};
        return true;
    }
    if (_balance_updated_at != steady_clock::time_point{}
        && steady_clock::now() - _balance_updated_at <= balance_cache_ttl) {
        return true;
    }

    kis_domain::information_balance updated;
    if (!_client->request_balance(_settings.kis_app_key, _settings.kis_app_secret,
        _settings.kis_account_number, _settings.kis_account_product_code, _token, updated)) {
        _last_error = "잔고 조회에 실패했습니다.";
        return false;
    }
    _balance = std::move(updated);
    _balance_updated_at = steady_clock::now();
    return true;
}

bool TradeManager::updateExchangeRate()
{
    if (!_credentials_available) {
        _exchange_rate = 0.0;
        return true;
    }
    if (_exchange_rate > 0
        && _exchange_rate_updated_at != steady_clock::time_point{}
        && steady_clock::now() - _exchange_rate_updated_at <= exchange_rate_cache_ttl) {
        return true;
    }

    double exchange_rate = 0.0;
    if (_client->request_exchange_rate(_settings.kis_app_key, _settings.kis_app_secret,
        _settings.kis_account_number, _settings.kis_account_product_code, _token, exchange_rate)) {
        _exchange_rate = exchange_rate;
        _exchange_rate_updated_at = steady_clock::now();
        return true;
    }
    _last_error = "환율 조회에 실패했습니다.";
    return false;
}

bool TradeManager::runMarketStream()
{
    std::vector<kis_domain::balance_item1> subscriptions = _balance.stockList;
    subscriptions.insert(subscriptions.end(), _manual_stream_items.begin(), _manual_stream_items.end());
    subscriptions.insert(subscriptions.end(), _watch_stream_items.begin(), _watch_stream_items.end());
    if (!_stream_client || !_credentials_available || subscriptions.empty()) {
        return false;
    }

    const bool started = _stream_client->start(
        _settings.kis_app_key,
        _settings.kis_app_secret,
        std::move(subscriptions),
        [this](const std::string& message) { on_receive_price_data(message); },
        [this](int status, const std::string& detail) {
            auto message = std::make_unique<std::string>(detail);
            if (IsWindow(_parent) && PostMessage(_parent, WM_RECEIVE_STREAM_STATUS,
                static_cast<WPARAM>(status), reinterpret_cast<LPARAM>(message.get()))) {
                message.release();
            }
        });
    if (started) {
        PostMessage(_parent, WM_RECEIVE_STREAM_STATUS, static_cast<WPARAM>(stream_status::connecting), 0);
    }
    return started;
}

void TradeManager::setManualSubscriptions(
    const std::vector<std::pair<std::string, std::string>>& subscriptions, bool restart_stream)
{
    if (restart_stream) stopMarketStream();
    _manual_stream_items.clear();
    for (const auto& [ticker, exchange] : subscriptions) {
        if (ticker.empty()) continue;
        kis_domain::balance_item1 item;
        item.ovrs_pdno = ticker;
        item.ovrs_excg_cd = exchange.empty() ? "NASD" : exchange;
        _manual_stream_items.push_back(std::move(item));
    }
    if (restart_stream && _credentials_available) {
        runMarketStream();
    }
}

void TradeManager::setWatchSubscriptions(
    const std::vector<std::pair<std::string, std::string>>& subscriptions, bool restart_stream)
{
    if (restart_stream) stopMarketStream();
    _watch_stream_items.clear();
    for (const auto& [ticker, exchange] : subscriptions) {
        if (ticker.empty()) continue;
        kis_domain::balance_item1 item;
        item.ovrs_pdno = ticker;
        item.ovrs_excg_cd = exchange.empty() ? "NASD" : exchange;
        _watch_stream_items.push_back(std::move(item));
    }
    if (restart_stream && _credentials_available) runMarketStream();
}

bool TradeManager::restartMarketStream()
{
    stopMarketStream();
    return runMarketStream();
}


bool TradeManager::requestPortfolioHistory(std::vector<portfolio_holding> holdings)
{
    if (!_credentials_available || !_token.ready || holdings.empty()) {
        return false;
    }

    holdings.erase(std::remove_if(holdings.begin(), holdings.end(), [](const portfolio_holding& item) {
        return item.ticker.empty() || item.quantity <= 0;
    }), holdings.end());
    if (holdings.empty()) return false;

    bool expected = false;
    if (!_history_loading.compare_exchange_strong(expected, true)) {
        return false;
    }
    if (_history_worker.joinable()) {
        _history_worker.join();
    }

    const std::string app_key = _settings.kis_app_key;
    const std::string app_secret = _settings.kis_app_secret;
    const kis_domain::information_token token = _token;
    const HWND parent = _parent;

    _history_worker = std::thread([this, parent, app_key, app_secret, token, holdings = std::move(holdings)]() {
        try {
        struct aggregate_bar {
            double open = 0;
            double high = 0;
            double low = 0;
            double close = 0;
            size_t count = 0;
        };

        boost::json::object result;
        boost::json::array errors;
        std::map<std::string, aggregate_bar> aggregate;
        size_t reflected = 0;

        KISClient history_client;
        for (const auto& holding : holdings) {
            std::vector<kis_domain::minute_bar> bars;
            std::string resolved_exchange = holding.exchange;
            bool loaded = history_client.request_overseas_minute_bars(
                app_key, app_secret, token, resolved_exchange, holding.ticker, bars, 480);

            // 수동 등록 종목에서 거래소를 US/USA로 입력한 경우 실제 미국 거래소를 순서대로 시도합니다.
            std::string exchange_upper = holding.exchange;
            std::transform(exchange_upper.begin(), exchange_upper.end(), exchange_upper.begin(),
                [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (!loaded && (exchange_upper.empty() || exchange_upper == "US" || exchange_upper == "USA")) {
                for (const char* fallback_exchange : { "NASD", "NYSE", "AMEX" }) {
                    bars.clear();
                    if (history_client.request_overseas_minute_bars(
                        app_key, app_secret, token, fallback_exchange, holding.ticker, bars, 480)) {
                        loaded = true;
                        resolved_exchange = fallback_exchange;
                        break;
                    }
                }
            }

            if (!loaded) {
                boost::json::object error;
                error["ticker"] = holding.ticker;
                error["exchange"] = holding.exchange;
                error["message"] = history_client.getLastError();
                errors.push_back(std::move(error));
                continue;
            }

            ++reflected;
            for (const auto& bar : bars) {
                if (bar.kr_timestamp.empty()) continue;
                auto& total = aggregate[bar.kr_timestamp];
                total.open += bar.open * holding.quantity;
                total.high += bar.high * holding.quantity;
                total.low += bar.low * holding.quantity;
                total.close += bar.close * holding.quantity;
                ++total.count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }

        boost::json::array candles;
        const bool all_holdings_loaded = reflected == holdings.size();
        if (all_holdings_loaded) {
            for (const auto& [timestamp, bar] : aggregate) {
                // 현재 포트폴리오의 모든 종목이 같은 시각에 존재하는 경우만 사용합니다.
                // 일부 종목만 합산한 분봉을 그리면 실시간 평가액과 큰 갭이 생길 수 있습니다.
                if (bar.count != holdings.size()) continue;
                boost::json::object candle;
                candle["timestamp"] = timestamp;
                candle["open"] = bar.open;
                candle["high"] = (std::max)(bar.high, (std::max)(bar.open, bar.close));
                candle["low"] = (std::min)(bar.low, (std::min)(bar.open, bar.close));
                candle["close"] = bar.close;
                candles.push_back(std::move(candle));
            }
        }

        result["success"] = all_holdings_loaded && !candles.empty();
        result["candles"] = std::move(candles);
        result["reflected_count"] = static_cast<int64_t>(reflected);
        result["requested_count"] = static_cast<int64_t>(holdings.size());
        result["errors"] = std::move(errors);
        if (reflected == 0) {
            result["message"] = "과거 분봉을 불러오지 못했습니다.";
        } else if (reflected < holdings.size()) {
            result["message"] = std::format(
                "과거 분봉 일부 종목 조회 실패 ({}/{}). 불완전한 포트폴리오 차트는 표시하지 않습니다.",
                reflected, holdings.size());
        } else if (candles.empty()) {
            result["message"] = "모든 보유종목이 동시에 존재하는 과거 1분봉을 찾지 못했습니다.";
        } else {
            result["message"] = std::format("과거 분봉 {}종목 반영", reflected);
        }

        auto payload = std::make_unique<std::string>(boost::json::serialize(result));
        if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PORTFOLIO_HISTORY, 0,
            reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
        }
        catch (const std::exception& error) {
            boost::json::object result;
            result["success"] = false;
            result["message"] = std::format("과거 분봉 처리 중 오류가 발생했습니다: {}", error.what());
            result["candles"] = boost::json::array{};
            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PORTFOLIO_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
            }
        }
        catch (...) {
            boost::json::object result;
            result["success"] = false;
            result["message"] = "과거 분봉 처리 중 알 수 없는 오류가 발생했습니다.";
            result["candles"] = boost::json::array{};
            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PORTFOLIO_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
            }
        }
        _history_loading = false;
    });

    return true;
}

bool TradeManager::requestPortfolioPerformance(std::vector<portfolio_holding> holdings,
    const std::string& start_date, const std::string& end_date)
{
    if (!_credentials_available || !_token.ready || holdings.empty()
        || start_date.size() != 8 || end_date.size() != 8 || start_date > end_date) {
        return false;
    }

    holdings.erase(std::remove_if(holdings.begin(), holdings.end(), [](const portfolio_holding& item) {
        return item.ticker.empty() || item.quantity <= 0;
    }), holdings.end());
    if (holdings.empty()) return false;

    bool expected = false;
    if (!_performance_loading.compare_exchange_strong(expected, true)) return false;
    if (_performance_worker.joinable()) _performance_worker.join();

    const std::string app_key = _settings.kis_app_key;
    const std::string app_secret = _settings.kis_app_secret;
    const kis_domain::information_token token = _token;
    const HWND parent = _parent;

    _performance_worker = std::thread([this, parent, app_key, app_secret, token,
        start_date, end_date, holdings = std::move(holdings)]() {
        try {
            struct aggregate_bar {
                double open = 0;
                double high = 0;
                double low = 0;
                double close = 0;
                size_t count = 0;
            };
            struct component_info {
                portfolio_holding holding;
                std::string first_date;
                std::string last_date;
                double base_close = 0;
                double last_close = 0;
            };

            const std::string buffered_start = offset_date_key_tm(start_date, -10);
            std::map<std::string, aggregate_bar> aggregate;
            std::vector<component_info> components;
            boost::json::array errors;
            size_t reflected = 0;
            KISClient history_client;

            for (const auto& holding : holdings) {
                std::vector<kis_domain::daily_bar> bars;
                std::string resolved_exchange = holding.exchange;
                bool loaded = history_client.request_overseas_daily_bars(
                    app_key, app_secret, token, resolved_exchange, holding.ticker,
                    buffered_start.empty() ? start_date : buffered_start, end_date, bars, 2200);

                std::string exchange_upper = holding.exchange;
                std::transform(exchange_upper.begin(), exchange_upper.end(), exchange_upper.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (!loaded && (exchange_upper.empty() || exchange_upper == "US" || exchange_upper == "USA")) {
                    for (const char* fallback_exchange : { "NASD", "NYSE", "AMEX" }) {
                        bars.clear();
                        if (history_client.request_overseas_daily_bars(
                            app_key, app_secret, token, fallback_exchange, holding.ticker,
                            buffered_start.empty() ? start_date : buffered_start, end_date, bars, 2200)) {
                            loaded = true;
                            resolved_exchange = fallback_exchange;
                            break;
                        }
                    }
                }

                if (!loaded) {
                    boost::json::object error;
                    error["ticker"] = holding.ticker;
                    error["exchange"] = holding.exchange;
                    error["message"] = history_client.getLastError();
                    errors.push_back(std::move(error));
                    continue;
                }

                ++reflected;
                component_info component;
                component.holding = holding;
                for (const auto& bar : bars) {
                    if (bar.date.empty()) continue;
                    auto& total = aggregate[bar.date];
                    total.open += bar.open * holding.quantity;
                    total.high += bar.high * holding.quantity;
                    total.low += bar.low * holding.quantity;
                    total.close += bar.close * holding.quantity;
                    ++total.count;

                    if (bar.date < start_date) {
                        if (component.first_date.empty() || bar.date > component.first_date) {
                            component.first_date = bar.date;
                            component.base_close = bar.close;
                        }
                    }
                    else if (bar.date <= end_date) {
                        if (component.base_close <= 0) component.base_close = bar.open > 0 ? bar.open : bar.close;
                        if (component.last_date.empty() || bar.date > component.last_date) {
                            component.last_date = bar.date;
                            component.last_close = bar.close;
                        }
                    }
                }
                components.push_back(std::move(component));
                std::this_thread::sleep_for(std::chrono::milliseconds(140));
            }

            const bool all_loaded = reflected == holdings.size();
            const std::string today = today_key();
            if (all_loaded && end_date >= today) {
                bool all_current = true;
                aggregate_bar live{};
                for (const auto& holding : holdings) {
                    if (holding.current_price <= 0) { all_current = false; break; }
                    const double value = holding.current_price * holding.quantity;
                    live.open += value;
                    live.high += value;
                    live.low += value;
                    live.close += value;
                    ++live.count;
                }
                if (all_current && live.count == holdings.size()) aggregate[today] = live;
            }

            boost::json::array series;
            if (all_loaded) {
                for (const auto& [date, bar] : aggregate) {
                    if (bar.count != holdings.size()) continue;
                    boost::json::object item;
                    item["date"] = date;
                    item["open"] = bar.open;
                    item["high"] = (std::max)(bar.high, (std::max)(bar.open, bar.close));
                    item["low"] = (std::min)(bar.low, (std::min)(bar.open, bar.close));
                    item["close"] = bar.close;
                    item["in_range"] = date >= start_date && date <= end_date;
                    series.push_back(std::move(item));
                }
            }

            boost::json::array component_json;
            for (const auto& component : components) {
                if (component.base_close <= 0 || component.last_close <= 0) continue;
                boost::json::object item;
                item["ticker"] = component.holding.ticker;
                item["name"] = component.holding.name;
                item["exchange"] = component.holding.exchange;
                item["quantity"] = component.holding.quantity;
                item["start_date"] = start_date;
                item["end_date"] = component.last_date;
                item["start_price"] = component.base_close;
                item["end_price"] = component.last_close;
                item["pnl"] = (component.last_close - component.base_close) * component.holding.quantity;
                item["return_rate"] = (component.last_close / component.base_close - 1.0) * 100.0;
                component_json.push_back(std::move(item));
            }

            boost::json::object result;
            result["success"] = all_loaded && !series.empty();
            result["series"] = std::move(series);
            result["components"] = std::move(component_json);
            result["requested_count"] = static_cast<int64_t>(holdings.size());
            result["reflected_count"] = static_cast<int64_t>(reflected);
            result["start_date"] = start_date;
            result["end_date"] = end_date;
            result["errors"] = std::move(errors);
            result["message"] = !all_loaded
                ? std::format("성과 데이터 일부 종목 조회 실패 ({}/{})", reflected, holdings.size())
                : std::format("{}개 종목 성과 데이터를 불러왔습니다.", reflected);

            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PERFORMANCE_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
            }
        }
        catch (const std::exception& error) {
            boost::json::object result;
            result["success"] = false;
            result["message"] = std::format("성과 데이터 처리 중 오류가 발생했습니다: {}", error.what());
            result["series"] = boost::json::array{};
            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PERFORMANCE_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) payload.release();
        }
        catch (...) {
            boost::json::object result;
            result["success"] = false;
            result["message"] = "성과 데이터 처리 중 알 수 없는 오류가 발생했습니다.";
            result["series"] = boost::json::array{};
            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PERFORMANCE_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) payload.release();
        }
        _performance_loading = false;
    });
    return true;
}

bool TradeManager::requestRealtimeQuote(const std::string& ticker, const std::string& exchange)
{
    if (!_credentials_available || !_token.ready || ticker.empty()) return false;
    bool expected = false;
    if (!_quote_loading.compare_exchange_strong(expected, true)) return false;
    if (_quote_worker.joinable()) _quote_worker.join();

    const std::string app_key = _settings.kis_app_key;
    const std::string app_secret = _settings.kis_app_secret;
    const kis_domain::information_token token = _token;
    const HWND parent = _parent;

    _quote_worker = std::thread([this, parent, app_key, app_secret, token, ticker, exchange]() {
        boost::json::object result;
        try {
            KISClient client;
            kis_domain::overseas_quote quote;
            std::vector<kis_domain::minute_bar> bars;
            const bool quote_ok = client.request_overseas_quote(app_key, app_secret, token, exchange, ticker, quote);
            const std::string quote_error = quote_ok ? std::string{} : client.getLastError();
            const bool bars_ok = client.request_overseas_minute_bars(app_key, app_secret, token, exchange, ticker, bars, 240);
            const std::string bars_error = bars_ok ? std::string{} : client.getLastError();

            result["success"] = quote_ok || bars_ok;
            result["ticker"] = ticker;
            result["exchange"] = exchange;
            if (quote_ok) {
                boost::json::object q;
                q["price"] = quote.price;
                q["previous_close"] = quote.previous_close;
                q["change"] = quote.change;
                q["change_rate"] = quote.change_rate;
                q["change_sign"] = quote.change_sign;
                q["volume"] = quote.volume;
                q["amount"] = quote.amount;
                q["orderable"] = quote.orderable;
                result["quote"] = std::move(q);
            }

            boost::json::array candles;
            if (bars_ok) {
                for (const auto& bar : bars) {
                    boost::json::object item;
                    item["timestamp"] = bar.kr_timestamp;
                    item["open"] = bar.open;
                    item["high"] = bar.high;
                    item["low"] = bar.low;
                    item["close"] = bar.close;
                    candles.push_back(std::move(item));
                }
            }
            result["candles"] = std::move(candles);
            if (!quote_ok && !bars_ok) result["message"] = quote_error.empty() ? bars_error : quote_error;
            else if (!quote_ok) result["message"] = "실시간 스냅샷은 대기 중이며 최근 분봉을 표시합니다.";
            else if (!bars_ok) result["message"] = "현재가는 조회했지만 과거 1분봉을 불러오지 못했습니다.";
            else result["message"] = "현재가와 최근 1분봉을 불러왔습니다.";
        }
        catch (const std::exception& error) {
            result["success"] = false;
            result["message"] = std::format("종목 시세 조회 중 오류가 발생했습니다: {}", error.what());
        }

        auto payload = std::make_unique<std::string>(boost::json::serialize(result));
        if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_QUOTE_SNAPSHOT, 0,
            reinterpret_cast<LPARAM>(payload.get()))) payload.release();
        _quote_loading = false;
    });
    return true;
}

bool TradeManager::clearKisCredentials()
{
    stopMarketStream();
    if (!app_config::clear_kis_credentials()) {
        _last_error = "Windows 자격 증명 저장소에서 인증정보를 삭제하지 못했습니다.";
        return false;
    }
    _settings.kis_app_key.clear();
    _settings.kis_app_secret.clear();
    _settings.kis_account_number.clear();
    _settings.kis_account_product_code = "01";
    _token = {};
    _balance = {};
    _exchange_rate = 0.0;
    _balance_updated_at = {};
    _exchange_rate_updated_at = {};
    _advice_updated_at = {};
    _advice_signature.clear();
    _advice_cache.clear();
    _credentials_available = false;
    _last_error.clear();
    return true;
}

bool TradeManager::saveKisCredentials(const std::string& app_key, const std::string& app_secret,
    const std::string& account_number, const std::string& product_code)
{
    app_config::settings updated;
    updated.kis_app_key = app_key;
    updated.kis_app_secret = app_secret;
    updated.kis_account_number = account_number;
    updated.kis_account_product_code = product_code.empty() ? "01" : product_code;
    updated.openai_api_key = _settings.openai_api_key;

    if (!updated.has_kis_credentials()) {
        _last_error = "필수 인증정보를 모두 입력해 주세요.";
        return false;
    }
    kis_domain::information_token verified_token;
    if (!_client || !_client->request_token(updated.kis_app_key, updated.kis_app_secret, verified_token)) {
        const std::string detail = _client ? _client->getLastError() : std::string{};
        _last_error = detail.empty() ? "App Key와 App Secret을 확인하지 못했습니다." : detail;
        return false;
    }
    kis_domain::information_balance verified_balance;
    if (!_client->request_balance(updated.kis_app_key, updated.kis_app_secret,
        updated.kis_account_number, updated.kis_account_product_code, verified_token, verified_balance)) {
        _last_error = "계좌번호와 상품코드를 확인하지 못했습니다.";
        return false;
    }
    if (!app_config::store_kis_credentials(updated)) {
        _last_error = "Windows 자격 증명 저장소에 인증정보를 저장하지 못했습니다.";
        return false;
    }

    stopMarketStream();
    _settings = std::move(updated);
    _credentials_available = true;
    _last_error.clear();
    if (!_db_manager || !_db_manager->initialize()) {
        _last_error = "로컬 토큰 저장소를 초기화하지 못했습니다.";
        return false;
    }
    _token = std::move(verified_token);
    _balance = std::move(verified_balance);
    _balance_updated_at = steady_clock::now();
    _advice_updated_at = {};
    _advice_signature.clear();
    _advice_cache.clear();
    if (!_db_manager->saveToken(_token)) {
        _last_error = "인증 토큰을 로컬 저장소에 저장하지 못했습니다.";
        return false;
    }
    return true;
}

bool TradeManager::saveOpenAiApiKey(const std::string& api_key)
{
    if (api_key.empty()) {
        _last_error = "OpenAI API Key를 입력해 주세요.";
        return false;
    }

    if (!app_config::store_openai_api_key(api_key)) {
        _last_error = "Windows 자격 증명 저장소에 OpenAI API Key를 저장하지 못했습니다.";
        return false;
    }

    _settings.openai_api_key = api_key;
    _advice_updated_at = {};
    _advice_signature.clear();
    _advice_cache.clear();
    _last_error.clear();
    return true;
}

bool TradeManager::clearOpenAiApiKey()
{
    if (!app_config::clear_openai_api_key()) {
        _last_error = "Windows 자격 증명 저장소에서 OpenAI API Key를 삭제하지 못했습니다.";
        return false;
    }

    _settings.openai_api_key.clear();
    _advice_updated_at = {};
    _advice_signature.clear();
    _advice_cache.clear();
    _last_error.clear();
    return true;
}

bool TradeManager::requestChatGPTAdvice(std::string& output)
{ 
    if (_settings.openai_api_key.empty()) {
        output = "OpenAI API Key가 설정되지 않아 AI 분석을 건너뛰었습니다.";
        return false;
    }

    std::string signature;
    for (const auto& stock : _balance.stockList) {
        signature += std::format("{}|{}|{:.6f}|{:.6f};", stock.ovrs_excg_cd,
            stock.ovrs_pdno, stock.ovrs_cblc_qty, stock.now_pric2);
    }
    if (!_advice_cache.empty()
        && signature == _advice_signature
        && _advice_updated_at != steady_clock::time_point{}
        && steady_clock::now() - _advice_updated_at <= advice_cache_ttl) {
        output = _advice_cache;
        return true;
    }

    m1::gpt_fn::gpt_bot gpt;

    gpt.model = "gpt-4.1-mini"; // gpt-5.1
    gpt.api_key = _settings.openai_api_key;
    gpt.messages.push_back("당신은 주식 프로그램의 조언자로써 일을 할겁니다, 텍스트로만 답변해주고 특수문자는 사용하지 말아주세요. 그리고 700자 이내로 대답해주세요.");
    gpt.messages.push_back("주식 전문가로써 포트폴리오를 보고 오늘 날짜의 최신 정보를 보고 최신 동향을 기준으로 분석해주고 조언해주세요.");
    gpt.messages.push_back("저는 개인 투자자로서 지나친 안전보다는 어느정도 도전적인 포트폴리오라도 괜찮아요, 단 지나지게 큰 리스크는 피하고 싶어요.");
    for (auto& stock : _balance.stockList) {
        gpt.messages.push_back(std::format("종목명: {}, 현재 평가 금액: {:.2f}", stock.ovrs_item_name, stock.now_pric2));
    }
    if (!gpt.execute(output)) return false;
    _advice_signature = std::move(signature);
    _advice_cache = output;
    _advice_updated_at = steady_clock::now();
    return true;
}

void TradeManager::stopMarketStream()
{
    if (_stream_client) _stream_client->stop();
}

////////////////////////////////////////////////////////////////////////////////

void TradeManager::on_receive_price_data(const std::string& msg)
{
    auto message = std::make_unique<std::string>(msg);
    if (IsWindow(_parent) && PostMessage(_parent, WM_REICVE_STOCK_REAL_TIME_PRICE, 0,
        reinterpret_cast<LPARAM>(message.get()))) {
        message.release();
    }
}

////////////////////////////////////////////////////////////////////////////////
