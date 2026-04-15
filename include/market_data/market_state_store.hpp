#pragma once

#include "core/clock.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace trading::market_data {

// Stores the latest cached state for one instrument.
struct MarketState {
    std::optional<double> best_bid;
    std::optional<double> best_ask;
    std::optional<double> last_trade_price;
    std::optional<double> last_trade_quantity;
    std::int64_t last_exchange_timestamp {0};
    std::int64_t last_receive_timestamp {0};
    std::int64_t last_process_timestamp {0};
};

// Maintains normalized market state for risk and strategy lookups.
class MarketStateStore {
public:
    // Builds the store using an injected clock for stale-state checks.
    explicit MarketStateStore(const trading::core::IClock& clock) : clock_(clock) {}

    // Applies one normalized market event to the in-memory state.
    void apply(const trading::core::MarketEvent& event);

    // Returns the latest state for the requested instrument id, if available.
    std::optional<MarketState> get(const std::string& instrument_id) const;

    // Returns true when the instrument has not been updated within the threshold.
    bool is_stale(const std::string& instrument_id, std::int64_t stale_after_ms) const;

    // Restores one persisted market snapshot into the in-memory state store.
    void restore_snapshot(const std::string& instrument_id, const MarketState& snapshot);

private:
    const trading::core::IClock& clock_;
    std::unordered_map<std::string, MarketState> states_;
};

}  // namespace trading::market_data
