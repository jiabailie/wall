#include "test_support.hpp"

using namespace wall_test;

// Verifies runtime applies market-driven fills from external book snapshots into portfolio state.
TEST(Simulation, simulation_runtime_market_event_applies_market_driven_fill) {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 41000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });

    log_test_step("restore resting local buy order for market-driven fill");
    runtime.restore_open_order({
        .order_id = "resting-buy-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::buy,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.4,
        .price = 42000.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process external book snapshot through simulation runtime");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41995.0, .quantity = 0.4},
        },
        .process_timestamp = 4900,
    });

    log_test_step("verify market-driven fill is applied into portfolio");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "market-driven book snapshot should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "market-driven fill should open a position");
    expect_near(position->net_quantity, 0.4, 1e-9, "position quantity should match market-driven fill quantity");
    expect_near(position->average_entry_price, 41995.0, 1e-9, "position entry price should use external ask level price");
}

// Verifies a full in-process simulation run reaches portfolio updates on the happy path.
TEST(Simulation, simulation_runtime_happy_path) {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.10,
                .partial_fill_ratio = 0.50,
            },
            .auto_complete_partial_fills = false,
        });

    log_test_step("seed restored resting sell order for matching");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process trigger trade event through runtime");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify approved request and applied fill");
    expect_equal(runtime.risk_approved_count(), std::size_t {1}, "happy path should approve one request");
    expect_equal(runtime.risk_rejected_count(), std::size_t {0}, "happy path should not reject");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "happy path should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "position should exist after happy path fill");
    expect_equal(position->net_quantity, 0.05, "position size should match strategy request quantity");
}

// Verifies risk rejection stops the pipeline before execution and portfolio updates.
TEST(Simulation, simulation_runtime_risk_rejection_stops_execution) {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 100.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.10,
                .partial_fill_ratio = 0.50,
            },
            .auto_complete_partial_fills = false,
        });

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_equal(runtime.risk_approved_count(), std::size_t {0}, "risk-reject path should not approve");
    expect_equal(runtime.risk_rejected_count(), std::size_t {1}, "risk-reject path should reject one request");
    expect_equal(runtime.applied_fill_count(), std::size_t {0}, "risk-reject path should not apply fills");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(!position.has_value(), "risk-reject path should not open a position");
}

// Verifies partial matching applies the executed quantity and leaves the residual resting.
TEST(Simulation, simulation_runtime_applies_partial_match_fill) {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 2.0,
            .max_order_notional = 100000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 1.0,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.5,
                .partial_fill_ratio = 0.4,
            },
            .auto_complete_partial_fills = true,
        });

    log_test_step("seed limited opposing sell liquidity");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.4,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process strategy-triggering market event");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify only available liquidity is filled");
    expect_equal(runtime.risk_approved_count(), std::size_t {1}, "partial-match path should approve one request");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "partial-match path should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.4, "partial match should apply only the available opposing quantity");
}

// Verifies pause trading blocks strategy-generated orders and records operational metrics.
TEST(Simulation, simulation_runtime_pause_trading_blocks_orders) {
    trading::core::FixedClock clock(5000);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::app::RuntimeOperationalControls controls;
    controls.pause_trading();

    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        },
        &controls,
        &metrics);

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_true(runtime.trading_paused(), "runtime should observe paused trading control");
    expect_equal(runtime.risk_approved_count(), std::size_t {0}, "paused trading should block order approval");
    expect_equal(runtime.applied_fill_count(), std::size_t {0}, "paused trading should block fills");
    expect_equal(metrics.counter("orders_paused"), std::int64_t {1}, "paused order metric should increment");
}

// Verifies runtime metrics capture fills, approvals, and per-event latency.
TEST(Simulation, simulation_runtime_records_metrics) {
    SteppingClock clock(5000, 2);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::app::RuntimeOperationalControls controls;

    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        },
        &controls,
        &metrics);

    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_equal(metrics.counter("market_events"), std::int64_t {1}, "market event metric should increment");
    expect_equal(metrics.counter("orders_approved"), std::int64_t {1}, "approved order metric should increment");
    expect_equal(metrics.counter("fills_applied"), std::int64_t {1}, "fill metric should increment");
    expect_equal(metrics.latency("runtime_event_latency_ms").sample_count, std::int64_t {1}, "runtime latency should be recorded");
}

// Verifies the backtest runner returns replay, strategy, and PnL summary data.
TEST(Simulation, backtest_runner_summarizes_runtime_results) {
    const auto temp_dir = make_temp_directory("backtest-summary");
    const auto log_path = temp_dir / "events.tsv";
    trading::storage::ReplayService replay_service(log_path);
    log_test_step("write historical backtest event log");
    replay_service.reset_log();

    replay_service.append_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    auto coordinator = std::make_shared<trading::strategy::StrategyCoordinator>();
    coordinator->add_strategy(std::make_unique<trading::strategy::SampleThresholdStrategy>(
        trading::strategy::SampleThresholdStrategyConfig {
            .strategy_id = "threshold-1",
            .instrument_id = "binance:BTCUSDT",
            .instrument = make_btc_instrument(),
            .trigger_price = 42000.0,
            .order_quantity = 0.05,
            .side = trading::core::OrderSide::buy,
        }));

    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 10000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .strategy_coordinator = coordinator,
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("seed runtime with resting liquidity for backtest matching");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    trading::app::BacktestRunner runner(log_path);
    log_test_step("run backtest and collect summary");
    const auto summary = runner.run(runtime);

    log_test_step("verify replay, strategy, and fill summary fields");
    expect_equal(summary.replay_stats.replayed_records, std::size_t {1}, "backtest should replay the market event");
    expect_equal(summary.configured_strategies, std::size_t {1}, "configured strategy count should be reported");
    expect_equal(summary.active_strategies, std::size_t {1}, "healthy strategy should remain active");
    expect_equal(summary.paused_strategies, std::size_t {0}, "no strategy should be paused");
    expect_equal(summary.risk_approved, std::size_t {1}, "threshold event should produce one approved request");
    expect_equal(summary.fills_applied, std::size_t {1}, "backtest should apply one fill");
    expect_true(summary.total_unrealized_pnl == 0.0, "new fill at mark price should have zero unrealized pnl");
    expect_equal(summary.strategy_stats.size(), std::size_t {1}, "summary should expose strategy stats");
    expect_equal(summary.strategy_stats[0].emitted_requests, std::size_t {1}, "strategy stats should include emitted request count");

    std::filesystem::remove_all(temp_dir);
}
