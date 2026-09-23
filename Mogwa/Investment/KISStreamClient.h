#pragma once

#include "KISDomain.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class KISStreamClient {
public:
    using message_callback = std::function<void(const std::string&)>;
    using status_callback = std::function<void(int, const std::string&)>;

    KISStreamClient();
    ~KISStreamClient();

    KISStreamClient(const KISStreamClient&) = delete;
    KISStreamClient& operator=(const KISStreamClient&) = delete;

    bool start(std::string app_key, std::string app_secret,
        std::vector<kis_domain::balance_item1> items,
        message_callback on_message, status_callback on_status = {});
    void stop();

private:
    class implementation;
    std::unique_ptr<implementation> _implementation;
};
