#pragma once

#include <string>

namespace app_config {
    struct settings {
        std::string kis_app_key;
        std::string kis_app_secret;
        std::string kis_account_number;
        std::string kis_account_product_code = "01";
        std::string openai_api_key;

        [[nodiscard]] bool has_kis_credentials() const noexcept;
        [[nodiscard]] bool has_openai_api_key() const noexcept;
    };

    [[nodiscard]] settings load();
    [[nodiscard]] bool store_kis_credentials(const settings& value);
    [[nodiscard]] bool clear_kis_credentials();
    [[nodiscard]] bool store_openai_api_key(const std::string& api_key);
    [[nodiscard]] bool clear_openai_api_key();
}
