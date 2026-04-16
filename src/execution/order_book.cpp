#include "execution/order_book.hpp"

#include <numeric>
#include <utility>

namespace trading::execution {

bool OrderBook::add_order(const trading::core::OrderRequest& request,
                          const std::string& order_id,
                          const std::string& client_order_id,
                          const double filled_quantity) {
    if (!is_request_compatible(request, order_id, client_order_id, filled_quantity)) {
        return false;
    }

    const auto remaining_quantity = request.quantity - filled_quantity;
    RestingOrder resting_order {
        .order_id = order_id,
        .client_order_id = client_order_id,
        .request = request,
        .remaining_quantity = remaining_quantity,
        .sequence_number = next_sequence_number_++,
    };

    const auto price = *request.price;
    if (request.side == trading::core::OrderSide::buy) {
        bid_levels_[price].push_back(resting_order);
    } else {
        ask_levels_[price].push_back(resting_order);
    }

    order_index_[order_id] = OrderLocator {
        .side = request.side,
        .price = price,
    };
    ++order_count_;
    return true;
}

bool OrderBook::cancel_order(const std::string& order_id) {
    const auto locator_iterator = order_index_.find(order_id);
    if (locator_iterator == order_index_.end()) {
        return false;
    }

    auto erase_from_levels = [&](auto& levels) {
        const auto level_iterator = levels.find(locator_iterator->second.price);
        if (level_iterator == levels.end()) {
            return false;
        }

        auto& orders = level_iterator->second;
        for (auto order_iterator = orders.begin(); order_iterator != orders.end(); ++order_iterator) {
            if (order_iterator->order_id == order_id) {
                orders.erase(order_iterator);
                if (orders.empty()) {
                    levels.erase(level_iterator);
                }
                return true;
            }
        }

        return false;
    };

    const auto erased = locator_iterator->second.side == trading::core::OrderSide::buy
        ? erase_from_levels(bid_levels_)
        : erase_from_levels(ask_levels_);
    if (!erased) {
        return false;
    }

    order_index_.erase(locator_iterator);
    --order_count_;
    return true;
}

std::optional<OrderBookLevel> OrderBook::best_bid() const {
    if (bid_levels_.empty()) {
        return std::nullopt;
    }

    return make_level(*bid_levels_.begin());
}

std::optional<OrderBookLevel> OrderBook::best_ask() const {
    if (ask_levels_.empty()) {
        return std::nullopt;
    }

    return make_level(*ask_levels_.begin());
}

std::vector<OrderBookLevel> OrderBook::bid_levels() const {
    std::vector<OrderBookLevel> levels;
    levels.reserve(bid_levels_.size());
    for (const auto& level : bid_levels_) {
        levels.push_back(make_level(level));
    }

    return levels;
}

std::vector<OrderBookLevel> OrderBook::ask_levels() const {
    std::vector<OrderBookLevel> levels;
    levels.reserve(ask_levels_.size());
    for (const auto& level : ask_levels_) {
        levels.push_back(make_level(level));
    }

    return levels;
}

std::optional<RestingOrder> OrderBook::find_order(const std::string& order_id) const {
    const auto locator_iterator = order_index_.find(order_id);
    if (locator_iterator == order_index_.end()) {
        return std::nullopt;
    }

    const auto find_in_levels = [&](const auto& levels) -> std::optional<RestingOrder> {
        const auto level_iterator = levels.find(locator_iterator->second.price);
        if (level_iterator == levels.end()) {
            return std::nullopt;
        }

        for (const auto& order : level_iterator->second) {
            if (order.order_id == order_id) {
                return order;
            }
        }

        return std::nullopt;
    };

    return locator_iterator->second.side == trading::core::OrderSide::buy
        ? find_in_levels(bid_levels_)
        : find_in_levels(ask_levels_);
}

bool OrderBook::is_request_compatible(const trading::core::OrderRequest& request,
                                      const std::string& order_id,
                                      const std::string& client_order_id,
                                      const double filled_quantity) const {
    if (order_id.empty() || client_order_id.empty()) {
        return false;
    }
    if (request.instrument.instrument_id != instrument_.instrument_id) {
        return false;
    }
    if (request.type != trading::core::OrderType::limit) {
        return false;
    }
    if (!request.price.has_value() || *request.price <= 0.0) {
        return false;
    }
    if (request.quantity <= 0.0 || filled_quantity < 0.0 || filled_quantity >= request.quantity) {
        return false;
    }
    if (order_index_.contains(order_id)) {
        return false;
    }

    return true;
}

OrderBookLevel OrderBook::make_level(const std::pair<const double, std::deque<RestingOrder>>& level) {
    return {
        .price = level.first,
        .total_quantity = std::accumulate(
            level.second.begin(),
            level.second.end(),
            0.0,
            [](const double total, const RestingOrder& order) {
                return total + order.remaining_quantity;
            }),
        .order_count = level.second.size(),
    };
}

}  // namespace trading::execution
