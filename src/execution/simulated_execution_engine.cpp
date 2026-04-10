#include "execution/simulated_execution_engine.hpp"

#include <algorithm>
#include <sstream>

namespace trading::execution {

// Submits one order request and returns the initial lifecycle events.
ExecutionResult SimulatedExecutionEngine::submit(const trading::core::OrderRequest& request) {
    std::stringstream order_id_builder;
    order_id_builder << "sim-order-" << next_order_id_++;

    std::stringstream client_order_id_builder;
    client_order_id_builder << "sim-client-" << next_client_order_id_++;

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
            "market orders are not supported by the simulator"));
        return result;
    }
    if (!request.price.has_value() || request.quantity <= 0.0) {
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::rejected,
            0.0,
            "invalid simulated order request"));
        return result;
    }

    result.updates.push_back(make_update(
        result.order_id,
        result.client_order_id,
        trading::core::OrderStatus::acknowledged,
        0.0));

    if (request.quantity >= config_.partial_fill_threshold && config_.partial_fill_threshold > 0.0) {
        const auto partial_fill_quantity = request.quantity * clamp_partial_fill_ratio();
        result.updates.push_back(make_update(
            result.order_id,
            result.client_order_id,
            trading::core::OrderStatus::partially_filled,
            partial_fill_quantity));
        result.fills.push_back({
            .fill_id = "sim-fill-" + std::to_string(next_fill_id_++),
            .order_id = result.order_id,
            .instrument = request.instrument,
            .side = request.side,
            .price = *request.price,
            .quantity = partial_fill_quantity,
            .fee = 0.0,
        });

        open_orders_.emplace(result.order_id, OpenOrderState {
            .request = request,
            .client_order_id = result.client_order_id,
            .filled_quantity = partial_fill_quantity,
        });
        return result;
    }

    result.updates.push_back(make_update(
        result.order_id,
        result.client_order_id,
        trading::core::OrderStatus::filled,
        request.quantity));
    result.fills.push_back({
        .fill_id = "sim-fill-" + std::to_string(next_fill_id_++),
        .order_id = result.order_id,
        .instrument = request.instrument,
        .side = request.side,
        .price = *request.price,
        .quantity = request.quantity,
        .fee = 0.0,
    });
    return result;
}

// Completes an open partially-filled order and returns the final lifecycle events.
ExecutionResult SimulatedExecutionEngine::complete_open_order(const std::string& order_id) {
    ExecutionResult result {
        .order_id = order_id,
    };

    const auto iterator = open_orders_.find(order_id);
    if (iterator == open_orders_.end()) {
        result.updates.push_back(make_update(
            order_id,
            "",
            trading::core::OrderStatus::rejected,
            0.0,
            "open order not found"));
        return result;
    }

    const auto remaining_quantity = iterator->second.request.quantity - iterator->second.filled_quantity;
    result.client_order_id = iterator->second.client_order_id;
    result.updates.push_back(make_update(
        order_id,
        result.client_order_id,
        trading::core::OrderStatus::filled,
        iterator->second.request.quantity));
    result.fills.push_back({
        .fill_id = "sim-fill-" + std::to_string(next_fill_id_++),
        .order_id = order_id,
        .instrument = iterator->second.request.instrument,
        .side = iterator->second.request.side,
        .price = *iterator->second.request.price,
        .quantity = remaining_quantity,
        .fee = 0.0,
    });
    open_orders_.erase(iterator);
    return result;
}

// Cancels an open partially-filled order and returns the cancel lifecycle events.
ExecutionResult SimulatedExecutionEngine::cancel_open_order(const std::string& order_id) {
    ExecutionResult result {
        .order_id = order_id,
    };

    const auto iterator = open_orders_.find(order_id);
    if (iterator == open_orders_.end()) {
        result.updates.push_back(make_update(
            order_id,
            "",
            trading::core::OrderStatus::rejected,
            0.0,
            "open order not found"));
        return result;
    }

    result.client_order_id = iterator->second.client_order_id;
    result.updates.push_back(make_update(
        order_id,
        result.client_order_id,
        trading::core::OrderStatus::pending_cancel,
        iterator->second.filled_quantity));
    result.updates.push_back(make_update(
        order_id,
        result.client_order_id,
        trading::core::OrderStatus::canceled,
        iterator->second.filled_quantity));
    open_orders_.erase(iterator);
    return result;
}

// Returns the configured partial fill ratio clamped into a safe range.
double SimulatedExecutionEngine::clamp_partial_fill_ratio() const {
    return std::clamp(config_.partial_fill_ratio, 0.0, 1.0);
}

// Builds one order lifecycle update payload.
trading::core::OrderUpdate SimulatedExecutionEngine::make_update(const std::string& order_id,
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

}  // namespace trading::execution
