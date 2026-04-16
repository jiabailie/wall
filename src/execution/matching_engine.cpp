#include "execution/matching_engine.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace trading::execution {

namespace {

constexpr double kQuantityTolerance = 1e-9;

}  // namespace

ExecutionResult MatchingEngine::submit(const trading::core::OrderRequest& request) {
    std::stringstream order_id_builder;
    order_id_builder << "match-order-" << next_order_id_++;

    std::stringstream client_order_id_builder;
    client_order_id_builder << "match-client-" << next_client_order_id_++;

    ExecutionResult result {
        .order_id = order_id_builder.str(),
        .client_order_id = client_order_id_builder.str(),
    };
    orders_[result.order_id] = trading::storage::OrderRecord {
        .order_id = result.order_id,
        .client_order_id = result.client_order_id,
        .strategy_id = request.strategy_id,
        .instrument_id = request.instrument.instrument_id,
        .side = request.side,
        .order_type = request.type,
        .status = trading::core::OrderStatus::created,
        .quantity = request.quantity,
        .price = request.price,
        .filled_quantity = 0.0,
    };

    if (request.type == trading::core::OrderType::market) {
        orders_[result.order_id].status = trading::core::OrderStatus::rejected;
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "market orders are not supported by the matching engine"));
        return result;
    }
    if (!request.price.has_value() || *request.price <= 0.0 || request.quantity <= 0.0) {
        orders_[result.order_id].status = trading::core::OrderStatus::rejected;
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "invalid matching-engine order request"));
        return result;
    }

    auto& book = book_for(request.instrument);
    orders_[result.order_id].status = trading::core::OrderStatus::acknowledged;
    result.updates.push_back(make_update(
        result.order_id,
        result.client_order_id,
        trading::core::OrderStatus::acknowledged,
        0.0));

    double remaining_quantity = request.quantity;
    while (remaining_quantity > kQuantityTolerance) {
        const auto resting_order = request.side == trading::core::OrderSide::buy
            ? book.best_ask_order()
            : book.best_bid_order();
        if (!resting_order.has_value() || !crosses(request, *resting_order)) {
            break;
        }

        const auto executed_quantity = std::min(remaining_quantity, resting_order->remaining_quantity);
        if (!book.apply_fill(resting_order->order_id, executed_quantity)) {
            break;
        }
        apply_fill_to_order(resting_order->order_id, executed_quantity);

        result.fills.push_back({
            .fill_id = "match-fill-" + std::to_string(next_fill_id_++),
            .order_id = result.order_id,
            .instrument = request.instrument,
            .side = request.side,
            .price = *resting_order->request.price,
            .quantity = executed_quantity,
            .fee = 0.0,
        });
        remaining_quantity = std::max(0.0, remaining_quantity - executed_quantity);
    }

    const auto filled_quantity = std::max(0.0, request.quantity - remaining_quantity);
    orders_[result.order_id].filled_quantity = filled_quantity;
    if (remaining_quantity > kQuantityTolerance) {
        const auto rested = book.add_order(request, result.order_id, result.client_order_id, filled_quantity);
        if (!rested) {
            orders_[result.order_id].status = trading::core::OrderStatus::rejected;
            result.updates.push_back(make_update(
                result.order_id,
                result.client_order_id,
                trading::core::OrderStatus::rejected,
                filled_quantity,
                "failed to rest residual order quantity"));
            return result;
        }
    }

    if (std::abs(filled_quantity - request.quantity) <= kQuantityTolerance) {
        orders_[result.order_id].filled_quantity = request.quantity;
        orders_[result.order_id].status = trading::core::OrderStatus::filled;
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::filled,
            filled_quantity));
    } else if (filled_quantity > kQuantityTolerance) {
        orders_[result.order_id].status = trading::core::OrderStatus::partially_filled;
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::partially_filled,
            filled_quantity));
    }

    return result;
}

ExecutionResult MatchingEngine::process_market_event(const trading::core::MarketEvent& event) {
    ExecutionResult result;

    const auto book_iterator = books_.find(event.instrument.instrument_id);
    if (book_iterator == books_.end()) {
        return result;
    }

    auto& book = book_iterator->second;
    std::vector<std::string> touched_orders;

    const auto process_buy_resting_orders = [&](const std::vector<trading::core::BookLevel>& ask_levels) {
        for (const auto& level : ask_levels) {
            double available_quantity = level.quantity;
            while (available_quantity > kQuantityTolerance) {
                const auto resting_order = book.best_bid_order();
                if (!resting_order.has_value()) {
                    break;
                }
                if (!resting_order->request.price.has_value() || *resting_order->request.price + kQuantityTolerance < level.price) {
                    break;
                }

                const auto executed_quantity = std::min(available_quantity, resting_order->remaining_quantity);
                if (!book.apply_fill(resting_order->order_id, executed_quantity)) {
                    break;
                }

                apply_fill_to_order(resting_order->order_id, executed_quantity);
                result.fills.push_back({
                    .fill_id = "match-fill-" + std::to_string(next_fill_id_++),
                    .order_id = resting_order->order_id,
                    .instrument = resting_order->request.instrument,
                    .side = resting_order->request.side,
                    .price = level.price,
                    .quantity = executed_quantity,
                    .fee = 0.0,
                });
                touched_orders.push_back(resting_order->order_id);
                available_quantity = std::max(0.0, available_quantity - executed_quantity);
            }
        }
    };

    const auto process_sell_resting_orders = [&](const std::vector<trading::core::BookLevel>& bid_levels) {
        for (const auto& level : bid_levels) {
            double available_quantity = level.quantity;
            while (available_quantity > kQuantityTolerance) {
                const auto resting_order = book.best_ask_order();
                if (!resting_order.has_value()) {
                    break;
                }
                if (!resting_order->request.price.has_value() || *resting_order->request.price - kQuantityTolerance > level.price) {
                    break;
                }

                const auto executed_quantity = std::min(available_quantity, resting_order->remaining_quantity);
                if (!book.apply_fill(resting_order->order_id, executed_quantity)) {
                    break;
                }

                apply_fill_to_order(resting_order->order_id, executed_quantity);
                result.fills.push_back({
                    .fill_id = "match-fill-" + std::to_string(next_fill_id_++),
                    .order_id = resting_order->order_id,
                    .instrument = resting_order->request.instrument,
                    .side = resting_order->request.side,
                    .price = level.price,
                    .quantity = executed_quantity,
                    .fee = 0.0,
                });
                touched_orders.push_back(resting_order->order_id);
                available_quantity = std::max(0.0, available_quantity - executed_quantity);
            }
        }
    };

    process_buy_resting_orders(event.ask_levels);
    process_sell_resting_orders(event.bid_levels);

    std::sort(touched_orders.begin(), touched_orders.end());
    touched_orders.erase(std::unique(touched_orders.begin(), touched_orders.end()), touched_orders.end());
    for (const auto& order_id : touched_orders) {
        append_order_update(result, order_id);
    }

    return result;
}

ExecutionResult MatchingEngine::cancel(const std::string& order_id, const std::string& client_order_id) {
    ExecutionResult result {
        .order_id = order_id,
        .client_order_id = client_order_id,
    };

    const auto order_iterator = orders_.find(order_id);
    if (order_iterator == orders_.end()) {
        result.updates.push_back(make_update(
            order_id,
            client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "open order not found"));
        return result;
    }

    result.client_order_id = order_iterator->second.client_order_id;
    if (is_terminal(order_iterator->second.status)) {
        result.updates.push_back(make_update(
            order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            order_iterator->second.filled_quantity,
            "order is not cancelable"));
        return result;
    }

    const auto book_iterator = books_.find(order_iterator->second.instrument_id);
    if (book_iterator == books_.end() || !book_iterator->second.cancel_order(order_id)) {
        result.updates.push_back(make_update(
            order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            order_iterator->second.filled_quantity,
            "open order not found"));
        return result;
    }

    order_iterator->second.status = trading::core::OrderStatus::pending_cancel;
    result.updates.push_back(make_update(
        order_id,
        result.client_order_id,
        trading::core::OrderStatus::pending_cancel,
        order_iterator->second.filled_quantity));

    order_iterator->second.status = trading::core::OrderStatus::canceled;
    result.updates.push_back(make_update(
        order_id,
        result.client_order_id,
        trading::core::OrderStatus::canceled,
        order_iterator->second.filled_quantity));
    return result;
}

void MatchingEngine::restore_open_order(const std::string& order_id,
                                        const std::string& client_order_id,
                                        const trading::core::OrderRequest& request,
                                        const double filled_quantity) {
    if (!request.price.has_value() || *request.price <= 0.0 || request.quantity <= 0.0) {
        return;
    }
    if (filled_quantity < 0.0 || filled_quantity >= (request.quantity - kQuantityTolerance)) {
        return;
    }

    orders_[order_id] = trading::storage::OrderRecord {
        .order_id = order_id,
        .client_order_id = client_order_id,
        .strategy_id = request.strategy_id,
        .instrument_id = request.instrument.instrument_id,
        .side = request.side,
        .order_type = request.type,
        .status = filled_quantity > 0.0
            ? trading::core::OrderStatus::partially_filled
            : trading::core::OrderStatus::acknowledged,
        .quantity = request.quantity,
        .price = request.price,
        .filled_quantity = filled_quantity,
    };
    auto& book = book_for(request.instrument);
    const auto restored = book.add_order(request, order_id, client_order_id, filled_quantity);
    if (!restored) {
        orders_.erase(order_id);
    }
}

std::optional<trading::storage::OrderRecord> MatchingEngine::get_order(const std::string& order_id) const {
    if (const auto iterator = orders_.find(order_id); iterator != orders_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

const OrderBook* MatchingEngine::find_book(const std::string& instrument_id) const {
    const auto iterator = books_.find(instrument_id);
    if (iterator == books_.end()) {
        return nullptr;
    }

    return &iterator->second;
}

OrderBook& MatchingEngine::book_for(const trading::core::Instrument& instrument) {
    const auto [iterator, _] = books_.try_emplace(instrument.instrument_id, instrument);
    return iterator->second;
}

trading::core::OrderUpdate MatchingEngine::make_update(const std::string& order_id,
                                                       const std::string& client_order_id,
                                                       const trading::core::OrderStatus status,
                                                       const double filled_quantity,
                                                       std::optional<std::string> reason) const {
    return {
        .order_id = order_id,
        .client_order_id = client_order_id,
        .status = status,
        .filled_quantity = filled_quantity,
        .reason = std::move(reason),
    };
}

bool MatchingEngine::crosses(const trading::core::OrderRequest& incoming,
                             const RestingOrder& resting) const {
    if (!incoming.price.has_value() || !resting.request.price.has_value()) {
        return false;
    }

    if (incoming.side == trading::core::OrderSide::buy) {
        return *incoming.price >= *resting.request.price;
    }

    return *incoming.price <= *resting.request.price;
}

bool MatchingEngine::is_terminal(const trading::core::OrderStatus status) const {
    return status == trading::core::OrderStatus::filled
        || status == trading::core::OrderStatus::canceled
        || status == trading::core::OrderStatus::rejected
        || status == trading::core::OrderStatus::expired;
}

void MatchingEngine::apply_fill_to_order(const std::string& order_id, const double executed_quantity) {
    const auto iterator = orders_.find(order_id);
    if (iterator == orders_.end() || executed_quantity <= kQuantityTolerance) {
        return;
    }

    auto& order = iterator->second;
    order.filled_quantity = std::min(order.quantity, order.filled_quantity + executed_quantity);
    if (std::abs(order.filled_quantity - order.quantity) <= kQuantityTolerance) {
        order.filled_quantity = order.quantity;
    }
    order.status = order.filled_quantity >= (order.quantity - kQuantityTolerance)
        ? trading::core::OrderStatus::filled
        : trading::core::OrderStatus::partially_filled;
}

void MatchingEngine::append_order_update(ExecutionResult& result, const std::string& order_id) {
    const auto iterator = orders_.find(order_id);
    if (iterator == orders_.end()) {
        return;
    }

    result.updates.push_back(make_update(
        iterator->second.order_id,
        iterator->second.client_order_id,
        iterator->second.status,
        iterator->second.filled_quantity));
}

}  // namespace trading::execution
