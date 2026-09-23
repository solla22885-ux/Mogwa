// KISClient.h: KISClient 헤더 파일.

#include "KISDomain.h"

#include <string>
#include <string_view>
#include <vector>

#pragma once

inline constexpr std::string_view BASE_URL = "https://openapi.koreainvestment.com:9443";
//inline constexpr std::string_view BASE_URL = "https://openapivts.koreainvestment.com:29443";
inline constexpr std::string_view BALANCE_PATH = "/uapi/domestic-stock/v1/trading/inquire-balance";
inline constexpr std::string_view OVERSEAS_BALANCE_PATH = "/uapi/overseas-stock/v1/trading/inquire-balance";
inline constexpr std::string_view OVERSEAS_PSAMOUNT_PATH = "/uapi/overseas-stock/v1/trading/inquire-psamount";
inline constexpr std::string_view OAUTH_PATH = "/oauth2/tokenP";
inline constexpr std::string_view WS_TOKEN_PATH = "/oauth2/Approval";
inline constexpr std::string_view ORDER_PATH = "/uapi/overseas-stock/v1/trading/order";
inline constexpr std::string_view OVERSEAS_MINUTE_PATH = "/uapi/overseas-price/v1/quotations/inquire-time-itemchartprice";
inline constexpr std::string_view OVERSEAS_DAILY_PATH = "/uapi/overseas-price/v1/quotations/dailyprice";
inline constexpr std::string_view OVERSEAS_PRICE_PATH = "/uapi/overseas-price/v1/quotations/price";

class KISClient {
public:
    KISClient();
    ~KISClient() = default;
private:
    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userp);
public:
    bool request_token(const std::string& appkey, const std::string& appsecret, kis_domain::information_token& output);
    bool request_ws_token(const std::string& appkey, const std::string& appsecret, std::string& ws_token);
    bool request_balance(const std::string& appkey, const std::string& appsecret,
        const std::string& account_number, const std::string& account_product_code,
        const kis_domain::information_token& info_token, kis_domain::information_balance& output);
    bool request_exchange_rate(const std::string& appkey, const std::string& appsecret,
        const std::string& account_number, const std::string& account_product_code,
        const kis_domain::information_token& info_token, double& output_rate);
    bool request_overseas_minute_bars(const std::string& appkey, const std::string& appsecret,
        const kis_domain::information_token& info_token, const std::string& exchange,
        const std::string& ticker, std::vector<kis_domain::minute_bar>& output, size_t max_records = 480);
    bool request_overseas_daily_bars(const std::string& appkey, const std::string& appsecret,
        const kis_domain::information_token& info_token, const std::string& exchange,
        const std::string& ticker, const std::string& start_date, const std::string& end_date,
        std::vector<kis_domain::daily_bar>& output, size_t max_records = 2000);
    bool request_overseas_quote(const std::string& appkey, const std::string& appsecret,
        const kis_domain::information_token& info_token, const std::string& exchange,
        const std::string& ticker, kis_domain::overseas_quote& output);

    const std::string& getLastError() const noexcept { return _last_error; }
private:
    std::string _last_error;
};
