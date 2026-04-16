#pragma once

#include "execution/order_book.hpp"
#include "execution/simulated_execution_engine.hpp"
#include "storage/storage_interfaces.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace trading::execution {

// Performs deterministic local matching against per-instrument resting books.
class MatchingEngine {
public:
    // Submits one order request and returns the resulting lifecycle updates and fills.
    [[nodiscard]] ExecutionResult submit(const trading::core::OrderRequest& request);

    // Cancels one currently resting order and returns the resulting lifecycle updates.
    [[nodiscard]] ExecutionResult cancel(const std::string& order_id, const std::string& client_order_id);

    // Restores one partially filled or fully resting order into the local book.
    void restore_open_order(const std::string& order_id,
                            const std::string& client_order_id,
                            const trading::core::OrderRequest& request,
                            double filled_quantity);

    // Returns the tracked order lifecycle state when it exists.
    [[nodiscard]] std::optional<trading::storage::OrderRecord> get_order(const std::string& order_id) const;

    // Returns the local book for an instrument when it exists.
    [[nodiscard]] const OrderBook* find_book(const std::string& instrument_id) const;

private:
    [[nodiscard]] OrderBook& book_for(const trading::core::Instrument& instrument);
    [[nodiscard]] trading::core::OrderUpdate make_update(const std::string& order_id,
                                                         const std::string& client_order_id,
                                                         trading::core::OrderStatus status,
                                                         double filled_quantity,
                                                         std::optional<std::string> reason = std::nullopt) const;
    [[nodiscard]] bool crosses(const trading::core::OrderRequest& incoming,
                               const RestingOrder& resting) const;
    [[nodiscard]] bool is_terminal(trading::core::OrderStatus status) const;
    void apply_fill_to_order(const std::string& order_id, double executed_quantity);

    std::unordered_map<std::string, OrderBook> books_;
    std::unordered_map<std::string, trading::storage::OrderRecord> orders_;
    std::size_t next_order_id_ {1};
    std::size_t next_client_order_id_ {1};
    std::size_t next_fill_id_ {1};
};

}  // namespace trading::execution
