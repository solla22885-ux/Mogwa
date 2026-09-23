#include "pch.h"
#include "KISStreamClient.h"

#include "KISClient.h"
#include "lib/scope_exit.hpp"
#include "lib/string.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {
    constexpr std::string_view stream_host = "ops.koreainvestment.com";
    constexpr std::string_view stream_port = "21000";

    std::string upper_copy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return value;
    }

    bool is_us_daytime_session_kst()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_s(&utc, &now);
        const int korea_hour = (utc.tm_hour + 9) % 24;
        const int minutes = korea_hour * 60 + utc.tm_min;
        // KIS 미국주간거래 운영시간: 한국시간(KST) 10:00~18:00.
        return minutes >= 10 * 60 && minutes < 18 * 60;
    }

    struct subscription_keys {
        std::string primary;
        std::string secondary;
    };

    subscription_keys make_subscription_keys(const kis_domain::balance_item1& item)
    {
        const std::string exchange = upper_copy(item.ovrs_excg_cd);
        const bool daytime = is_us_daytime_session_kst();

        std::string regular_market;
        std::string daytime_market;
        if (exchange == "NASD" || exchange == "NASDAQ" || exchange == "NAS" || exchange == "BAQ") {
            regular_market = "NAS";
            daytime_market = "BAQ";
        }
        else if (exchange == "NYSE" || exchange == "NYS" || exchange == "BAY") {
            regular_market = "NYS";
            daytime_market = "BAY";
        }
        else if (exchange == "AMEX" || exchange == "AMS" || exchange == "BAA") {
            regular_market = "AMS";
            daytime_market = "BAA";
        }

        if (!regular_market.empty()) {
            const std::string regular_key = "D" + regular_market + item.ovrs_pdno;
            const std::string daytime_key = "R" + daytime_market + item.ovrs_pdno;
            return daytime
                ? subscription_keys{ daytime_key, regular_key }
                : subscription_keys{ regular_key, daytime_key };
        }

        std::string market = exchange;
        if (exchange == "SEHK") market = "HKS";
        else if (exchange == "SHAA") market = "SHS";
        else if (exchange == "SZAA") market = "SZS";
        else if (exchange == "TKSE") market = "TSE";
        else if (exchange == "VNSE") market = "HSX";
        else if (exchange == "HASE") market = "HNX";
        if (market.size() != 3) market = "NAS";
        return { "D" + market + item.ovrs_pdno, {} };
    }
}

class KISStreamClient::implementation {
public:
    using websocket_type = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    ~implementation()
    {
        stop();
    }

    bool start(std::string app_key, std::string app_secret,
        std::vector<kis_domain::balance_item1> items,
        message_callback on_message, status_callback on_status)
    {
        std::scoped_lock lifecycle_lock(_lifecycle_mutex);
        if (_running) return false;
        if (_worker.joinable()) _worker.join();

        _running = true;
        _io_context.restart();
        try {
            _worker = std::thread([this,
                app_key = std::move(app_key),
                app_secret = std::move(app_secret),
                items = std::move(items),
                on_message = std::move(on_message),
                on_status = std::move(on_status)]() mutable {
                run(std::move(app_key), std::move(app_secret), std::move(items),
                    std::move(on_message), std::move(on_status));
            });
        }
        catch (...) {
            _running = false;
            throw;
        }
        return true;
    }

    void stop()
    {
        _running = false;
        {
            std::scoped_lock socket_lock(_socket_mutex);
            if (_active_socket) {
                boost::system::error_code ignored;
                _active_socket->cancel(ignored);
                _active_socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                _active_socket->close(ignored);
            }
        }
        _io_context.stop();

        std::scoped_lock lifecycle_lock(_lifecycle_mutex);
        if (_worker.joinable() && _worker.get_id() != std::this_thread::get_id()) {
            _worker.join();
        }
    }

private:
    void run(std::string app_key, std::string app_secret,
        std::vector<kis_domain::balance_item1> items,
        message_callback on_message, status_callback on_status)
    {
        m1::util::scope_exit reset_running([this]() { _running = false; });
        try {
            const std::string approval_identity = app_key + '\n' + app_secret;
            if (_approval_key.empty()
                || _approval_identity != approval_identity
                || std::chrono::steady_clock::now() - _approval_issued_at >= std::chrono::hours(23)) {
                KISClient approval_client;
                if (!approval_client.request_ws_token(app_key, app_secret, _approval_key)) {
                    if (on_status) on_status(3, approval_client.getLastError());
                    return;
                }
                _approval_identity = approval_identity;
                _approval_issued_at = std::chrono::steady_clock::now();
            }
            if (!_running) return;

            websocket_type websocket(_io_context);
            {
                std::scoped_lock socket_lock(_socket_mutex);
                _active_socket = &websocket.next_layer();
            }
            m1::util::scope_exit clear_socket([this]() {
                std::scoped_lock socket_lock(_socket_mutex);
                _active_socket = nullptr;
            });

            boost::asio::ip::tcp::resolver resolver(_io_context);
            const auto endpoints = resolver.resolve(stream_host, stream_port);
            if (!_running) return;
            boost::asio::connect(websocket.next_layer(), endpoints);
            websocket.handshake(stream_host, "/tryitout");
            websocket.text(true);
            if (!_running) return;

            // KIS HDFSCNT0 uses different tr_key values for the US daytime session.
            // Normal/extended US market: DNAS/DNYS/DAMS + ticker
            // US daytime session:        RBAQ/RBAY/RBAA + ticker
            // Subscribe to the active-session key first. If the 40-key allowance has
            // room, also keep the alternate key subscribed so an app left open across
            // the daytime/regular-session boundary continues receiving prices.
            std::vector<std::string> subscription_keys;
            std::vector<std::string> alternate_keys;
            std::unordered_set<std::string> subscribed;
            bool subscription_limit_reached = false;
            for (const auto& item : items) {
                if (item.ovrs_pdno.empty()) continue;
                const auto keys = make_subscription_keys(item);
                if (keys.primary.empty() || !subscribed.insert(keys.primary).second) continue;
                if (subscription_keys.size() >= 40) {
                    subscription_limit_reached = true;
                    break;
                }
                subscription_keys.push_back(keys.primary);
                if (!keys.secondary.empty()) alternate_keys.push_back(keys.secondary);
            }
            for (const auto& alternate_key : alternate_keys) {
                if (subscription_keys.size() >= 40) {
                    subscription_limit_reached = true;
                    break;
                }
                if (subscribed.insert(alternate_key).second) {
                    subscription_keys.push_back(alternate_key);
                }
            }

            if (subscription_keys.empty()) {
                if (on_status) on_status(3, "실시간으로 구독할 수 있는 종목이 없습니다.");
                return;
            }

            for (const auto& subscription_key : subscription_keys) {
                boost::json::object header;
                header["approval_key"] = _approval_key;
                header["custtype"] = "P";
                header["tr_type"] = "1";
                header["content-type"] = "utf-8";

                boost::json::object input;
                input["tr_id"] = "HDFSCNT0";
                input["tr_key"] = subscription_key;
                boost::json::object body;
                body["input"] = std::move(input);
                boost::json::object root;
                root["header"] = std::move(header);
                root["body"] = std::move(body);

                const std::string payload = boost::json::serialize(root);
                websocket.write(boost::asio::buffer(payload));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Do not mark the stream as connected until KIS actually acknowledges at
            // least one subscription. This makes an invalid daytime/regular key visible
            // instead of silently showing a connected state with no ticks.
            size_t ack_count = 0;
            size_t ack_success_count = 0;
            bool connected_notified = false;
            std::string first_subscription_error;

            boost::beast::flat_buffer buffer;
            while (_running) {
                buffer.clear();
                boost::system::error_code error;
                websocket.read(buffer, error);
                if (error) {
                    if (_running) {
                        TRACE(L"[KISStreamClient] read error: %hs\n", error.message().c_str());
                    }
                    break;
                }

                const std::string message = boost::beast::buffers_to_string(buffer.data());
                if (message.empty()) continue;

                // KIS sends an application-level PINGPONG JSON message. If this is not
                // answered, an otherwise idle connection can be closed by the server.
                // Reply with a websocket pong frame, matching the official KIS samples.
                if (message.front() != '0' && message.front() != '1') {
                    boost::system::error_code parse_error;
                    const boost::json::value parsed = boost::json::parse(message, parse_error);
                    if (!parse_error && parsed.is_object()) {
                        const auto& object = parsed.as_object();
                        const auto* header_value = object.if_contains("header");
                        if (header_value && header_value->is_object()) {
                            const auto& response_header = header_value->as_object();
                            const auto* tr_id = response_header.if_contains("tr_id");
                            if (tr_id && tr_id->is_string() && tr_id->as_string() == "PINGPONG") {
                                if (message.size() <= 125) {
                                    websocket.pong(boost::beast::websocket::ping_data{ message });
                                }
                                else {
                                    // Defensive fallback. KIS PINGPONG payloads are normally
                                    // short enough for a websocket control frame.
                                    websocket.write(boost::asio::buffer(message));
                                }
                                continue;
                            }

                            const auto* body_value = object.if_contains("body");
                            if (body_value && body_value->is_object()) {
                                const auto& response_body = body_value->as_object();
                                const auto* rt_cd_value = response_body.if_contains("rt_cd");
                                const auto* msg_value = response_body.if_contains("msg1");
                                if (rt_cd_value && rt_cd_value->is_string()) {
                                    const std::string rt_cd = rt_cd_value->as_string().c_str();
                                    const std::string msg = msg_value && msg_value->is_string()
                                        ? std::string(msg_value->as_string().c_str())
                                        : std::string{};
                                    const auto* key_value = response_header.if_contains("tr_key");
                                    const std::string tr_key = key_value && key_value->is_string()
                                        ? std::string(key_value->as_string().c_str())
                                        : std::string{};

                                    ++ack_count;
                                    const bool accepted = rt_cd == "0" || msg == "ALREADY IN SUBSCRIBE";
                                    if (accepted) {
                                        ++ack_success_count;
                                        if (!connected_notified) {
                                            connected_notified = true;
                                            if (on_status) on_status(1, subscription_limit_reached
                                                ? "실시간 구독 한도 40개까지만 연결했습니다."
                                                : std::string{});
                                        }
                                    }
                                    else if (first_subscription_error.empty()) {
                                        first_subscription_error = tr_key.empty()
                                            ? msg
                                            : tr_key + ": " + msg;
                                    }

                                    if (ack_count >= subscription_keys.size() && ack_success_count == 0) {
                                        if (on_status) on_status(3, first_subscription_error.empty()
                                            ? "KIS 실시간 구독 요청이 모두 거절되었습니다."
                                            : first_subscription_error);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    // Subscription ACK/error messages are system messages, not trade ticks.
                    continue;
                }

                // Receiving a realtime tick itself also proves that the subscription is live.
                if (!connected_notified) {
                    connected_notified = true;
                    if (on_status) on_status(1, subscription_limit_reached
                        ? "실시간 구독 한도 40개까지만 연결했습니다."
                        : std::string{});
                }
                if (_running && on_message) {
                    on_message(message);
                }
            }

            boost::system::error_code close_error;
            websocket.close(boost::beast::websocket::close_code::normal, close_error);
            if (close_error && _running) {
                TRACE(L"[KISStreamClient] close error: %s\n",
                    m1::string::string_to_wstring(close_error.message()).c_str());
            }
            if (on_status) on_status(_running ? 3 : 2,
                _running ? "WebSocket 연결이 예기치 않게 종료되었습니다." : std::string{});
        }
        catch (const std::exception& error) {
            TRACE(L"[KISStreamClient] exception: %hs\n", error.what());
            _approval_key.clear();
            _approval_identity.clear();
            _approval_issued_at = {};
            if (on_status && _running) on_status(3, error.what());
        }
    }

    std::atomic<bool> _running = false;
    boost::asio::io_context _io_context;
    std::mutex _lifecycle_mutex;
    std::mutex _socket_mutex;
    boost::asio::ip::tcp::socket* _active_socket = nullptr;
    std::thread _worker;
    std::string _approval_key;
    std::string _approval_identity;
    std::chrono::steady_clock::time_point _approval_issued_at;
};

KISStreamClient::KISStreamClient()
    : _implementation(std::make_unique<implementation>())
{
}

KISStreamClient::~KISStreamClient() = default;

bool KISStreamClient::start(std::string app_key, std::string app_secret,
    std::vector<kis_domain::balance_item1> items,
    message_callback on_message, status_callback on_status)
{
    return _implementation->start(std::move(app_key), std::move(app_secret), std::move(items),
        std::move(on_message), std::move(on_status));
}

void KISStreamClient::stop()
{
    _implementation->stop();
}
