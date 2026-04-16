#include "execution/matching_engine.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace trading::execution {

ExecutionResult MatchingEngine::submit(const trading::core::OrderRequest& request) {
    std::stringstream order_id_builder;
    order_id_builder << "match-order-" << next_order_id_++;

    std::stringstream client_order_id_builder;
    client_order_id_builder << "match-client-" << next_client_order_id_++;

    ExecutionResult result {
        .order_id = order_id_builder.str(),
        .client_order_id = client_order_id_builder.str(),
    };

    if (request.type == trading::core::OrderType::market) {
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "market orders are not supported by the matching engine"));
        return result;
    }
    if (!request.price.has_value() || *request.price <= 0.0 || request.quantity <= 0.0) {
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "invalid matching-engine order request"));
        return result;
    }

    auto& book = book_for(request.instrument);
    result.updates.push_back(make_update(
        result.order_id,
        result.client_order_id,
        trading::core::OrderStatus::acknowledged,
        0.0));

    double remaining_quantity = request.quantity;
    while (remaining_quantity > 0.0) {
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

        result.fills.push_back({
            .fill_id = "match-fill-" + std::to_string(next_fill_id_++),
            .order_id = result.order_id,
            .instrument = request.instrument,
            .side = request.side,
            .price = *resting_order->request.price,
            .quantity = executed_quantity,
            .fee = 0.0,
        });
        remaining_quantity -= executed_quantity;
    }

    const auto filled_quantity = request.quantity - remaining_quantity;
    if (remaining_quantity > 0.0) {
        const auto rested = book.add_order(request, result.order_id, result.client_order_id, filled_quantity);
        static_cast<void>(rested);
    }

    if (filled_quantity == request.quantity) {
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::filled,
            filled_quantity));
    } else if (filled_quantity > 0.0) {
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::partially_filled,
            filled_quantity));
    }

    return result;
}

void MatchingEngine::restore_open_order(const std::string& order_id,
                                        const std::string& client_order_id,
                                        const trading::core::OrderRequest& request,
                                        const double filled_quantity) {
    auto& book = book_for(request.instrument);
    const auto restored = book.add_order(request, order_id, client_order_id, filled_quantity);
    static_cast<void>(restored);
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

}  // namespace trading::execution
