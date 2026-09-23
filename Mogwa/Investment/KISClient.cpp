// KISClient.cpp: KISClient 구현 파일.

#include "pch.h"
#include "KISClient.h"

#include "lib/scope_exit.hpp"
#include <boost/json.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace {
    using steady_clock = std::chrono::steady_clock;

    constexpr auto quote_cache_ttl = std::chrono::seconds(5);
    constexpr auto minute_cache_ttl = std::chrono::seconds(60);
    constexpr auto current_daily_cache_ttl = std::chrono::minutes(5);
    constexpr auto historical_daily_cache_ttl = std::chrono::hours(24);
    constexpr auto minimum_request_interval = std::chrono::milliseconds(60);

    struct quote_cache_entry {
        kis_domain::overseas_quote value;
        steady_clock::time_point stored_at;
    };

    struct minute_cache_entry {
        std::vector<kis_domain::minute_bar> values;
        size_t max_records = 0;
        steady_clock::time_point stored_at;
    };

    struct daily_cache_entry {
        std::vector<kis_domain::daily_bar> values;
        struct coverage {
            std::string start;
            std::string end;
            steady_clock::time_point stored_at;
        };
        std::vector<coverage> coverages;
    };

    std::mutex market_cache_mutex;
    std::unordered_map<std::string, quote_cache_entry> quote_cache;
    std::unordered_map<std::string, minute_cache_entry> minute_cache;
    std::unordered_map<std::string, daily_cache_entry> daily_cache;
    std::unordered_map<std::string, std::string> resolved_us_exchanges;

    std::string current_date_key()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        std::ostringstream result;
        result << std::put_time(&local, "%Y%m%d");
        return result.str();
    }

    std::string market_cache_key(const std::string& exchange, std::string ticker)
    {
        std::transform(ticker.begin(), ticker.end(), ticker.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return exchange + ':' + ticker;
    }

    void configure_request(CURL* curl)
    {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }

    bool perform_request(CURL* curl, long* response_status = nullptr)
    {
        // KIS applies a shared per-app REST rate limit. All client instances use
        // this gate so background portfolio workers cannot burst requests.
        static std::mutex request_mutex;
        static steady_clock::time_point last_request;
        std::unique_lock request_lock(request_mutex);
        const auto elapsed = steady_clock::now() - last_request;
        if (last_request != steady_clock::time_point{} && elapsed < minimum_request_interval) {
            std::this_thread::sleep_for(minimum_request_interval - elapsed);
        }

        const CURLcode result = curl_easy_perform(curl);
        last_request = steady_clock::now();
        if (result != CURLE_OK) {
            TRACE(L"[KISClient] curl error: %hs\n", curl_easy_strerror(result));
            if (response_status) *response_status = 0;
            return false;
        }

        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        if (response_status) *response_status = status_code;
        return status_code >= 200 && status_code < 300;
    }

    std::string response_error_message(const std::string& response, const std::string& fallback)
    {
        boost::system::error_code error;
        const boost::json::value value = boost::json::parse(response, error);
        if (error || !value.is_object()) return fallback;

        const auto& object = value.as_object();
        for (const char* key : { "error_description", "msg1", "message", "error" }) {
            const auto* field = object.if_contains(key);
            if (field && field->is_string() && !field->as_string().empty()) {
                return std::string(field->as_string());
            }
        }
        return fallback;
    }

    std::string response_error_code(const std::string& response)
    {
        boost::system::error_code error;
        const boost::json::value value = boost::json::parse(response, error);
        if (error || !value.is_object()) return {};

        const auto& object = value.as_object();
        for (const char* key : { "msg_cd", "error_code", "code" }) {
            const auto* field = object.if_contains(key);
            if (field && field->is_string() && !field->as_string().empty()) {
                return std::string(field->as_string());
            }
        }
        return {};
    }

    bool api_response_succeeded(const boost::json::object& object)
    {
        const auto* result = object.if_contains("rt_cd");
        return !result || (result->is_string() && result->as_string() == "0");
    }

    std::string normalize_quote_exchange(std::string exchange)
    {
        std::transform(exchange.begin(), exchange.end(), exchange.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (exchange == "NASD" || exchange == "NASDAQ" || exchange == "NAS") return "NAS";
        if (exchange == "NYSE" || exchange == "NYS") return "NYS";
        if (exchange == "AMEX" || exchange == "AMS") return "AMS";
        if (exchange == "SEHK" || exchange == "HKS") return "HKS";
        if (exchange == "SHAA" || exchange == "SHS") return "SHS";
        if (exchange == "SZAA" || exchange == "SZS") return "SZS";
        if (exchange == "TKSE" || exchange == "TSE") return "TSE";
        return exchange;
    }

    std::string resolve_quote_exchange(const std::string& exchange, const std::string& ticker)
    {
        const std::string normalized = normalize_quote_exchange(exchange);
        if (!normalized.empty() && normalized != "US" && normalized != "USA") return normalized;

        const std::string ticker_key = market_cache_key({}, ticker);
        std::scoped_lock cache_lock(market_cache_mutex);
        const auto cached = resolved_us_exchanges.find(ticker_key);
        return cached == resolved_us_exchanges.end() ? normalized : cached->second;
    }

    void remember_us_exchange(const std::string& ticker, const std::string& exchange)
    {
        if (exchange != "NAS" && exchange != "NYS" && exchange != "AMS") return;
        std::scoped_lock cache_lock(market_cache_mutex);
        resolved_us_exchanges[market_cache_key({}, ticker)] = exchange;
    }

    std::string json_string(const boost::json::object& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys) {
            const auto* value = object.if_contains(key);
            if (!value) continue;
            if (value->is_string()) return std::string(value->as_string());
            if (value->is_double()) return std::to_string(value->as_double());
            if (value->is_int64()) return std::to_string(value->as_int64());
            if (value->is_uint64()) return std::to_string(value->as_uint64());
        }
        return {};
    }

    double json_double(const boost::json::object& object, std::initializer_list<const char*> keys)
    {
        const std::string text = json_string(object, keys);
        if (text.empty()) return 0.0;
        try { return std::stod(text); }
        catch (...) { return 0.0; }
    }

    std::string previous_minute_key(const std::string& timestamp)
    {
        if (timestamp.size() != 14) return {};
        std::tm tm{};
        std::istringstream input(timestamp);
        input >> std::get_time(&tm, "%Y%m%d%H%M%S");
        if (input.fail()) return {};
        tm.tm_isdst = -1;
        const std::time_t value = std::mktime(&tm);
        if (value == static_cast<std::time_t>(-1)) return {};
        const std::time_t previous = value - 60;
        std::tm out{};
        localtime_s(&out, &previous);
        std::ostringstream result;
        result << std::put_time(&out, "%Y%m%d%H%M%S");
        return result.str();
    }


    std::string offset_date_key(const std::string& date, int days)
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
}

KISClient::KISClient() = default;

////////////////////////////////////////////////////////////////////////////////

size_t KISClient::write_cb(char* ptr, size_t size, size_t nmemb, void* userp)
{
    auto* out = static_cast<std::string*>(userp);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

////////////////////////////////////////////////////////////////////////////////

bool KISClient::request_token(const std::string& appkey, const std::string& appsecret, kis_domain::information_token& output)
{
    output = {};
    _last_error.clear();
    _last_error_code.clear();
    std::string body = "{"
        "\"grant_type\":\"client_credentials\","
        "\"appkey\":\"" + appkey + "\","
        "\"appsecret\":\"" + appsecret + "\""
        "}";

    CURL* curl = curl_easy_init();
    std::string response;

    if (curl == nullptr) {
        _last_error = "HTTP 요청을 초기화하지 못했습니다.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    m1::util::scope_exit exit([&curl, &headers]() {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    });

    headers = curl_slist_append(headers, "Content-Type: application/json");

    const std::string url = std::string(BASE_URL) + std::string(OAUTH_PATH);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    configure_request(curl);

    long status_code = 0;
    if (!perform_request(curl, &status_code)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response,
            status_code > 0 ? std::format("한국투자증권 인증 요청이 HTTP {}로 실패했습니다.", status_code)
                            : "한국투자증권 인증 서버에 연결하지 못했습니다.");
        return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value parsed = boost::json::parse(response, parse_error);
    if (parse_error || !parsed.is_object()) {
        _last_error = "인증 서버가 올바른 JSON을 반환하지 않았습니다.";
        return false;
    }

    const auto& obj = parsed.as_object();
    const auto* access_token = obj.if_contains("access_token");
    const auto* expired = obj.if_contains("access_token_token_expired");
    const auto* token_type = obj.if_contains("token_type");
    const auto* expires_in = obj.if_contains("expires_in");
    if (!access_token || !access_token->is_string()
        || !expired || !expired->is_string()
        || !token_type || !token_type->is_string()
        || !expires_in || (!expires_in->is_int64() && !expires_in->is_uint64())) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response, "인증 응답에 접근 토큰이 없습니다.");
        return false;
    }

    output.access_token = access_token->as_string().c_str();
    output.access_token_expired = expired->as_string().c_str();
    output.token_type = token_type->as_string().c_str();
    output.expires_in = expires_in->is_int64()
        ? expires_in->as_int64()
        : static_cast<int64_t>(expires_in->as_uint64());
    output.app_key_tag = kis_domain::make_app_key_tag(appkey);
    output.ready = true;
    return true;
}

bool KISClient::request_ws_token(const std::string& appkey, const std::string& appsecret, std::string& ws_key)
{
    ws_key.clear();
    _last_error.clear();
    _last_error_code.clear();
    std::string body = "{"
        "\"grant_type\":\"client_credentials\","
        "\"appkey\":\"" + appkey + "\","
        "\"secretkey\":\"" + appsecret + "\""
        "}";

    CURL* curl = curl_easy_init();
    std::string response;

    if (curl == nullptr) {
        _last_error = "HTTP 요청을 초기화하지 못했습니다.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    m1::util::scope_exit exit([&curl, &headers]() {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    });

    headers = curl_slist_append(headers, "Content-Type: application/json");

    const std::string url = std::string(BASE_URL) + std::string(WS_TOKEN_PATH);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    configure_request(curl);

    long status_code = 0;
    if (!perform_request(curl, &status_code)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response,
            status_code > 0 ? std::format("WebSocket 승인키 요청이 HTTP {}로 실패했습니다.", status_code)
                            : "WebSocket 승인키 서버에 연결하지 못했습니다.");
        return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value parsed = boost::json::parse(response, parse_error);
    if (parse_error || !parsed.is_object()) {
        _last_error = "WebSocket 승인키 서버가 올바른 JSON을 반환하지 않았습니다.";
        return false;
    }

    const auto& obj = parsed.as_object();
    const auto* approval_key = obj.if_contains("approval_key");
    if (!approval_key || !approval_key->is_string() || approval_key->as_string().empty()) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response, "WebSocket 승인 응답에 approval_key가 없습니다.");
        return false;
    }

    ws_key = approval_key->as_string().c_str();
    return true;
}

bool KISClient::request_balance(const std::string& appkey, const std::string& appsecret,
    const std::string& account_number, const std::string& account_product_code,
    const kis_domain::information_token& info_token, kis_domain::information_balance& output)
{
    _last_error.clear();
    _last_error_code.clear();
    output = {};
    std::string query = "?CANO=" + account_number;
    query += "&ACNT_PRDT_CD=" + account_product_code;
    query += "&OVRS_EXCG_CD=NASD";
    query += "&TR_CRCY_CD=USD";
    query += "&CTX_AREA_FK200=";
    query += "&CTX_AREA_NK200=";

    std::string url = std::format("{}{}{}", BASE_URL, OVERSEAS_BALANCE_PATH, query);

    CURL* curl = curl_easy_init();
    std::string response;

    if (curl == nullptr) {
        _last_error = "잔고 조회 요청을 초기화하지 못했습니다.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    m1::util::scope_exit exit([&curl, &headers]() {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    });

    headers = curl_slist_append(headers, ("authorization: Bearer " + info_token.access_token).c_str());
    headers = curl_slist_append(headers, ("appkey: " + appkey).c_str());
    headers = curl_slist_append(headers, ("appsecret: " + appsecret).c_str());
    headers = curl_slist_append(headers, "tr_id: JTTT3012R");
    headers = curl_slist_append(headers, "custtype: P");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    configure_request(curl);

    long status_code = 0;
    if (!perform_request(curl, &status_code)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response,
            status_code > 0 ? std::format("잔고 조회가 HTTP {}로 실패했습니다.", status_code)
                            : "잔고 조회 서버에 연결하지 못했습니다.");
        return false;
    }

    try {
        boost::json::value token_json = boost::json::parse(response);
        boost::json::object obj = token_json.as_object();
        if (!api_response_succeeded(obj)) {
            _last_error_code = response_error_code(response);
            _last_error = response_error_message(response, "잔고 조회에 실패했습니다.");
            return false;
        }

    if (auto it = obj.find("output1"); it != obj.end() && it->value().is_array()) {
        for (const auto& row : it->value().as_array()) {
            if (!row.is_object()) continue;
            kis_domain::balance_item1 item1;
            boost::json::object o = row.as_object();
            item1.cano = o["cano"].as_string().c_str();
            item1.acnt_prdt_cd = o["acnt_prdt_cd"].as_string().c_str();
            item1.prdt_type_cd = o["prdt_type_cd"].as_string().c_str();
            item1.ovrs_pdno = o["ovrs_pdno"].as_string().c_str();
            item1.ovrs_item_name = o["ovrs_item_name"].as_string().c_str();
            item1.frcr_evlu_pfls_amt = std::stod(o["frcr_evlu_pfls_amt"].as_string().c_str());
            item1.evlu_pfls_rt = std::stod(o["evlu_pfls_rt"].as_string().c_str());
            item1.pchs_avg_pric = std::stod(o["pchs_avg_pric"].as_string().c_str());
            item1.ovrs_cblc_qty = std::stod(o["ovrs_cblc_qty"].as_string().c_str());
            item1.ord_psbl_qty = std::stod(o["ord_psbl_qty"].as_string().c_str());
            item1.frcr_pchs_amt1 = std::stod(o["frcr_pchs_amt1"].as_string().c_str());
            item1.ovrs_stck_evlu_amt = std::stod(o["ovrs_stck_evlu_amt"].as_string().c_str());
            item1.now_pric2 = std::stod(o["now_pric2"].as_string().c_str());
            item1.tr_crcy_cd = o["tr_crcy_cd"].as_string().c_str();
            item1.ovrs_excg_cd = o["ovrs_excg_cd"].as_string().c_str();
            item1.loan_type_cd = o["loan_type_cd"].as_string().c_str();
            item1.loan_dt = o["loan_dt"].as_string().c_str();
            item1.expd_dt = o["expd_dt"].as_string().c_str();
            output.stockList.push_back(std::move(item1));
        }
    }

    if (auto it = obj.find("output2"); it != obj.end() && it->value().is_object()) {
        boost::json::object o = it->value().as_object();
        kis_domain::balance_item2 item2;
        item2.frcr_pchs_amt1 = std::stod(o["frcr_pchs_amt1"].as_string().c_str());
        item2.ovrs_rlzt_pfls_amt = std::stod(o["ovrs_rlzt_pfls_amt"].as_string().c_str());
        item2.ovrs_tot_pfls = std::stod(o["ovrs_tot_pfls"].as_string().c_str());
        item2.rlzt_erng_rt = std::stod(o["rlzt_erng_rt"].as_string().c_str());
        item2.tot_evlu_pfls_amt = std::stod(o["tot_evlu_pfls_amt"].as_string().c_str());
        item2.tot_pftrt = std::stod(o["tot_pftrt"].as_string().c_str());
        item2.frcr_buy_amt_smtl1 = std::stod(o["frcr_buy_amt_smtl1"].as_string().c_str());
        item2.ovrs_rlzt_pfls_amt2 = std::stod(o["ovrs_rlzt_pfls_amt2"].as_string().c_str());
        item2.frcr_buy_amt_smtl2 = std::stod(o["frcr_buy_amt_smtl2"].as_string().c_str());
        output.summary = std::move(item2);
    }

        return true;
    }
    catch (const std::exception& error) {
        output = {};
        _last_error = std::format("잔고 응답 처리에 실패했습니다: {}", error.what());
        TRACE(L"[KISClient] balance parse error: %hs\n", error.what());
        return false;
    }
}

bool KISClient::request_exchange_rate(const std::string& appkey, const std::string& appsecret,
    const std::string& account_number, const std::string& account_product_code,
    const kis_domain::information_token& info_token, double& output_rate)
{
    _last_error.clear();
    _last_error_code.clear();
    std::string query = "?CANO=" + account_number;
    query += "&ACNT_PRDT_CD=" + account_product_code;
    query += "&OVRS_EXCG_CD=NASD";
    query += "&OVRS_ORD_UNPR=1.4";
    query += "&ITEM_CD=QQQ";

    std::string url = std::format("{}{}{}", BASE_URL, OVERSEAS_PSAMOUNT_PATH, query);

    CURL* curl = curl_easy_init();
    std::string response;

    if (curl == nullptr) {
        _last_error = "환율 조회 요청을 초기화하지 못했습니다.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    m1::util::scope_exit exit([&curl, &headers]() {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        });

    headers = curl_slist_append(headers, ("authorization: Bearer " + info_token.access_token).c_str());
    headers = curl_slist_append(headers, ("appkey: " + appkey).c_str());
    headers = curl_slist_append(headers, ("appsecret: " + appsecret).c_str());
    headers = curl_slist_append(headers, "tr_id: TTTS3007R");
    headers = curl_slist_append(headers, "custtype: P");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    configure_request(curl);

    long status_code = 0;
    if (!perform_request(curl, &status_code)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response,
            status_code > 0 ? std::format("환율 조회가 HTTP {}로 실패했습니다.", status_code)
                            : "환율 조회 서버에 연결하지 못했습니다.");
        return false;
    }

    try {
        boost::json::value json_value = boost::json::parse(response);
        boost::json::object obj = json_value.as_object();

        if (auto it = obj.find("rt_cd"); it != obj.end() && it->value().is_string()) {
            std::string rt_cd = it->value().as_string().c_str();
            if (rt_cd != "0") {
                std::string msg = "";

                if (auto msg_it = obj.find("msg1"); msg_it != obj.end() && msg_it->value().is_string()) {
                    msg = msg_it->value().as_string().c_str();
                }

                _last_error_code = response_error_code(response);
                _last_error = msg.empty() ? "환율 조회에 실패했습니다." : msg;
                TRACE(L"[KISClient] request_exchange_rate api error: %hs\n", msg.c_str());
                return false;
            }
        }

        if (auto it = obj.find("output"); it != obj.end() && it->value().is_object()) {
            boost::json::object output = it->value().as_object();

            if (auto exrt_it = output.find("exrt"); exrt_it != output.end()) {
                if (exrt_it->value().is_string()) {
                    output_rate = std::stod(exrt_it->value().as_string().c_str());
                    return true;
                }

                if (exrt_it->value().is_double()) {
                    output_rate = exrt_it->value().as_double();
                    return true;
                }

                if (exrt_it->value().is_int64()) {
                    output_rate = static_cast<double>(exrt_it->value().as_int64());
                    return true;
                }
            }
        }

        _last_error = "환율 조회 응답에 환율(exrt)이 없습니다.";
        TRACE(L"[KISClient] request_exchange_rate exrt not found. response: %hs\n", response.c_str());
        return false;
    }
    catch (const std::exception& e) {
        _last_error = std::format("환율 응답 처리에 실패했습니다: {}", e.what());
        TRACE(L"[KISClient] request_exchange_rate parse exception: %hs\n", e.what());
        TRACE(L"[KISClient] response: %hs\n", response.c_str());
        return false;
    }
}



bool KISClient::request_overseas_quote(const std::string& appkey, const std::string& appsecret,
    const kis_domain::information_token& info_token, const std::string& exchange,
    const std::string& ticker, kis_domain::overseas_quote& output)
{
    output = {};
    _last_error.clear();
    _last_error_code.clear();
    if (ticker.empty()) return false;

    const std::string excd = resolve_quote_exchange(exchange, ticker);
    const std::string cache_key = market_cache_key(excd, ticker);
    {
        std::scoped_lock cache_lock(market_cache_mutex);
        const auto cached = quote_cache.find(cache_key);
        if (cached != quote_cache.end()
            && steady_clock::now() - cached->second.stored_at <= quote_cache_ttl) {
            output = cached->second.value;
            return true;
        }
    }
    const std::string query = "?AUTH=&EXCD=" + excd + "&SYMB=" + ticker;
    const std::string url = std::format("{}{}{}", BASE_URL, OVERSEAS_PRICE_PATH, query);

    CURL* curl = curl_easy_init();
    if (!curl) {
        _last_error = "해외주식 현재가 요청을 초기화하지 못했습니다.";
        return false;
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    m1::util::scope_exit exit([&curl, &headers]() {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    });

    headers = curl_slist_append(headers, ("authorization: Bearer " + info_token.access_token).c_str());
    headers = curl_slist_append(headers, ("appkey: " + appkey).c_str());
    headers = curl_slist_append(headers, ("appsecret: " + appsecret).c_str());
    headers = curl_slist_append(headers, "tr_id: HHDFS00000300");
    headers = curl_slist_append(headers, "custtype: P");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    configure_request(curl);

    long status_code = 0;
    if (!perform_request(curl, &status_code)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response,
            status_code > 0 ? std::format("{} 현재가 조회가 HTTP {}로 실패했습니다.", ticker, status_code)
                            : std::format("{} 현재가 조회 서버에 연결하지 못했습니다.", ticker));
        return false;
    }

    boost::system::error_code parse_error;
    const boost::json::value parsed = boost::json::parse(response, parse_error);
    if (parse_error || !parsed.is_object()) {
        _last_error = std::format("{} 현재가 응답이 올바른 JSON이 아닙니다.", ticker);
        return false;
    }
    const auto& root = parsed.as_object();
    if (!api_response_succeeded(root)) {
        _last_error_code = response_error_code(response);
        _last_error = response_error_message(response, std::format("{} 현재가 조회에 실패했습니다.", ticker));
        return false;
    }

    const auto* out = root.if_contains("output");
    if (!out || !out->is_object()) {
        _last_error = std::format("{} 현재가 응답에 output이 없습니다.", ticker);
        return false;
    }
    const auto& row = out->as_object();
    output.ticker = ticker;
    output.exchange = excd;
    output.previous_close = json_double(row, { "base" });
    output.price = json_double(row, { "last" });
    output.change = json_double(row, { "diff" });
    output.change_rate = json_double(row, { "rate" });
    output.volume = json_double(row, { "tvol" });
    output.amount = json_double(row, { "tamt" });
    output.change_sign = json_string(row, { "sign" });
    output.orderable = json_string(row, { "ordy" });
    if (output.price <= 0) return false;
    remember_us_exchange(ticker, excd);
    {
        std::scoped_lock cache_lock(market_cache_mutex);
        quote_cache[cache_key] = { output, steady_clock::now() };
    }
    return true;
}

bool KISClient::request_overseas_daily_bars(const std::string& appkey, const std::string& appsecret,
    const kis_domain::information_token& info_token, const std::string& exchange,
    const std::string& ticker, const std::string& start_date, const std::string& end_date,
    std::vector<kis_domain::daily_bar>& output, size_t max_records)
{
    output.clear();
    _last_error.clear();
    _last_error_code.clear();
    if (ticker.empty() || start_date.size() != 8 || end_date.size() != 8 || max_records == 0) return false;

    const std::string excd = resolve_quote_exchange(exchange, ticker);
    const std::string cache_key = market_cache_key(excd, ticker);
    {
        std::scoped_lock cache_lock(market_cache_mutex);
        const auto cached = daily_cache.find(cache_key);
        if (cached != daily_cache.end()) {
            const auto ttl = end_date < current_date_key()
                ? historical_daily_cache_ttl
                : current_daily_cache_ttl;
            const auto now = steady_clock::now();
            const bool covered = std::any_of(cached->second.coverages.begin(),
                cached->second.coverages.end(), [&](const daily_cache_entry::coverage& range) {
                    return now - range.stored_at <= ttl
                        && range.start <= start_date
                        && range.end >= end_date;
                });
            if (covered) {
                for (const auto& bar : cached->second.values) {
                    if (bar.date >= start_date && bar.date <= end_date) output.push_back(bar);
                }
                if (output.size() > max_records) {
                    output.erase(output.begin(), output.end() - static_cast<ptrdiff_t>(max_records));
                }
                return !output.empty();
            }
        }
    }
    std::string current_bymd = end_date;
    std::unordered_set<std::string> seen;
    bool reached_requested_start = false;

    for (size_t page = 0; page < 40 && output.size() < max_records; ++page) {
        std::string query = "?AUTH=";
        query += "&EXCD=" + excd;
        query += "&SYMB=" + ticker;
        query += "&GUBN=0";
        query += "&BYMD=" + current_bymd;
        query += "&MODP=1";

        const std::string url = std::format("{}{}{}", BASE_URL, OVERSEAS_DAILY_PATH, query);
        CURL* curl = curl_easy_init();
        if (!curl) {
            _last_error = "해외주식 일봉 요청을 초기화하지 못했습니다.";
            return false;
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        m1::util::scope_exit exit([&curl, &headers]() {
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        });

        headers = curl_slist_append(headers, ("authorization: Bearer " + info_token.access_token).c_str());
        headers = curl_slist_append(headers, ("appkey: " + appkey).c_str());
        headers = curl_slist_append(headers, ("appsecret: " + appsecret).c_str());
        headers = curl_slist_append(headers, "tr_id: HHDFS76240000");
        headers = curl_slist_append(headers, "custtype: P");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        configure_request(curl);

        long status_code = 0;
        if (!perform_request(curl, &status_code)) {
            _last_error_code = response_error_code(response);
            _last_error = response_error_message(response,
                status_code > 0 ? std::format("{} 일봉 조회가 HTTP {}로 실패했습니다.", ticker, status_code)
                                : std::format("{} 일봉 조회 서버에 연결하지 못했습니다.", ticker));
            return false;
        }

        boost::system::error_code parse_error;
        const boost::json::value parsed = boost::json::parse(response, parse_error);
        if (parse_error || !parsed.is_object()) {
            _last_error = std::format("{} 일봉 응답이 올바른 JSON이 아닙니다.", ticker);
            return false;
        }
        const auto& root = parsed.as_object();
        if (!api_response_succeeded(root)) {
            _last_error_code = response_error_code(response);
            _last_error = response_error_message(response, std::format("{} 일봉 조회에 실패했습니다.", ticker));
            return false;
        }

        const auto* rows = root.if_contains("output2");
        if (!rows || !rows->is_array() || rows->as_array().empty()) {
            reached_requested_start = true;
            break;
        }

        std::string oldest;
        size_t rows_seen = 0;
        for (const auto& value : rows->as_array()) {
            if (!value.is_object()) continue;
            const auto& row = value.as_object();
            const std::string date = json_string(row, { "xymd", "stck_bsop_date" });
            if (date.size() != 8) continue;
            ++rows_seen;
            if (oldest.empty() || date < oldest) oldest = date;
            if (date < start_date || date > end_date || !seen.insert(date).second) continue;

            kis_domain::daily_bar bar;
            bar.date = date;
            bar.open = json_double(row, { "open", "stck_oprc" });
            bar.high = json_double(row, { "high", "stck_hgpr" });
            bar.low = json_double(row, { "low", "stck_lwpr" });
            bar.close = json_double(row, { "clos", "close", "stck_clpr" });
            bar.volume = json_double(row, { "tvol", "acml_vol" });
            if (bar.close <= 0) continue;
            if (bar.open <= 0) bar.open = bar.close;
            if (bar.high <= 0) bar.high = (std::max)(bar.open, bar.close);
            if (bar.low <= 0) bar.low = (std::min)(bar.open, bar.close);
            output.push_back(std::move(bar));
            if (output.size() >= max_records) break;
        }

        if (oldest <= start_date) reached_requested_start = true;
        if (rows_seen == 0 || oldest.empty() || reached_requested_start || output.size() >= max_records) break;
        const std::string next = offset_date_key(oldest, -1);
        if (next.empty() || next >= current_bymd) break;
        current_bymd = next;
        std::this_thread::sleep_for(std::chrono::milliseconds(130));
    }

    std::sort(output.begin(), output.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.date < rhs.date;
    });
    if (output.empty()) return false;
    remember_us_exchange(ticker, excd);

    {
        std::scoped_lock cache_lock(market_cache_mutex);
        auto& cached = daily_cache[cache_key];
        std::map<std::string, kis_domain::daily_bar> merged;
        for (const auto& bar : cached.values) merged[bar.date] = bar;
        for (const auto& bar : output) merged[bar.date] = bar;
        cached.values.clear();
        cached.values.reserve(merged.size());
        for (auto& [date, bar] : merged) cached.values.push_back(std::move(bar));

        const std::string actual_start = reached_requested_start ? start_date : output.front().date;
        cached.coverages.push_back({ actual_start, end_date, steady_clock::now() });
        // Month navigation can create many overlapping entries. Keep the cache bounded
        // without merging disjoint ranges (which would incorrectly cache the gap).
        if (cached.coverages.size() > 64) {
            cached.coverages.erase(cached.coverages.begin(),
                cached.coverages.begin() + static_cast<ptrdiff_t>(cached.coverages.size() - 64));
        }
    }
    return true;
}

bool KISClient::request_overseas_minute_bars(const std::string& appkey, const std::string& appsecret,
    const kis_domain::information_token& info_token, const std::string& exchange,
    const std::string& ticker, std::vector<kis_domain::minute_bar>& output, size_t max_records)
{
    output.clear();
    _last_error.clear();
    _last_error_code.clear();
    if (ticker.empty() || max_records == 0) return false;

    const std::string excd = resolve_quote_exchange(exchange, ticker);
    const std::string cache_key = market_cache_key(excd, ticker);
    {
        std::scoped_lock cache_lock(market_cache_mutex);
        const auto cached = minute_cache.find(cache_key);
        if (cached != minute_cache.end()
            && cached->second.max_records >= max_records
            && steady_clock::now() - cached->second.stored_at <= minute_cache_ttl) {
            const size_t first = cached->second.values.size() > max_records
                ? cached->second.values.size() - max_records
                : 0;
            output.assign(cached->second.values.begin() + static_cast<ptrdiff_t>(first),
                cached->second.values.end());
            return !output.empty();
        }
    }
    std::string keyb;
    bool next_page = false;
    std::unordered_set<std::string> seen;

    while (output.size() < max_records) {
        std::string query = "?AUTH=";
        query += "&EXCD=" + excd;
        query += "&SYMB=" + ticker;
        query += "&NMIN=1";
        query += "&PINC=1";
        query += "&NEXT=" + std::string(next_page ? "1" : "");
        query += "&NREC=120";
        query += "&FILL=";
        query += "&KEYB=" + keyb;

        const std::string url = std::format("{}{}{}", BASE_URL, OVERSEAS_MINUTE_PATH, query);
        CURL* curl = curl_easy_init();
        if (!curl) {
            _last_error = "해외주식 분봉 요청을 초기화하지 못했습니다.";
            return false;
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        m1::util::scope_exit exit([&curl, &headers]() {
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        });

        headers = curl_slist_append(headers, ("authorization: Bearer " + info_token.access_token).c_str());
        headers = curl_slist_append(headers, ("appkey: " + appkey).c_str());
        headers = curl_slist_append(headers, ("appsecret: " + appsecret).c_str());
        headers = curl_slist_append(headers, "tr_id: HHDFS76950200");
        headers = curl_slist_append(headers, "custtype: P");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        configure_request(curl);

        long status_code = 0;
        if (!perform_request(curl, &status_code)) {
            _last_error_code = response_error_code(response);
            _last_error = response_error_message(response,
                status_code > 0 ? std::format("{} 분봉 조회가 HTTP {}로 실패했습니다.", ticker, status_code)
                                : std::format("{} 분봉 조회 서버에 연결하지 못했습니다.", ticker));
            return false;
        }

        boost::system::error_code parse_error;
        const boost::json::value parsed = boost::json::parse(response, parse_error);
        if (parse_error || !parsed.is_object()) {
            _last_error = std::format("{} 분봉 응답이 올바른 JSON이 아닙니다.", ticker);
            return false;
        }

        const auto& root = parsed.as_object();
        if (!api_response_succeeded(root)) {
            _last_error_code = response_error_code(response);
            _last_error = response_error_message(response, std::format("{} 분봉 조회에 실패했습니다.", ticker));
            return false;
        }

        size_t appended = 0;
        std::string oldest_local;
        if (const auto* value = root.if_contains("output2"); value && value->is_array()) {
            for (const auto& row_value : value->as_array()) {
                if (!row_value.is_object()) continue;
                const auto& row = row_value.as_object();

                const std::string xymd = json_string(row, { "xymd", "tymd", "stck_bsop_date" });
                const std::string xhms = json_string(row, { "xhms", "stck_cntg_hour" });
                const std::string kymd = json_string(row, { "kymd" });
                const std::string khms = json_string(row, { "khms" });
                if (xymd.empty() || xhms.empty()) continue;

                kis_domain::minute_bar bar;
                bar.local_timestamp = xymd + xhms;
                bar.kr_timestamp = (!kymd.empty() && !khms.empty()) ? kymd + khms : bar.local_timestamp;
                bar.open = json_double(row, { "open", "stck_oprc" });
                bar.high = json_double(row, { "high", "stck_hgpr" });
                bar.low = json_double(row, { "low", "stck_lwpr" });
                bar.close = json_double(row, { "last", "clos", "close", "stck_prpr" });

                if (bar.close <= 0) continue;
                if (bar.open <= 0) bar.open = bar.close;
                if (bar.high <= 0) bar.high = (std::max)(bar.open, bar.close);
                if (bar.low <= 0) bar.low = (std::min)(bar.open, bar.close);
                if (!seen.insert(bar.kr_timestamp).second) continue;

                if (oldest_local.empty() || bar.local_timestamp < oldest_local) oldest_local = bar.local_timestamp;
                output.push_back(std::move(bar));
                ++appended;
                if (output.size() >= max_records) break;
            }
        }

        bool has_next = false;
        if (const auto* value = root.if_contains("output1")) {
            if (value->is_object()) {
                has_next = json_string(value->as_object(), { "next", "NEXT" }) == "1";
            } else if (value->is_array() && !value->as_array().empty() && value->as_array().front().is_object()) {
                has_next = json_string(value->as_array().front().as_object(), { "next", "NEXT" }) == "1";
            }
        }

        if (!has_next || appended == 0 || oldest_local.empty() || output.size() >= max_records) break;
        keyb = previous_minute_key(oldest_local);
        if (keyb.empty()) break;
        next_page = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    std::sort(output.begin(), output.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.kr_timestamp < rhs.kr_timestamp;
    });
    if (output.empty()) return false;
    remember_us_exchange(ticker, excd);
    {
        std::scoped_lock cache_lock(market_cache_mutex);
        auto& cached = minute_cache[cache_key];
        const bool expired = cached.stored_at == steady_clock::time_point{}
            || steady_clock::now() - cached.stored_at > minute_cache_ttl;
        if (expired || max_records >= cached.max_records || cached.values.empty()) {
            cached.values = output;
            cached.max_records = max_records;
            cached.stored_at = steady_clock::now();
        }
    }
    return true;
}
