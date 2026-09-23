// TradeManager.cpp: TradeManager 구현 파일.

#include "pch.h"
#include "TradeManager.h"
#include "CommonMessages.h"

#include "lib/chat_gpt_fn.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <ctime>
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

    bool is_us_daytime_session_kst()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_s(&utc, &now);
        const int korea_hour = (utc.tm_hour + 9) % 24;
        const int minutes = korea_hour * 60 + utc.tm_min;
        return minutes >= 10 * 60 && minutes < 18 * 60;
    }

    std::string realtime_quote_exchange(const std::string& exchange)
    {
        if (!is_us_daytime_session_kst()) return exchange;

        std::string upper = exchange;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (upper == "NASD" || upper == "NASDAQ" || upper == "NAS" || upper == "BAQ") return "BAQ";
        if (upper == "NYSE" || upper == "NYS" || upper == "BAY") return "BAY";
        if (upper == "AMEX" || upper == "AMS" || upper == "BAA") return "BAA";
        return exchange;
    }


    constexpr auto token_refresh_margin = std::chrono::minutes(5);

    bool parse_kis_token_expiry(const std::string& value, std::time_t& expiry_utc)
    {
        if (value.empty()) return false;
        std::tm kst{};
        std::istringstream input(value);
        input >> std::get_time(&kst, "%Y-%m-%d %H:%M:%S");
        if (input.fail()) return false;

        // KIS returns access_token_token_expired in Korea Standard Time. Interpret the
        // fields as UTC first and subtract KST offset so the check is independent of
        // the Windows machine's configured time zone.
        const std::time_t kst_as_utc = _mkgmtime(&kst);
        if (kst_as_utc == static_cast<std::time_t>(-1)) return false;
        expiry_utc = kst_as_utc - 9 * 60 * 60;
        return true;
    }

    bool token_needs_refresh(const kis_domain::information_token& token)
    {
        if (!token.ready || token.access_token.empty()) return true;
        std::time_t expiry = 0;
        if (!parse_kis_token_expiry(token.access_token_expired, expiry)) return true;
        const std::time_t now = std::time(nullptr);
        return std::difftime(expiry, now) <=
            std::chrono::duration_cast<std::chrono::seconds>(token_refresh_margin).count();
    }

    std::string masked_app_key(const std::string& key)
    {
        if (key.empty()) return "<empty>";
        if (key.size() <= 8) return std::string(key.size(), '*') + " len=" + std::to_string(key.size());
        return key.substr(0, 4) + "..." + key.substr(key.size() - 4)
            + " len=" + std::to_string(key.size());
    }

    bool token_matches_app_key(const kis_domain::information_token& token, const std::string& app_key)
    {
        return !token.app_key_tag.empty()
            && token.app_key_tag == kis_domain::make_app_key_tag(app_key);
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

bool TradeManager::requestAndStoreAccessTokenLocked()
{
    if (!_client || !_db_manager) {
        _last_error = "필수 인증 서비스가 생성되지 않았습니다.";
        return false;
    }

    KISClient auth_client;
    kis_domain::information_token new_token;
    if (!auth_client.request_token(_settings.kis_app_key, _settings.kis_app_secret, new_token)) {
        const std::string detail = auth_client.getLastError();
        const std::string key_hint = masked_app_key(_settings.kis_app_key);
        _last_error = "[토큰 발급 단계, AppKey " + key_hint + "] "
            + (detail.empty() ? "한국투자증권 인증 토큰 발급에 실패했습니다." : detail);
        return false;
    }

    if (!_db_manager->saveToken(new_token)) {
        _last_error = "인증 토큰을 로컬 저장소에 저장하지 못했습니다.";
        return false;
    }

    _token = std::move(new_token);
    TRACE(L"[TradeManager] KIS access token refreshed. expires=%hs\n",
        _token.access_token_expired.c_str());
    return true;
}

bool TradeManager::ensureAccessToken(bool force_refresh)
{
    if (!_credentials_available) return false;

    std::scoped_lock token_lock(_token_mutex);
    if (!force_refresh && !token_needs_refresh(_token)) {
        return true;
    }
    return requestAndStoreAccessTokenLocked();
}

bool TradeManager::refreshAccessTokenAfterExpiration(
    const kis_domain::information_token& rejected_token, std::string* error)
{
    std::scoped_lock token_lock(_token_mutex);

    // Another worker may already have refreshed the shared token while this request
    // was in flight. In that case reuse it instead of issuing another token request.
    if (_token.ready && _token.access_token != rejected_token.access_token
        && !token_needs_refresh(_token)) {
        if (error) error->clear();
        return true;
    }

    TRACE(L"[TradeManager] KIS server reported expired access token; refreshing.\n");
    const bool refreshed = requestAndStoreAccessTokenLocked();
    if (error) *error = refreshed ? std::string{} : _last_error;
    return refreshed;
}

kis_domain::information_token TradeManager::accessTokenSnapshot() const
{
    std::scoped_lock token_lock(_token_mutex);
    return _token;
}

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

    if (!force_token_refresh) {
        kis_domain::information_token cached_token;
        if (_db_manager->loadToken(cached_token)) {
            if (token_matches_app_key(cached_token, _settings.kis_app_key)
                && !token_needs_refresh(cached_token)) {
                std::scoped_lock token_lock(_token_mutex);
                _token = std::move(cached_token);
                TRACE(L"[TradeManager] Reusing cached KIS token for matching AppKey.\n");
                return true;
            }
            TRACE(L"[TradeManager] Discarding cached KIS token: AppKey mismatch, legacy cache, or expiration.\n");
            _db_manager->clearToken();
        }
    }

    return ensureAccessToken(true);
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

    if (!ensureAccessToken()) return false;

    KISClient client;
    kis_domain::information_token token = accessTokenSnapshot();
    kis_domain::information_balance updated;
    bool loaded = client.request_balance(_settings.kis_app_key, _settings.kis_app_secret,
        _settings.kis_account_number, _settings.kis_account_product_code, token, updated);
    if (!loaded && client.isLastErrorTokenExpired()) {
        if (!refreshAccessTokenAfterExpiration(token)) return false;
        token = accessTokenSnapshot();
        loaded = client.request_balance(_settings.kis_app_key, _settings.kis_app_secret,
            _settings.kis_account_number, _settings.kis_account_product_code, token, updated);
    }
    if (!loaded) {
        const std::string detail = client.getLastError();
        _last_error = detail.empty() ? "잔고 조회에 실패했습니다." : detail;
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

    if (!ensureAccessToken()) return false;

    KISClient client;
    kis_domain::information_token token = accessTokenSnapshot();
    double exchange_rate = 0.0;
    bool loaded = client.request_exchange_rate(_settings.kis_app_key, _settings.kis_app_secret,
        _settings.kis_account_number, _settings.kis_account_product_code, token, exchange_rate);
    if (!loaded && client.isLastErrorTokenExpired()) {
        if (!refreshAccessTokenAfterExpiration(token)) return false;
        token = accessTokenSnapshot();
        loaded = client.request_exchange_rate(_settings.kis_app_key, _settings.kis_app_secret,
            _settings.kis_account_number, _settings.kis_account_product_code, token, exchange_rate);
    }
    if (loaded) {
        _exchange_rate = exchange_rate;
        _exchange_rate_updated_at = steady_clock::now();
        return true;
    }
    const std::string detail = client.getLastError();
    _last_error = detail.empty() ? "환율 조회에 실패했습니다." : detail;
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

    // Publish the connecting state before the worker starts. Otherwise the worker can
    // report "connected" first and this function can overwrite it with "connecting".
    PostMessage(_parent, WM_RECEIVE_STREAM_STATUS, static_cast<WPARAM>(stream_status::connecting), 0);

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


bool TradeManager::requestPortfolioHistory(std::vector<portfolio_holding> holdings,
    const std::string& requested_range)
{
    if (!_credentials_available || holdings.empty()) {
        return false;
    }
    if (!ensureAccessToken()) return false;

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
    const HWND parent = _parent;
    const std::string range = requested_range == "1w" || requested_range == "1m"
        || requested_range == "1y" ? requested_range : "4h";

    _history_worker = std::thread([this, parent, app_key, app_secret, range,
        holdings = std::move(holdings)]() {
        try {
            struct aggregate_bar {
                double open = 0;
                double high = 0;
                double low = 0;
                double close = 0;
                size_t count = 0;
            };

            const bool intraday = range == "4h";
            const int history_days = range == "1w" ? 7 : range == "1m" ? 31 : 366;
            const size_t daily_records = range == "1w" ? 10 : range == "1m" ? 40 : 380;
            const std::string end_date = today_key();
            const std::string start_date = intraday ? std::string{} : offset_date_key_tm(end_date, -history_days);

            boost::json::object result;
            boost::json::array errors;
            std::map<std::string, aggregate_bar> aggregate;
            size_t reflected = 0;

            KISClient history_client;
            kis_domain::information_token token = accessTokenSnapshot();
            std::string token_refresh_error;
            const auto request_minute_bars = [&](const std::string& exchange,
                const std::string& ticker, std::vector<kis_domain::minute_bar>& bars) {
                if (!token_refresh_error.empty()) return false;
                if (history_client.request_overseas_minute_bars(
                    app_key, app_secret, token, exchange, ticker, bars, 480)) return true;
                if (!history_client.isLastErrorTokenExpired()) return false;
                if (!refreshAccessTokenAfterExpiration(token, &token_refresh_error)) return false;
                token_refresh_error.clear();
                token = accessTokenSnapshot();
                bars.clear();
                return history_client.request_overseas_minute_bars(
                    app_key, app_secret, token, exchange, ticker, bars, 480);
            };
            const auto request_daily_bars = [&](const std::string& exchange,
                const std::string& ticker, std::vector<kis_domain::daily_bar>& bars) {
                if (!token_refresh_error.empty()) return false;
                if (history_client.request_overseas_daily_bars(app_key, app_secret, token,
                    exchange, ticker, start_date, end_date, bars, daily_records)) return true;
                if (!history_client.isLastErrorTokenExpired()) return false;
                if (!refreshAccessTokenAfterExpiration(token, &token_refresh_error)) return false;
                token_refresh_error.clear();
                token = accessTokenSnapshot();
                bars.clear();
                return history_client.request_overseas_daily_bars(app_key, app_secret, token,
                    exchange, ticker, start_date, end_date, bars, daily_records);
            };

            for (const auto& holding : holdings) {
                std::vector<std::string> exchanges{ holding.exchange };
                std::string exchange_upper = holding.exchange;
                std::transform(exchange_upper.begin(), exchange_upper.end(), exchange_upper.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (exchange_upper.empty() || exchange_upper == "US" || exchange_upper == "USA") {
                    exchanges = { "NASD", "NYSE", "AMEX" };
                }

                bool loaded = false;
                if (intraday) {
                    std::vector<kis_domain::minute_bar> bars;
                    for (const auto& exchange : exchanges) {
                        bars.clear();
                        if (request_minute_bars(exchange, holding.ticker, bars)) {
                            loaded = true;
                            break;
                        }
                    }
                    if (loaded) {
                        for (const auto& bar : bars) {
                            if (bar.kr_timestamp.empty()) continue;
                            auto& total = aggregate[bar.kr_timestamp];
                            total.open += bar.open * holding.quantity;
                            total.high += bar.high * holding.quantity;
                            total.low += bar.low * holding.quantity;
                            total.close += bar.close * holding.quantity;
                            ++total.count;
                        }
                    }
                }
                else {
                    std::vector<kis_domain::daily_bar> bars;
                    for (const auto& exchange : exchanges) {
                        bars.clear();
                        if (request_daily_bars(exchange, holding.ticker, bars)) {
                            loaded = true;
                            break;
                        }
                    }
                    if (loaded) {
                        for (const auto& bar : bars) {
                            if (bar.date.empty()) continue;
                            auto& total = aggregate[bar.date + "000000"];
                            total.open += bar.open * holding.quantity;
                            total.high += bar.high * holding.quantity;
                            total.low += bar.low * holding.quantity;
                            total.close += bar.close * holding.quantity;
                            ++total.count;
                        }
                    }
                }

                if (!loaded) {
                    boost::json::object error;
                    error["ticker"] = holding.ticker;
                    error["exchange"] = holding.exchange;
                    error["message"] = token_refresh_error.empty()
                        ? history_client.getLastError() : token_refresh_error;
                    errors.push_back(std::move(error));
                    continue;
                }
                ++reflected;
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }

            boost::json::array candles;
            const bool all_holdings_loaded = reflected == holdings.size();
            if (all_holdings_loaded) {
                for (const auto& [timestamp, bar] : aggregate) {
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

            const std::string unit = intraday ? "1분봉" : "일봉";
            const bool has_candles = !candles.empty();
            result["success"] = all_holdings_loaded && has_candles;
            result["candles"] = std::move(candles);
            result["range"] = range;
            result["interval"] = intraday ? "minute" : "day";
            result["reflected_count"] = static_cast<int64_t>(reflected);
            result["requested_count"] = static_cast<int64_t>(holdings.size());
            result["errors"] = std::move(errors);
            if (reflected == 0) result["message"] = "과거 " + unit + "을 불러오지 못했습니다.";
            else if (reflected < holdings.size()) {
                result["message"] = std::format(
                    "과거 {} 일부 종목 조회 실패 ({}/{}). 불완전한 포트폴리오 차트는 표시하지 않습니다.",
                    unit, reflected, holdings.size());
            }
            else if (!has_candles) result["message"] = "공통 거래일의 과거 " + unit + "을 찾지 못했습니다.";
            else result["message"] = std::format("과거 {} {}종목 반영", unit, reflected);

            auto payload = std::make_unique<std::string>(boost::json::serialize(result));
            if (IsWindow(parent) && PostMessage(parent, WM_RECEIVE_PORTFOLIO_HISTORY, 0,
                reinterpret_cast<LPARAM>(payload.get()))) payload.release();
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
    if (!_credentials_available || holdings.empty()
        || start_date.size() != 8 || end_date.size() != 8 || start_date > end_date) {
        return false;
    }
    if (!ensureAccessToken()) return false;

    holdings.erase(std::remove_if(holdings.begin(), holdings.end(), [](const portfolio_holding& item) {
        return item.ticker.empty() || item.quantity <= 0;
    }), holdings.end());
    if (holdings.empty()) return false;

    bool expected = false;
    if (!_performance_loading.compare_exchange_strong(expected, true)) return false;
    if (_performance_worker.joinable()) _performance_worker.join();

    const std::string app_key = _settings.kis_app_key;
    const std::string app_secret = _settings.kis_app_secret;
    const HWND parent = _parent;

    _performance_worker = std::thread([this, parent, app_key, app_secret,
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
            kis_domain::information_token token = accessTokenSnapshot();
            std::string token_refresh_error;
            const auto request_daily_bars = [&](const std::string& request_exchange,
                const std::string& ticker, std::vector<kis_domain::daily_bar>& bars) {
                if (!token_refresh_error.empty()) return false;
                if (history_client.request_overseas_daily_bars(
                    app_key, app_secret, token, request_exchange, ticker,
                    buffered_start.empty() ? start_date : buffered_start, end_date, bars, 2200)) {
                    return true;
                }
                if (!history_client.isLastErrorTokenExpired()) return false;
                if (!refreshAccessTokenAfterExpiration(token, &token_refresh_error)) {
                    return false;
                }
                token_refresh_error.clear();
                token = accessTokenSnapshot();
                bars.clear();
                return history_client.request_overseas_daily_bars(
                    app_key, app_secret, token, request_exchange, ticker,
                    buffered_start.empty() ? start_date : buffered_start, end_date, bars, 2200);
            };

            for (const auto& holding : holdings) {
                std::vector<kis_domain::daily_bar> bars;
                std::string resolved_exchange = holding.exchange;
                bool loaded = request_daily_bars(resolved_exchange, holding.ticker, bars);

                std::string exchange_upper = holding.exchange;
                std::transform(exchange_upper.begin(), exchange_upper.end(), exchange_upper.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (!loaded && token_refresh_error.empty()
                    && (exchange_upper.empty() || exchange_upper == "US" || exchange_upper == "USA")) {
                    for (const char* fallback_exchange : { "NASD", "NYSE", "AMEX" }) {
                        bars.clear();
                        if (request_daily_bars(fallback_exchange, holding.ticker, bars)) {
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
                    error["message"] = token_refresh_error.empty()
                        ? history_client.getLastError()
                        : token_refresh_error;
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
    if (!_credentials_available || ticker.empty()) return false;
    if (!ensureAccessToken()) return false;
    bool expected = false;
    if (!_quote_loading.compare_exchange_strong(expected, true)) return false;
    if (_quote_worker.joinable()) _quote_worker.join();

    const std::string app_key = _settings.kis_app_key;
    const std::string app_secret = _settings.kis_app_secret;
    const HWND parent = _parent;

    _quote_worker = std::thread([this, parent, app_key, app_secret, ticker, exchange]() {
        boost::json::object result;
        try {
            KISClient client;
            kis_domain::information_token token = accessTokenSnapshot();
            std::string token_refresh_error;
            kis_domain::overseas_quote quote;
            std::vector<kis_domain::minute_bar> bars;

            const auto request_quote = [&](const std::string& request_exchange) {
                if (!token_refresh_error.empty()) return false;
                if (client.request_overseas_quote(
                    app_key, app_secret, token, request_exchange, ticker, quote)) {
                    return true;
                }
                if (!client.isLastErrorTokenExpired()) return false;
                if (!refreshAccessTokenAfterExpiration(token, &token_refresh_error)) {
                    return false;
                }
                token_refresh_error.clear();
                token = accessTokenSnapshot();
                return client.request_overseas_quote(
                    app_key, app_secret, token, request_exchange, ticker, quote);
            };
            const auto request_bars = [&](const std::string& request_exchange) {
                bars.clear();
                if (!token_refresh_error.empty()) return false;
                if (client.request_overseas_minute_bars(
                    app_key, app_secret, token, request_exchange, ticker, bars, 240)) {
                    return true;
                }
                if (!client.isLastErrorTokenExpired()) return false;
                if (!refreshAccessTokenAfterExpiration(token, &token_refresh_error)) {
                    return false;
                }
                token_refresh_error.clear();
                token = accessTokenSnapshot();
                bars.clear();
                return client.request_overseas_minute_bars(
                    app_key, app_secret, token, request_exchange, ticker, bars, 240);
            };

            // KIS uses separate quote exchange codes for the US daytime session
            // (NASDAQ BAQ / NYSE BAY / AMEX BAA). The normal NAS/NYS/AMS codes
            // can remain at the previous close during this session.
            const std::string market_exchange = realtime_quote_exchange(exchange);
            bool quote_ok = request_quote(market_exchange);
            std::string quote_error = quote_ok
                ? std::string{}
                : (token_refresh_error.empty() ? client.getLastError() : token_refresh_error);
            bool quote_from_daytime = quote_ok && market_exchange != exchange;
            if (!quote_ok && token_refresh_error.empty() && market_exchange != exchange) {
                quote_ok = request_quote(exchange);
                if (quote_ok) {
                    quote_error.clear();
                    quote_from_daytime = false;
                }
                else if (quote_error.empty()) {
                    quote_error = client.getLastError();
                }
            }

            bool bars_ok = request_bars(market_exchange);
            std::string bars_error = bars_ok
                ? std::string{}
                : (token_refresh_error.empty() ? client.getLastError() : token_refresh_error);
            bool bars_from_daytime = bars_ok && market_exchange != exchange;
            if (!bars_ok && token_refresh_error.empty() && market_exchange != exchange) {
                bars_ok = request_bars(exchange);
                if (bars_ok) {
                    bars_error.clear();
                    bars_from_daytime = false;
                }
                else if (bars_error.empty()) {
                    bars_error = client.getLastError();
                }
            }

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
            else if (!bars_ok) result["message"] = quote_from_daytime
                ? "미국 주간거래 현재가는 조회했지만 최근 1분봉을 불러오지 못했습니다."
                : "현재가는 조회했지만 과거 1분봉을 불러오지 못했습니다.";
            else if (quote_from_daytime || bars_from_daytime)
                result["message"] = "미국 주간거래 현재가와 최근 1분봉을 불러왔습니다.";
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
    {
        std::scoped_lock token_lock(_token_mutex);
        _token = {};
    }
    if (_db_manager) {
        _db_manager->initialize();
        _db_manager->clearToken();
    }
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
        _last_error = "[토큰 발급 검증 단계, AppKey " + masked_app_key(updated.kis_app_key) + "] "
            + (detail.empty() ? "App Key와 App Secret을 확인하지 못했습니다." : detail);
        return false;
    }
    kis_domain::information_balance verified_balance;
    if (!_client->request_balance(updated.kis_app_key, updated.kis_app_secret,
        updated.kis_account_number, updated.kis_account_product_code, verified_token, verified_balance)) {
        const std::string detail = _client->getLastError();
        _last_error = "[계좌 검증 단계, AppKey " + masked_app_key(updated.kis_app_key) + "] "
            + (detail.empty() ? "계좌번호와 상품코드를 확인하지 못했습니다." : detail);
        return false;
    }
    const kis_domain::information_token previous_token = accessTokenSnapshot();
    if (!_db_manager || !_db_manager->initialize()) {
        _last_error = "로컬 토큰 저장소를 초기화하지 못했습니다.";
        return false;
    }
    if (!_db_manager->saveToken(verified_token)) {
        _last_error = "인증 토큰을 로컬 저장소에 저장하지 못했습니다.";
        return false;
    }
    if (!app_config::store_kis_credentials(updated)) {
        if (previous_token.ready) _db_manager->saveToken(previous_token);
        else _db_manager->clearToken();
        _last_error = "Windows 자격 증명 저장소에 인증정보를 저장하지 못했습니다.";
        return false;
    }

    // Do not mutate the live configuration until validation and both persistent
    // writes have succeeded. A failed save must leave the existing session usable.
    stopMarketStream();
    _settings = std::move(updated);
    _credentials_available = true;
    _last_error.clear();
    {
        std::scoped_lock token_lock(_token_mutex);
        _token = std::move(verified_token);
    }
    _balance = std::move(verified_balance);
    _balance_updated_at = steady_clock::now();
    _advice_updated_at = {};
    _advice_signature.clear();
    _advice_cache.clear();
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
