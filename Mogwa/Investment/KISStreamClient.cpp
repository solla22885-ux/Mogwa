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
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {
    constexpr std::string_view stream_host = "ops.koreainvestment.com";
    constexpr std::string_view stream_port = "21000";
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
            KISClient approval_client;
            std::string approval_key;
            if (!approval_client.request_ws_token(app_key, app_secret, approval_key)) {
                if (on_status) on_status(3, approval_client.getLastError());
                return;
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
            websocket.handshake(stream_host, "/");
            if (!_running) return;

            std::unordered_set<std::string> subscribed;
            for (const auto& item : items) {
                std::string exchange_prefix = "DNAS";
                if (item.ovrs_excg_cd == "NYSE" || item.ovrs_excg_cd == "NYS") exchange_prefix = "DNYS";
                else if (item.ovrs_excg_cd == "AMEX" || item.ovrs_excg_cd == "AMS") exchange_prefix = "DAMS";

                const std::string subscription_key = exchange_prefix + item.ovrs_pdno;
                if (!subscribed.insert(subscription_key).second) continue;

                boost::json::object header;
                header["approval_key"] = approval_key;
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
            }

            if (on_status) on_status(1, {});
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
                if (_running && on_message) {
                    on_message(boost::beast::buffers_to_string(buffer.data()));
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
            if (on_status && _running) on_status(3, error.what());
        }
    }

    std::atomic<bool> _running = false;
    boost::asio::io_context _io_context;
    std::mutex _lifecycle_mutex;
    std::mutex _socket_mutex;
    boost::asio::ip::tcp::socket* _active_socket = nullptr;
    std::thread _worker;
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
