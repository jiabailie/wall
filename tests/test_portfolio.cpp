#include "test_support.hpp"

using namespace wall_test;

// Verifies the first buy fill opens a long position and updates balances.
TEST(Portfolio, portfolio_service_opens_position_from_first_fill) {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 42000.0,
        .quantity = 1.0,
        .fee = 10.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist after first fill");
    expect_equal(position->net_quantity, 1.0, "position net quantity should match buy fill");
    expect_equal(position->average_entry_price, 42000.0, "average entry should match first fill price");

    const auto base_balance = portfolio_service.get_balance(instrument.base_asset);
    const auto quote_balance = portfolio_service.get_balance(instrument.quote_asset);
    expect_equal(base_balance.total_balance, 1.0, "base balance should increase on buy");
    expect_equal(quote_balance.total_balance, -42010.0, "quote balance should include notional and fee");
}

// Verifies average entry price updates across additional same-side fills.
TEST(Portfolio, portfolio_service_updates_average_entry_price) {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 42000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 43000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 2.0, "net quantity should include both buys");
    expect_equal(position->average_entry_price, 42500.0, "average entry should be weighted across fills");
}

// Verifies realized PnL is computed when reducing a long position.
TEST(Portfolio, portfolio_service_realized_pnl_on_position_reduction) {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 40000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::sell,
        .price = 41000.0,
        .quantity = 0.4,
        .fee = 0.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.6, "position should be reduced after sell");
    expect_equal(position->realized_pnl, 400.0, "realized pnl should reflect closed quantity");
}

// Verifies unrealized PnL updates when mark prices change.
TEST(Portfolio, portfolio_service_unrealized_pnl_from_mark_price) {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 40000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.set_mark_price(instrument.instrument_id, 40500.0);

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->unrealized_pnl, 500.0, "unrealized pnl should use mark price");
}

// Verifies fees are reflected in quote balances for both buy and sell fills.
TEST(Portfolio, portfolio_service_fee_handling_on_balances) {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 100.0,
        .quantity = 1.0,
        .fee = 2.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::sell,
        .price = 120.0,
        .quantity = 0.5,
        .fee = 1.0,
    });

    const auto base_balance = portfolio_service.get_balance(instrument.base_asset);
    const auto quote_balance = portfolio_service.get_balance(instrument.quote_asset);
    expect_equal(base_balance.total_balance, 0.5, "base balance should reflect buy then partial sell");
    expect_equal(quote_balance.total_balance, -43.0, "quote balance should account for both fees");
}
