#pragma once

#include "core/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::portfolio {

// Tracks in-memory positions, balances, and PnL from execution fills.
class PortfolioService {
public:
    // Applies one fill and updates position and balance state.
    void apply_fill(const trading::core::FillEvent& fill);

    // Returns the latest position for the instrument, if any.
    [[nodiscard]] std::optional<trading::core::Position> get_position(const std::string& instrument_id) const;

    // Returns the latest balance snapshot for the asset, or an empty snapshot.
    [[nodiscard]] trading::core::BalanceSnapshot get_balance(const std::string& asset) const;

    // Updates the mark price used for unrealized PnL calculations.
    void set_mark_price(const std::string& instrument_id, double price);

    // Restores one recovered position snapshot into the in-memory portfolio state.
    void restore_position(const trading::core::Position& position);

    // Restores one recovered balance snapshot into the in-memory portfolio state.
    void restore_balance(const trading::core::BalanceSnapshot& balance);

    // Returns all known positions for persistence and recovery workflows.
    [[nodiscard]] std::vector<trading::core::Position> all_positions() const;

    // Returns all known balances for persistence and recovery workflows.
    [[nodiscard]] std::vector<trading::core::BalanceSnapshot> all_balances() const;

private:
    [[nodiscard]] static double sign(double value);
    static void update_unrealized(trading::core::Position& position, double mark_price);
    void apply_fill_to_position(trading::core::Position& position, const trading::core::FillEvent& fill);
    void apply_fill_to_balances(const trading::core::FillEvent& fill);

    std::unordered_map<std::string, trading::core::Position> positions_;
    std::unordered_map<std::string, trading::core::BalanceSnapshot> balances_;
    std::unordered_map<std::string, double> mark_prices_;
};

}  // namespace trading::portfolio
