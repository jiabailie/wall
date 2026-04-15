#include "exchange/live_market_data_feed.hpp"

#include <stdexcept>

namespace trading::exchange {

void LiveMarketDataFeedController::start() {
    session_.connect();
    session_.subscribe(symbols_);
    connected_ = true;
}

void LiveMarketDataFeedController::handle_disconnect() {
    connected_ = false;
    session_.disconnect();

    if (reconnect_attempts_ >= max_reconnect_attempts_) {
        throw std::runtime_error("market-data reconnect attempts exhausted");
    }

    ++reconnect_attempts_;
    session_.connect();
    session_.subscribe(symbols_);
    connected_ = true;
}

}  // namespace trading::exchange
