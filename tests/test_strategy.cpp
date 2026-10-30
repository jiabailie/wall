#include "test_support.hpp"

using namespace wall_test;

// Verifies the sample strategy emits a deterministic order when the threshold is crossed.
TEST(Strategy, sample_threshold_strategy_emits_order_on_trigger) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);

    expect_equal(requests.size(), std::size_t {1}, "strategy should emit one order request");
    expect_equal(requests[0].strategy_id, std::string("threshold-1"), "strategy id should propagate");
    expect_equal(requests[0].quantity, 0.25, "configured order quantity should propagate");
    expect_true(requests[0].price.has_value(), "emitted order should include a limit price");
    expect_equal(*requests[0].price, 41990.0, "emitted price should match the triggering trade");
}

// Verifies the sample strategy stays idle when the trigger condition is not met.
TEST(Strategy, sample_threshold_strategy_no_order_without_trigger) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);

    expect_true(requests.empty(), "strategy should not emit when trigger condition is not met");
}

// Verifies emitted orders inherit full market-event instrument metadata when config is sparse.
TEST(Strategy, sample_threshold_strategy_propagates_full_instrument_metadata) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .instrument = {
            .instrument_id = instrument.instrument_id,
        },
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    log_test_step("emit threshold-strategy order from sparse configured instrument");
    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.25,
        .process_timestamp = 4901,
    }, context);

    log_test_step("verify emitted order carries full instrument metadata");
    expect_equal(requests.size(), std::size_t {1}, "strategy should emit one order");
    expect_equal(requests[0].instrument.exchange, std::string("binance"), "emitted order should carry exchange metadata");
    expect_equal(requests[0].instrument.symbol, std::string("BTCUSDT"), "emitted order should carry symbol metadata");
    expect_equal(requests[0].instrument.base_asset, std::string("BTC"), "emitted order should carry base-asset metadata");
    expect_equal(requests[0].instrument.quote_asset, std::string("USDT"), "emitted order should carry quote-asset metadata");
}

// Verifies a transaction command can reset the sample strategy after a prior signal.
TEST(Strategy, sample_threshold_strategy_reset_command) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto first_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);
    const auto second_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41980.0,
        .quantity = 0.5,
        .process_timestamp = 4950,
    }, context);

    auto reset_command = make_transaction("tx-reset", 30);
    reset_command.command_type = "reset_strategy";
    const auto reset_requests = strategy.on_event(reset_command, context);
    const auto third_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-3",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41970.0,
        .quantity = 0.5,
        .process_timestamp = 4990,
    }, context);

    expect_equal(first_requests.size(), std::size_t {1}, "first trigger should emit");
    expect_true(second_requests.empty(), "strategy should suppress repeated signals before reset");
    expect_true(reset_requests.empty(), "reset command should not emit an order");
    expect_equal(third_requests.size(), std::size_t {1}, "strategy should emit again after reset");
}

// Verifies one failing strategy is paused without blocking healthy strategies in the same runtime.
TEST(Strategy, strategy_coordinator_isolates_failing_strategy) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::StrategyCoordinator coordinator;
    coordinator.add_strategy(std::make_unique<trading::strategy::SampleThresholdStrategy>(
        trading::strategy::SampleThresholdStrategyConfig {
            .strategy_id = "threshold-1",
            .instrument_id = instrument.instrument_id,
            .instrument = instrument,
            .trigger_price = 42000.0,
            .order_quantity = 0.25,
            .side = trading::core::OrderSide::buy,
        }));
    coordinator.add_strategy(std::make_unique<ThrowingStrategy>("failing-1"));

    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };
    const trading::core::MarketEvent event {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4950,
    };

    const auto requests = coordinator.on_event(event, context);

    expect_equal(requests.size(), std::size_t {1}, "healthy strategy should still emit when another strategy fails");
    expect_equal(coordinator.strategy_count(), std::size_t {2}, "both strategies should remain registered");
    expect_equal(coordinator.active_strategy_count(), std::size_t {1}, "failing strategy should be paused");
    expect_true(coordinator.is_paused("failing-1"), "failing strategy should be marked paused");

    const auto stats = coordinator.all_stats();
    expect_equal(stats.size(), std::size_t {2}, "stats should be present for both strategies");
    expect_true(stats[1].failures == 1 || stats[0].failures == 1, "one strategy should record a failure");
}

// Verifies the spread-capture strategy emits one bid-side order once the spread is wide enough, then stays quiet until reset.
TEST(Strategy, spread_capture_strategy_emits_once_when_spread_is_wide_enough) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    trading::strategy::SpreadCaptureStrategy strategy({
        .strategy_id = "spread-1",
        .instrument = instrument,
        .min_spread = 10.0,
        .order_quantity = 0.1,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto make_quote = [&instrument](const std::string& event_id, const double bid, const double ask) {
        return trading::core::MarketEvent {
            .event_id = event_id,
            .type = trading::core::MarketEventType::ticker,
            .instrument = instrument,
            .bid_price = bid,
            .ask_price = ask,
            .process_timestamp = 4900,
        };
    };

    // Step 1: A narrow spread must not trigger an order.
    const auto narrow = make_quote("quote-1", 42000.0, 42005.0);
    store.apply(narrow);
    expect_true(strategy.on_event(narrow, context).empty(), "narrow spread should not emit an order");

    // Step 2: A wide spread emits one order at the best bid.
    const auto wide = make_quote("quote-2", 42000.0, 42020.0);
    store.apply(wide);
    const auto requests = strategy.on_event(wide, context);
    expect_equal(requests.size(), std::size_t {1}, "wide spread should emit one order");
    expect_equal(requests[0].strategy_id, std::string("spread-1"), "strategy id should propagate");
    expect_equal(*requests[0].price, 42000.0, "buy order should rest at the best bid");

    // Step 3: The strategy stays quiet after emitting.
    expect_true(strategy.on_event(wide, context).empty(), "strategy should emit only once");
}
