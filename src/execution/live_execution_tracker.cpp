#include "execution/live_execution_tracker.hpp"

#include <algorithm>

namespace trading::execution {

void LiveExecutionTracker::register_order(const trading::core::OrderRequest& request,
                                          const std::string& order_id,
                                          const std::string& client_order_id) {
    orders_[order_id] = LocalOrderState {
        .request = request,
        .order_id = order_id,
        .client_order_id = client_order_id,
        .status = trading::core::OrderStatus::pending_submit,
        .filled_quantity = 0.0,
        .last_exchange_timestamp = 0,
    };
}

void LiveExecutionTracker::restore_order(const trading::storage::OrderRecord& order) {
    orders_[order.order_id] = LocalOrderState {
        .request = {
            .request_id = order.order_id,
            .strategy_id = order.strategy_id,
            .instrument = {
                .instrument_id = order.instrument_id,
            },
            .side = order.side,
            .type = order.order_type,
            .quantity = order.quantity,
            .price = order.price,
        },
        .order_id = order.order_id,
        .client_order_id = order.client_order_id,
        .status = order.status,
        .filled_quantity = order.filled_quantity,
        .last_exchange_timestamp = 0,
    };
}

ReconciliationResult LiveExecutionTracker::apply_report(const ExecutionReport& report) {
    ReconciliationResult result;

    if (!report.report_id.empty() && !processed_report_ids_.insert(report.report_id).second) {
        result.ignored_duplicate = true;
        return result;
    }

    const auto iterator = orders_.find(report.order_id);
    if (iterator == orders_.end()) {
        result.ignored_stale = true;
        return result;
    }

    auto& state = iterator->second;
    if (should_ignore_as_stale(state, report)) {
        result.ignored_stale = true;
        return result;
    }

    const double new_cumulative = std::max(report.cumulative_filled_quantity, state.filled_quantity);
    const double fill_delta = new_cumulative - state.filled_quantity;
    if (fill_delta > 0.0) {
        result.fills.push_back({
            .fill_id = !report.report_id.empty() ? report.report_id : ("live-fill-" + std::to_string(next_fill_id_++)),
            .order_id = report.order_id,
            .instrument = report.instrument.instrument_id.empty() ? state.request.instrument : report.instrument,
            .side = report.side,
            .price = report.last_fill_price.value_or(state.request.price.value_or(0.0)),
            .quantity = fill_delta,
            .fee = report.last_fill_fee,
        });
        state.filled_quantity = new_cumulative;
        result.applied = true;
    }

    if (should_apply_status(state.status, report.status) || result.applied) {
        state.status = report.status;
        state.client_order_id = report.client_order_id.empty() ? state.client_order_id : report.client_order_id;
        state.last_exchange_timestamp = std::max(state.last_exchange_timestamp, report.exchange_timestamp);
        result.updates.push_back({
            .order_id = report.order_id,
            .client_order_id = state.client_order_id,
            .status = state.status,
            .filled_quantity = state.filled_quantity,
            .reason = report.reason,
        });
        result.applied = true;
        return result;
    }

    if (report.exchange_timestamp > state.last_exchange_timestamp) {
        state.last_exchange_timestamp = report.exchange_timestamp;
    }
    return result;
}

std::vector<ReconciliationResult> LiveExecutionTracker::reconcile(const std::vector<ExecutionReport>& reports) {
    std::vector<ReconciliationResult> results;
    results.reserve(reports.size());
    for (const auto& report : reports) {
        results.push_back(apply_report(report));
    }

    return results;
}

std::optional<trading::storage::OrderRecord> LiveExecutionTracker::get_order(const std::string& order_id) const {
    const auto iterator = orders_.find(order_id);
    if (iterator == orders_.end()) {
        return std::nullopt;
    }

    return trading::storage::OrderRecord {
        .order_id = iterator->second.order_id,
        .client_order_id = iterator->second.client_order_id,
        .strategy_id = iterator->second.request.strategy_id,
        .instrument_id = iterator->second.request.instrument.instrument_id,
        .side = iterator->second.request.side,
        .order_type = iterator->second.request.type,
        .status = iterator->second.status,
        .quantity = iterator->second.request.quantity,
        .price = iterator->second.request.price,
        .filled_quantity = iterator->second.filled_quantity,
    };
}

bool LiveExecutionTracker::is_terminal(const trading::core::OrderStatus status) {
    return status == trading::core::OrderStatus::filled
        || status == trading::core::OrderStatus::canceled
        || status == trading::core::OrderStatus::rejected
        || status == trading::core::OrderStatus::expired;
}

bool LiveExecutionTracker::should_ignore_as_stale(const LocalOrderState& state, const ExecutionReport& report) {
    if (report.cumulative_filled_quantity < state.filled_quantity) {
        return true;
    }

    if (report.exchange_timestamp > 0
        && state.last_exchange_timestamp > 0
        && report.exchange_timestamp < state.last_exchange_timestamp
        && report.cumulative_filled_quantity <= state.filled_quantity) {
        return true;
    }

    if (is_terminal(state.status)
        && report.cumulative_filled_quantity <= state.filled_quantity
        && report.status != state.status) {
        return true;
    }

    return false;
}

bool LiveExecutionTracker::should_apply_status(const trading::core::OrderStatus current,
                                               const trading::core::OrderStatus incoming) {
    if (incoming == current) {
        return false;
    }
    if (is_terminal(current)) {
        return false;
    }

    switch (current) {
        case trading::core::OrderStatus::created:
        case trading::core::OrderStatus::pending_submit:
            return true;
        case trading::core::OrderStatus::acknowledged:
            return incoming == trading::core::OrderStatus::partially_filled
                || incoming == trading::core::OrderStatus::filled
                || incoming == trading::core::OrderStatus::pending_cancel
                || incoming == trading::core::OrderStatus::canceled
                || incoming == trading::core::OrderStatus::rejected
                || incoming == trading::core::OrderStatus::expired;
        case trading::core::OrderStatus::partially_filled:
            return incoming == trading::core::OrderStatus::partially_filled
                || incoming == trading::core::OrderStatus::filled
                || incoming == trading::core::OrderStatus::pending_cancel
                || incoming == trading::core::OrderStatus::canceled
                || incoming == trading::core::OrderStatus::rejected
                || incoming == trading::core::OrderStatus::expired;
        case trading::core::OrderStatus::pending_cancel:
            return incoming == trading::core::OrderStatus::canceled
                || incoming == trading::core::OrderStatus::filled
                || incoming == trading::core::OrderStatus::partially_filled;
        default:
            return false;
    }
}

}  // namespace trading::execution
