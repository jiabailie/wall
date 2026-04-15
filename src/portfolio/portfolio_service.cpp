#include "portfolio/portfolio_service.hpp"

#include <algorithm>
#include <cmath>

namespace trading::portfolio {

// Applies one fill and updates position and balance state.
void PortfolioService::apply_fill(const trading::core::FillEvent& fill) {
    auto& position = positions_[fill.instrument.instrument_id];
    if (position.position_id.empty()) {
        position.position_id = "position-" + fill.instrument.instrument_id;
        position.instrument = fill.instrument;
    }

    apply_fill_to_position(position, fill);
    apply_fill_to_balances(fill);
}

// Returns the latest position for the instrument, if any.
std::optional<trading::core::Position> PortfolioService::get_position(const std::string& instrument_id) const {
    if (const auto iterator = positions_.find(instrument_id); iterator != positions_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

// Returns the latest balance snapshot for the asset, or an empty snapshot.
trading::core::BalanceSnapshot PortfolioService::get_balance(const std::string& asset) const {
    if (const auto iterator = balances_.find(asset); iterator != balances_.end()) {
        return iterator->second;
    }

    return {
        .asset = asset,
        .total_balance = 0.0,
        .available_balance = 0.0,
    };
}

// Updates the mark price used for unrealized PnL calculations.
void PortfolioService::set_mark_price(const std::string& instrument_id, const double price) {
    mark_prices_[instrument_id] = price;
    if (const auto iterator = positions_.find(instrument_id); iterator != positions_.end()) {
        update_unrealized(iterator->second, price);
    }
}

void PortfolioService::restore_position(const trading::core::Position& position) {
    positions_[position.instrument.instrument_id] = position;
}

void PortfolioService::restore_balance(const trading::core::BalanceSnapshot& balance) {
    balances_[balance.asset] = balance;
}

std::vector<trading::core::Position> PortfolioService::all_positions() const {
    std::vector<trading::core::Position> positions;
    positions.reserve(positions_.size());
    for (const auto& [_, position] : positions_) {
        positions.push_back(position);
    }

    return positions;
}

std::vector<trading::core::BalanceSnapshot> PortfolioService::all_balances() const {
    std::vector<trading::core::BalanceSnapshot> balances;
    balances.reserve(balances_.size());
    for (const auto& [_, balance] : balances_) {
        balances.push_back(balance);
    }

    return balances;
}

// Returns the sign of the provided value.
double PortfolioService::sign(const double value) {
    if (value > 0.0) {
        return 1.0;
    }
    if (value < 0.0) {
        return -1.0;
    }

    return 0.0;
}

// Recomputes unrealized PnL from the current net quantity, average price, and mark price.
void PortfolioService::update_unrealized(trading::core::Position& position, const double mark_price) {
    if (position.net_quantity == 0.0) {
        position.unrealized_pnl = 0.0;
        return;
    }

    position.unrealized_pnl = (mark_price - position.average_entry_price) * position.net_quantity;
}

// Applies one signed fill quantity to the position state.
void PortfolioService::apply_fill_to_position(trading::core::Position& position, const trading::core::FillEvent& fill) {
    const auto signed_fill_quantity = fill.side == trading::core::OrderSide::buy ? fill.quantity : -fill.quantity;
    const auto previous_quantity = position.net_quantity;

    if (previous_quantity == 0.0 || sign(previous_quantity) == sign(signed_fill_quantity)) {
        const auto new_quantity = previous_quantity + signed_fill_quantity;
        const auto weighted_notional =
            (std::abs(previous_quantity) * position.average_entry_price) +
            (std::abs(signed_fill_quantity) * fill.price);
        position.net_quantity = new_quantity;
        position.average_entry_price = std::abs(new_quantity) > 0.0
            ? weighted_notional / std::abs(new_quantity)
            : 0.0;
    } else {
        const auto closing_quantity = std::min(std::abs(previous_quantity), std::abs(signed_fill_quantity));
        position.realized_pnl += closing_quantity * (fill.price - position.average_entry_price) * sign(previous_quantity);
        position.net_quantity = previous_quantity + signed_fill_quantity;

        if (position.net_quantity == 0.0) {
            position.average_entry_price = 0.0;
        } else if (sign(previous_quantity) != sign(position.net_quantity)) {
            position.average_entry_price = fill.price;
        }
    }

    if (const auto mark_iterator = mark_prices_.find(fill.instrument.instrument_id); mark_iterator != mark_prices_.end()) {
        update_unrealized(position, mark_iterator->second);
    } else {
        update_unrealized(position, fill.price);
    }
}

// Applies one fill to base and quote balances.
void PortfolioService::apply_fill_to_balances(const trading::core::FillEvent& fill) {
    auto& base_balance = balances_[fill.instrument.base_asset];
    base_balance.asset = fill.instrument.base_asset;

    auto& quote_balance = balances_[fill.instrument.quote_asset];
    quote_balance.asset = fill.instrument.quote_asset;

    const auto notional = fill.quantity * fill.price;
    if (fill.side == trading::core::OrderSide::buy) {
        base_balance.total_balance += fill.quantity;
        quote_balance.total_balance -= (notional + fill.fee);
    } else {
        base_balance.total_balance -= fill.quantity;
        quote_balance.total_balance += (notional - fill.fee);
    }

    base_balance.available_balance = base_balance.total_balance;
    quote_balance.available_balance = quote_balance.total_balance;
}

}  // namespace trading::portfolio
