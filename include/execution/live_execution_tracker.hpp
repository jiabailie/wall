#pragma once

#include "core/types.hpp"
#include "storage/storage_interfaces.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trading::execution {

// Stores one normalized execution report emitted by a live exchange adapter.
struct ExecutionReport {
    std::string report_id;
    std::string order_id;
    std::string client_order_id;
    trading::core::Instrument instrument;
    trading::core::OrderSide side {trading::core::OrderSide::buy};
    trading::core::OrderStatus status {trading::core::OrderStatus::created};
    double cumulative_filled_quantity {0.0};
    std::optional<double> last_fill_price;
    double last_fill_fee {0.0};
    std::optional<std::string> reason;
    std::int64_t exchange_timestamp {0};
};

// Stores the output of applying one live execution report to local order state.
struct ReconciliationResult {
    bool applied {false};
    bool ignored_duplicate {false};
    bool ignored_stale {false};
    std::vector<trading::core::OrderUpdate> updates;
    std::vector<trading::core::FillEvent> fills;
};

// Tracks live order lifecycle state and reconciles normalized exchange reports.
class LiveExecutionTracker {
public:
    // Registers a newly submitted local order so later exchange reports can be reconciled.
    void register_order(const trading::core::OrderRequest& request,
                        const std::string& order_id,
                        const std::string& client_order_id);

    // Restores one durable local order record during startup recovery.
    void restore_order(const trading::storage::OrderRecord& order);

    // Applies one normalized exchange execution report with duplicate and stale-report protection.
    [[nodiscard]] ReconciliationResult apply_report(const ExecutionReport& report);

    // Applies a batch of reports in order and returns each reconciliation result.
    [[nodiscard]] std::vector<ReconciliationResult> reconcile(const std::vector<ExecutionReport>& reports);

    // Returns the current local order state, if known.
    [[nodiscard]] std::optional<trading::storage::OrderRecord> get_order(const std::string& order_id) const;

private:
    struct LocalOrderState {
        trading::core::OrderRequest request;
        std::string order_id;
        std::string client_order_id;
        trading::core::OrderStatus status {trading::core::OrderStatus::created};
        double filled_quantity {0.0};
        std::int64_t last_exchange_timestamp {0};
    };

    [[nodiscard]] static bool is_terminal(trading::core::OrderStatus status);
    [[nodiscard]] static bool should_ignore_as_stale(const LocalOrderState& state, const ExecutionReport& report);
    [[nodiscard]] static bool should_apply_status(trading::core::OrderStatus current, trading::core::OrderStatus incoming);

    std::unordered_map<std::string, LocalOrderState> orders_;
    std::unordered_set<std::string> processed_report_ids_;
    std::size_t next_fill_id_ {1};
};

}  // namespace trading::execution
