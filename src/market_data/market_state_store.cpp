#include "market_data/market_state_store.hpp"

namespace trading::market_data {

// Applies one market event by updating the stored bid, ask, trade, and timestamp fields.
void MarketStateStore::apply(const trading::core::MarketEvent& event) {
    auto& state = states_[event.instrument.instrument_id];

    // Step 1: Ignore out-of-order events so older data cannot overwrite newer state.
    if (state.last_process_timestamp > 0 && event.process_timestamp < state.last_process_timestamp) {
        return;
    }

    // Step 2: Update quote fields when the event carries them.
    if (event.bid_price.has_value()) {
        state.best_bid = event.bid_price;
    }
    if (event.ask_price.has_value()) {
        state.best_ask = event.ask_price;
    }

    // Step 3: Update the last trade view when quantity and price are present.
    if (event.quantity > 0.0 && event.price > 0.0) {
        state.last_trade_price = event.price;
        state.last_trade_quantity = event.quantity;
    }

    // Step 4: Record the latest event timestamps.
    state.last_exchange_timestamp = event.exchange_timestamp;
    state.last_receive_timestamp = event.receive_timestamp;
    state.last_process_timestamp = event.process_timestamp;
}

// Returns the stored state when the instrument exists.
std::optional<MarketState> MarketStateStore::get(const std::string& instrument_id) const {
    if (const auto iterator = states_.find(instrument_id); iterator != states_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

// Returns true when the stored timestamp is older than the allowed threshold.
bool MarketStateStore::is_stale(const std::string& instrument_id, const std::int64_t stale_after_ms) const {
    const auto state = get(instrument_id);
    if (!state.has_value()) {
        return true;
    }

    return (clock_.now_ms() - state->last_process_timestamp) > stale_after_ms;
}

void MarketStateStore::restore_snapshot(const std::string& instrument_id, const MarketState& snapshot) {
    states_[instrument_id] = snapshot;
}

}  // namespace trading::market_data
