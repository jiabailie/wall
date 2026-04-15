#include "storage/storage_interfaces.hpp"

namespace trading::storage {

// Appends a newly received transaction to the in-memory repository.
void InMemoryTransactionRepository::save_received(const trading::core::TransactionCommand& command) {
    // Step 1: Copy the durable metadata into the in-memory record list.
    records_.push_back({
        .transaction_id = command.transaction_id,
        .status = "received",
        .kafka_topic = command.kafka_topic,
        .kafka_partition = command.kafka_partition,
        .kafka_offset = command.kafka_offset,
    });
}

// Updates the status of the matching transaction record.
void InMemoryTransactionRepository::save_processed(const std::string& transaction_id, const std::string& status) {
    // Step 1: Find the matching record and replace its status.
    for (auto& record : records_) {
        if (record.transaction_id == transaction_id) {
            record.status = status;
            return;
        }
    }

    // Step 2: Add a fallback record when the transaction was not seen earlier.
    records_.push_back({
        .transaction_id = transaction_id,
        .status = status,
        .kafka_topic = "",
        .kafka_partition = 0,
        .kafka_offset = 0,
    });
}

std::vector<TransactionRecord> InMemoryTransactionRepository::all_records() const {
    return records_;
}

// Stores the latest transaction status in insertion order.
void InMemoryTransactionCache::set_status(const std::string& transaction_id, const std::string& status) {
    // Step 1: Update the existing entry when it already exists.
    for (auto& [cached_transaction_id, cached_status] : statuses_) {
        if (cached_transaction_id == transaction_id) {
            cached_status = status;
            return;
        }
    }

    // Step 2: Append a new cache entry when no prior status exists.
    statuses_.emplace_back(transaction_id, status);
}

// Returns the cached status for the requested transaction id.
std::string InMemoryTransactionCache::get_status(const std::string& transaction_id) const {
    // Step 1: Scan the in-memory entries for a match.
    for (const auto& [cached_transaction_id, cached_status] : statuses_) {
        if (cached_transaction_id == transaction_id) {
            return cached_status;
        }
    }

    // Step 2: Return an empty string when no status exists.
    return "";
}

// Stores a newly created order record in memory.
void InMemoryOrderRepository::save_order(const trading::core::OrderRequest& request,
                                         const std::string& order_id,
                                         const std::string& client_order_id) {
    orders_[order_id] = {
        .order_id = order_id,
        .client_order_id = client_order_id,
        .strategy_id = request.strategy_id,
        .instrument_id = request.instrument.instrument_id,
        .side = request.side,
        .order_type = request.type,
        .status = trading::core::OrderStatus::created,
        .quantity = request.quantity,
        .price = request.price,
        .filled_quantity = 0.0,
    };
}

// Updates an existing order record from a lifecycle update.
void InMemoryOrderRepository::save_order_update(const trading::core::OrderUpdate& update) {
    if (auto iterator = orders_.find(update.order_id); iterator != orders_.end()) {
        iterator->second.client_order_id = update.client_order_id;
        iterator->second.status = update.status;
        iterator->second.filled_quantity = update.filled_quantity;
        return;
    }

    orders_[update.order_id] = {
        .order_id = update.order_id,
        .client_order_id = update.client_order_id,
        .status = update.status,
        .filled_quantity = update.filled_quantity,
    };
}

// Returns the latest stored order record.
std::optional<OrderRecord> InMemoryOrderRepository::get_order(const std::string& order_id) const {
    if (const auto iterator = orders_.find(order_id); iterator != orders_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

std::vector<OrderRecord> InMemoryOrderRepository::all_open_orders() const {
    std::vector<OrderRecord> open_orders;
    for (const auto& [_, order] : orders_) {
        switch (order.status) {
            case trading::core::OrderStatus::created:
            case trading::core::OrderStatus::pending_submit:
            case trading::core::OrderStatus::acknowledged:
            case trading::core::OrderStatus::partially_filled:
            case trading::core::OrderStatus::pending_cancel:
                open_orders.push_back(order);
                break;
            default:
                break;
        }
    }

    return open_orders;
}

// Appends one fill record in insertion order.
void InMemoryFillRepository::append_fill(const trading::core::FillEvent& fill) {
    fills_.push_back(fill);
}

// Returns all stored fills in insertion order.
std::vector<trading::core::FillEvent> InMemoryFillRepository::all_fills() const {
    return fills_;
}

// Stores the latest position keyed by instrument id.
void InMemoryPositionRepository::save_position(const trading::core::Position& position) {
    positions_[position.instrument.instrument_id] = position;
}

// Returns the stored position for the provided instrument id.
std::optional<trading::core::Position> InMemoryPositionRepository::get_position(const std::string& instrument_id) const {
    if (const auto iterator = positions_.find(instrument_id); iterator != positions_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

std::vector<trading::core::Position> InMemoryPositionRepository::all_positions() const {
    std::vector<trading::core::Position> positions;
    positions.reserve(positions_.size());
    for (const auto& [_, position] : positions_) {
        positions.push_back(position);
    }

    return positions;
}

void InMemoryBalanceRepository::save_balance(const trading::core::BalanceSnapshot& balance) {
    balances_[balance.asset] = balance;
}

std::optional<trading::core::BalanceSnapshot> InMemoryBalanceRepository::get_balance(const std::string& asset) const {
    if (const auto iterator = balances_.find(asset); iterator != balances_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

std::vector<trading::core::BalanceSnapshot> InMemoryBalanceRepository::all_balances() const {
    std::vector<trading::core::BalanceSnapshot> balances;
    balances.reserve(balances_.size());
    for (const auto& [_, balance] : balances_) {
        balances.push_back(balance);
    }

    return balances;
}

// Stores or replaces the latest market snapshot for one instrument.
void InMemoryMarketStateCache::upsert_snapshot(const MarketSnapshot& snapshot) {
    snapshots_[snapshot.instrument_id] = snapshot;
}

// Returns the cached market snapshot for the provided instrument id.
std::optional<MarketSnapshot> InMemoryMarketStateCache::get_snapshot(const std::string& instrument_id) const {
    if (const auto iterator = snapshots_.find(instrument_id); iterator != snapshots_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

}  // namespace trading::storage
