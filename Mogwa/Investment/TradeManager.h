// TradeManager.h: TradeManager 헤더 파일.

#include "KISDomain.h"
#include "KISClient.h"
#include "KISStreamClient.h"
#include "AppConfig.h"
#include "DB/DBManager.h"
#include <atomic>

#pragma once

class TradeManager {
public:
    struct portfolio_holding {
        std::string ticker;
        std::string exchange;
        std::string name;
        double quantity = 0;
        double current_price = 0;
    };

    TradeManager(HWND parent);
    ~TradeManager();
private:
    HWND _parent;

    std::shared_ptr<KISClient> _client;
    std::unique_ptr<KISStreamClient> _stream_client;
    kis_domain::information_token _token;
    kis_domain::information_balance _balance;
    std::shared_ptr<DBManager> _db_manager;
    app_config::settings _settings;
    double _exchange_rate = 0.0;
    bool _credentials_available = false;
    std::string _last_error;

    std::thread _history_worker;
    std::thread _performance_worker;
    std::thread _quote_worker;
    std::atomic<bool> _history_loading = false;
    std::atomic<bool> _performance_loading = false;
    std::atomic<bool> _quote_loading = false;
    std::vector<kis_domain::balance_item1> _manual_stream_items;
    std::vector<kis_domain::balance_item1> _watch_stream_items;
public:
    bool initialize(bool force_token_refresh = false);
    bool updateBalance();
    bool updateExchangeRate();
    bool runMarketStream();
    void setManualSubscriptions(const std::vector<std::pair<std::string, std::string>>& subscriptions,
        bool restart_stream = true);
    void setWatchSubscriptions(const std::vector<std::pair<std::string, std::string>>& subscriptions,
        bool restart_stream = true);
    bool restartMarketStream();
    bool requestPortfolioHistory(std::vector<portfolio_holding> holdings);
    bool requestPortfolioPerformance(std::vector<portfolio_holding> holdings,
        const std::string& start_date, const std::string& end_date);
    bool requestRealtimeQuote(const std::string& ticker, const std::string& exchange);
    bool clearKisCredentials();
    bool saveKisCredentials(const std::string& app_key, const std::string& app_secret,
        const std::string& account_number, const std::string& product_code);
    bool saveOpenAiApiKey(const std::string& api_key);
    bool clearOpenAiApiKey();
    bool requestChatGPTAdvice(std::string& output);
    void stopMarketStream();
public:
    void on_receive_price_data(const std::string& msg);
public:
    const kis_domain::information_balance& getBalance() const { return _balance; }
    double getExchangeRate() const { return _exchange_rate; }
    bool hasKisCredentials() const { return _credentials_available; }
    bool hasOpenAiApiKey() const { return _settings.has_openai_api_key(); }
    const std::string& getLastError() const { return _last_error; }
};
