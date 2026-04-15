#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace trading::exchange {

// Defines the low-level session boundary for a live market-data transport.
class IMarketDataFeedSession {
public:
    virtual ~IMarketDataFeedSession() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void subscribe(const std::vector<std::string>& symbols) = 0;
};

// Manages reconnect and resubscribe behavior for a market-data feed session.
class LiveMarketDataFeedController {
public:
    LiveMarketDataFeedController(IMarketDataFeedSession& session,
                                 std::vector<std::string> symbols,
                                 std::size_t max_reconnect_attempts)
        : session_(session),
          symbols_(std::move(symbols)),
          max_reconnect_attempts_(max_reconnect_attempts) {}

    void start();
    void handle_disconnect();

    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] std::size_t reconnect_attempts() const { return reconnect_attempts_; }

private:
    IMarketDataFeedSession& session_;
    std::vector<std::string> symbols_;
    std::size_t max_reconnect_attempts_ {0};
    std::size_t reconnect_attempts_ {0};
    bool connected_ {false};
};

// Test-friendly session double for reconnect and resubscribe workflows.
class MockMarketDataFeedSession final : public IMarketDataFeedSession {
public:
    void connect() override { ++connect_calls_; }
    void disconnect() override { ++disconnect_calls_; }
    void subscribe(const std::vector<std::string>& symbols) override {
        ++subscribe_calls_;
        last_symbols_ = symbols;
    }

    [[nodiscard]] std::size_t connect_calls() const { return connect_calls_; }
    [[nodiscard]] std::size_t disconnect_calls() const { return disconnect_calls_; }
    [[nodiscard]] std::size_t subscribe_calls() const { return subscribe_calls_; }
    [[nodiscard]] const std::vector<std::string>& last_symbols() const { return last_symbols_; }

private:
    std::size_t connect_calls_ {0};
    std::size_t disconnect_calls_ {0};
    std::size_t subscribe_calls_ {0};
    std::vector<std::string> last_symbols_;
};

}  // namespace trading::exchange
