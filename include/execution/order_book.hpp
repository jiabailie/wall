#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::execution {

// Summarizes one visible price level in the local book.
struct OrderBookLevel {
    double price {0.0};
    double total_quantity {0.0};
    std::size_t order_count {0};
};

// Stores one resting local order tracked inside the book.
struct RestingOrder {
    std::string order_id;
    std::string client_order_id;
    trading::core::OrderRequest request;
    double remaining_quantity {0.0};
    std::size_t sequence_number {0};
};

// Maintains one instrument-local limit order book with deterministic price-time ordering.
class OrderBook {
public:
    explicit OrderBook(trading::core::Instrument instrument) : instrument_(std::move(instrument)) {}

    // Adds one resting limit order when the request is valid for this book.
    [[nodiscard]] bool add_order(const trading::core::OrderRequest& request,
                                 const std::string& order_id,
                                 const std::string& client_order_id,
                                 double filled_quantity = 0.0);

    // Cancels one resting order by id and returns true when found.
    [[nodiscard]] bool cancel_order(const std::string& order_id);

    // Returns the current best bid level, if present.
    [[nodiscard]] std::optional<OrderBookLevel> best_bid() const;

    // Returns the current best ask level, if present.
    [[nodiscard]] std::optional<OrderBookLevel> best_ask() const;

    // Returns a snapshot of all current bid levels.
    [[nodiscard]] std::vector<OrderBookLevel> bid_levels() const;

    // Returns a snapshot of all current ask levels.
    [[nodiscard]] std::vector<OrderBookLevel> ask_levels() const;

    // Returns one resting order when it exists.
    [[nodiscard]] std::optional<RestingOrder> find_order(const std::string& order_id) const;

    // Returns the number of currently resting orders.
    [[nodiscard]] std::size_t order_count() const { return order_count_; }

    // Returns the instrument handled by this book.
    [[nodiscard]] const trading::core::Instrument& instrument() const { return instrument_; }

private:
    struct OrderLocator {
        trading::core::OrderSide side {trading::core::OrderSide::buy};
        double price {0.0};
    };

    using BidLevels = std::map<double, std::deque<RestingOrder>, std::greater<double>>;
    using AskLevels = std::map<double, std::deque<RestingOrder>, std::less<double>>;

    [[nodiscard]] bool is_request_compatible(const trading::core::OrderRequest& request,
                                             const std::string& order_id,
                                             const std::string& client_order_id,
                                             double filled_quantity) const;
    [[nodiscard]] static OrderBookLevel make_level(const std::pair<const double, std::deque<RestingOrder>>& level);

    trading::core::Instrument instrument_;
    BidLevels bid_levels_;
    AskLevels ask_levels_;
    std::unordered_map<std::string, OrderLocator> order_index_;
    std::size_t order_count_ {0};
    std::size_t next_sequence_number_ {1};
};

}  // namespace trading::execution
