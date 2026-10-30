#include "test_support.hpp"

using namespace wall_test;

// Verifies valid orders pass baseline risk checks.
TEST(Risk, risk_engine_approves_valid_order) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(decision.approved, "valid order should be approved");
}

// Verifies oversize orders are rejected by the configured quantity limit.
TEST(Risk, risk_engine_rejects_oversize_order) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 0.25,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "oversize order should be rejected");
    expect_equal(*decision.reason, std::string("order quantity exceeds limit"), "oversize rejection reason should match");
}

// Verifies stale market data blocks submissions.
TEST(Risk, risk_engine_rejects_stale_market) {
    trading::core::FixedClock clock(8000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "stale-market order should be rejected");
    expect_equal(*decision.reason, std::string("market data is stale"), "stale-market rejection reason should match");
}

// Verifies the kill switch blocks all submissions.
TEST(Risk, risk_engine_kill_switch_blocks_orders) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = true,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "kill switch should reject all orders");
    expect_equal(*decision.reason, std::string("kill switch enabled"), "kill-switch rejection reason should match");
}
