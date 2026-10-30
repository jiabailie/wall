#include "test_support.hpp"

using namespace wall_test;

// Verifies recovery rebuilds runtime state, warms cache, and derives Kafka checkpoints.
TEST(Recovery, recovery_service_restores_runtime_and_cache) {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx_1 = make_transaction("tx-1", 10);
    auto tx_2 = make_transaction("tx-2", 12);
    log_test_step("seed repositories with processed transactions and open order state");
    transaction_repository.save_received(tx_1);
    transaction_repository.save_processed("tx-1", "processed");
    transaction_repository.save_received(tx_2);
    transaction_repository.save_processed("tx-2", "processed");

    order_repository.save_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        "order-open-1",
        "client-1");
    order_repository.save_order_update({
        .order_id = "order-open-1",
        .client_order_id = "client-1",
        .status = trading::core::OrderStatus::partially_filled,
        .filled_quantity = 0.2,
        .reason = std::nullopt,
    });

    position_repository.save_position({
        .position_id = "position-binance:BTCUSDT",
        .instrument = make_btc_instrument(),
        .net_quantity = 0.2,
        .average_entry_price = 42000.0,
        .realized_pnl = 15.0,
        .unrealized_pnl = 0.0,
    });
    balance_repository.save_balance({
        .asset = "USDT",
        .total_balance = 5000.0,
        .available_balance = 4900.0,
    });

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);

    log_test_step("run recovery and derive runtime snapshot");
    const auto snapshot = recovery_service.recover({
        {
            .instrument_id = "binance:BTCUSDT",
            .best_bid = 41995.0,
            .best_ask = 42005.0,
            .last_trade_price = 42000.0,
            .last_trade_quantity = 0.25,
            .last_process_timestamp = 5000,
        },
    });

    expect_equal(snapshot.open_orders.size(), std::size_t {1}, "one open order should be recovered");
    expect_equal(snapshot.positions.size(), std::size_t {1}, "one position should be recovered");
    expect_equal(snapshot.balances.size(), std::size_t {1}, "one balance should be recovered");
    expect_equal(snapshot.kafka_checkpoints.size(), std::size_t {1}, "one partition checkpoint should be derived");
    expect_equal(snapshot.kafka_checkpoints[0].offset, std::int64_t {12}, "latest processed offset should be recovered");
    expect_equal(transaction_cache.get_status("tx-2"), std::string("processed"), "transaction cache should be warmed");

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
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("restore recovered snapshot into simulation runtime");
    recovery_service.restore_runtime(snapshot, runtime);

    log_test_step("verify restored portfolio and warmed market cache");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    const auto balance = runtime.portfolio().get_balance("USDT");
    expect_true(position.has_value(), "recovered runtime should restore position");
    expect_equal(position->net_quantity, 0.2, "recovered position quantity should match");
    expect_equal(balance.total_balance, 5000.0, "recovered balance should match");

    const auto warmed_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(warmed_snapshot.has_value(), "market cache should be warmed from exchange snapshot");
    expect_equal(*warmed_snapshot->best_bid, 41995.0, "warmed best bid should match exchange snapshot");
}

// Verifies explicit Kafka checkpoints override inferred transaction offsets during recovery.
TEST(Recovery, recovery_service_prefers_explicit_checkpoints) {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx = make_transaction("tx-1", 77);
    tx.kafka_partition = 2;
    log_test_step("seed processed transaction and explicit Kafka checkpoint");
    transaction_repository.save_received(tx);
    transaction_repository.save_processed("tx-1", "processed");
    transaction_repository.save_checkpoint({
        .topic = tx.kafka_topic,
        .partition = tx.kafka_partition,
        .offset = 55,
    });

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);
    log_test_step("recover snapshot and verify explicit checkpoint precedence");
    const auto snapshot = recovery_service.recover();

    expect_equal(snapshot.kafka_checkpoints.size(), std::size_t {1}, "one explicit checkpoint should be recovered");
    expect_equal(snapshot.kafka_checkpoints[0].offset, std::int64_t {55}, "explicit checkpoint should override derived offset");
}

// Verifies Kafka recovery resumes from the next intended offset.
TEST(Recovery, recovery_service_resumes_kafka_from_next_offset) {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx = make_transaction("tx-1", 77);
    tx.kafka_partition = 2;
    log_test_step("seed processed transaction for Kafka resume");
    transaction_repository.save_received(tx);
    transaction_repository.save_processed("tx-1", "processed");

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);
    log_test_step("recover snapshot and resume Kafka consumer");
    const auto snapshot = recovery_service.recover();

    FakeKafkaConsumerClient kafka_client({});
    recovery_service.resume_kafka(snapshot, kafka_client);

    log_test_step("verify recovered Kafka seek offset");
    expect_equal(kafka_client.seeks().size(), std::size_t {1}, "one kafka partition should be resumed");
    expect_equal(kafka_client.seeks()[0].partition, 2, "recovery should resume the recovered partition");
    expect_equal(kafka_client.seeks()[0].offset, std::int64_t {78}, "recovery should seek to the next offset");
}

// Verifies runtime persists local open-order state, fills, portfolio, and market snapshots for recovery.
TEST(Recovery, simulation_runtime_persists_local_state_for_recovery) {
    trading::core::FixedClock clock(5000);
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryFillRepository fill_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;

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
                .instrument = make_btc_instrument(),
                .trigger_price = 42000.0,
                .order_quantity = 0.4,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
            .order_repository = &order_repository,
            .fill_repository = &fill_repository,
            .position_repository = &position_repository,
            .balance_repository = &balance_repository,
            .market_state_cache = &market_cache,
        });

    log_test_step("process trade event that creates a resting local buy order");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "trade-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify resting open order and cached market snapshot were persisted");
    const auto open_orders = order_repository.all_open_orders();
    expect_equal(open_orders.size(), std::size_t {1}, "one resting local order should persist as open");
    expect_equal(open_orders[0].status, trading::core::OrderStatus::acknowledged, "resting local order should persist as acknowledged");
    const auto cached_trade_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(cached_trade_snapshot.has_value(), "market snapshot should be persisted to cache");
    expect_true(cached_trade_snapshot->last_trade_price.has_value(), "persisted market snapshot should retain last trade price");
    expect_equal(*cached_trade_snapshot->last_trade_price, 41990.0, "persisted trade price should match event");

    log_test_step("process book snapshot that fills the persisted resting order");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
        },
        .process_timestamp = 4901,
    });

    log_test_step("verify filled order, persisted fill, portfolio, and book snapshot state");
    const auto filled_order = order_repository.get_order(open_orders[0].order_id);
    expect_true(filled_order.has_value(), "filled local order should remain persisted");
    expect_equal(filled_order->status, trading::core::OrderStatus::filled, "filled local order should persist terminal status");
    expect_equal(fill_repository.all_fills().size(), std::size_t {1}, "one market-driven fill should be persisted");
    const auto persisted_position = position_repository.get_position("binance:BTCUSDT");
    expect_true(persisted_position.has_value(), "position should be persisted after fill");
    expect_near(persisted_position->net_quantity, 0.4, 1e-9, "persisted position quantity should match fill");
    const auto persisted_balance = balance_repository.get_balance("BTC");
    expect_true(persisted_balance.has_value(), "base-asset balance should be persisted after fill");
    expect_near(persisted_balance->total_balance, 0.4, 1e-9, "persisted base balance should reflect fill quantity");
    const auto cached_book_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(cached_book_snapshot.has_value(), "book snapshot should persist to market cache");
    expect_true(cached_book_snapshot->best_ask.has_value(), "persisted book snapshot should retain best ask");
    expect_equal(*cached_book_snapshot->best_ask, 41990.0, "persisted best ask should match book snapshot");
}

// Verifies recovery restores persisted local open orders and matching can continue after restart.
TEST(Recovery, recovery_service_restores_local_open_order_continuity) {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    {
        trading::core::FixedClock first_clock(5000);
        trading::app::SimulationRuntime first_runtime(
            first_clock,
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
                    .instrument = make_btc_instrument(),
                    .trigger_price = 42000.0,
                    .order_quantity = 0.4,
                    .side = trading::core::OrderSide::buy,
                },
                .execution = {
                    .partial_fill_threshold = 1.0,
                    .partial_fill_ratio = 0.5,
                },
                .auto_complete_partial_fills = false,
                .order_repository = &order_repository,
                .fill_repository = nullptr,
                .position_repository = &position_repository,
                .balance_repository = &balance_repository,
                .market_state_cache = &market_cache,
            });

        log_test_step("run first runtime until local order is persisted as resting open state");
        first_runtime.on_event(trading::core::MarketEvent {
            .event_id = "trade-1",
            .type = trading::core::MarketEventType::trade,
            .instrument = make_btc_instrument(),
            .price = 41990.0,
            .quantity = 0.5,
            .process_timestamp = 4900,
        });
    }

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);

    log_test_step("recover persisted open-order snapshot after restart");
    const auto snapshot = recovery_service.recover();
    expect_equal(snapshot.open_orders.size(), std::size_t {1}, "one local open order should be recoverable after restart");

    trading::core::FixedClock second_clock(6000);
    trading::storage::InMemoryFillRepository fill_repository;
    trading::app::SimulationRuntime recovered_runtime(
        second_clock,
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
                    .instrument = make_btc_instrument(),
                    .trigger_price = 41000.0,
                    .order_quantity = 0.4,
                    .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
            .order_repository = &order_repository,
            .fill_repository = &fill_repository,
            .position_repository = &position_repository,
            .balance_repository = &balance_repository,
            .market_state_cache = &market_cache,
        });

    log_test_step("restore recovered snapshot into new runtime");
    recovery_service.restore_runtime(snapshot, recovered_runtime);

    log_test_step("continue matching after restart with external executable ask liquidity");
    recovered_runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
        },
        .process_timestamp = 5900,
    });

    log_test_step("verify recovered local open order continues to filled state after restart");
    const auto filled_orders = order_repository.all_open_orders();
    expect_equal(filled_orders.size(), std::size_t {0}, "filled recovered order should no longer appear as open");
    expect_equal(fill_repository.all_fills().size(), std::size_t {1}, "continued matching after restart should persist fill");
    const auto position = recovered_runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "recovered runtime should apply continued fill into portfolio");
    expect_near(position->net_quantity, 0.4, 1e-9, "continued post-restart fill should restore intended position");
}

// Verifies replay reproduces the same state as direct fixed-session processing.
TEST(Recovery, replay_service_reproduces_fixed_session_state) {
    const auto temp_dir = make_temp_directory("replay-fixed-session");
    const auto log_path = temp_dir / "events.tsv";

    const std::vector<trading::core::EngineEvent> events {
        trading::core::MarketEvent {
            .event_id = "event-1",
            .type = trading::core::MarketEventType::book_snapshot,
            .instrument = make_btc_instrument(),
            .bid_levels = {
                {.price = 41990.0, .quantity = 0.05},
            },
            .process_timestamp = 4900,
        },
        trading::core::TimerEvent {
            .timer_id = "timer-1",
            .timestamp = 5000,
        },
    };

    trading::storage::ReplayService replay_service(log_path);
    log_test_step("write fixed event log");
    replay_service.reset_log();
    for (const auto& event : events) {
        replay_service.append_event(event);
    }

    trading::core::FixedClock direct_clock(5000);
    trading::app::SimulationRuntime direct_runtime(
        direct_clock,
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
        });
    log_test_step("seed direct runtime with matching resting liquidity");
    direct_runtime.restore_open_order({
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
    for (const auto& event : events) {
        direct_runtime.on_event(event);
    }

    trading::core::FixedClock replay_clock(5000);
    trading::app::SimulationRuntime replay_runtime(
        replay_clock,
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
        });
    log_test_step("seed replay runtime with matching resting liquidity");
    replay_runtime.restore_open_order({
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

    log_test_step("replay persisted events into runtime");
    const auto stats = replay_service.replay([&replay_runtime](const trading::core::EngineEvent& event) {
        replay_runtime.on_event(event);
    });

    log_test_step("compare replay results to direct processing");
    expect_equal(stats.replayed_records, std::size_t {2}, "replay should apply both records");
    expect_equal(replay_runtime.risk_approved_count(), direct_runtime.risk_approved_count(), "replay should match approved count");
    expect_equal(replay_runtime.applied_fill_count(), direct_runtime.applied_fill_count(), "replay should match fill count");
    expect_equal(replay_runtime.applied_fill_count(), std::size_t {1}, "book-driven replay should apply one fill");

    const auto direct_position = direct_runtime.portfolio().get_position("binance:BTCUSDT");
    const auto replay_position = replay_runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(direct_position.has_value() && replay_position.has_value(), "both runs should produce a position");
    expect_equal(replay_position->net_quantity, direct_position->net_quantity, "replayed position should match direct run");

    std::filesystem::remove_all(temp_dir);
}

// Verifies invalid persisted records are skipped without aborting replay.
TEST(Recovery, replay_service_skips_invalid_records) {
    const auto temp_dir = make_temp_directory("replay-invalid-records");
    const auto log_path = temp_dir / "events.tsv";

    trading::storage::ReplayService replay_service(log_path);
    replay_service.reset_log();
    replay_service.append_event(trading::core::TimerEvent {
        .timer_id = "timer-1",
        .timestamp = 1000,
    });
    {
        std::ofstream output(log_path, std::ios::app);
        output << "invalid\tbroken\trecord\n";
    }

    std::size_t replayed = 0;
    const auto stats = replay_service.replay([&replayed](const trading::core::EngineEvent&) {
        ++replayed;
    });

    expect_equal(stats.total_records, std::size_t {2}, "two persisted records should be scanned");
    expect_equal(stats.replayed_records, std::size_t {1}, "one valid record should be replayed");
    expect_equal(stats.skipped_records, std::size_t {1}, "one invalid record should be skipped");
    expect_equal(replayed, std::size_t {1}, "callback should run only for valid records");

    std::filesystem::remove_all(temp_dir);
}

// Verifies replay outcomes are deterministic across different wall-clock values.
TEST(Recovery, replay_service_isolated_from_live_clock) {
    const auto temp_dir = make_temp_directory("replay-clock-isolation");
    const auto log_path = temp_dir / "events.tsv";

    trading::storage::ReplayService replay_service(log_path);
    replay_service.reset_log();
    replay_service.append_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 9999990,
    });

    const auto risk_config = trading::config::RiskConfig {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    const auto runtime_config = trading::app::SimulationRuntimeConfig {
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
    };

    trading::core::FixedClock clock_a(0);
    trading::app::SimulationRuntime runtime_a(clock_a, risk_config, runtime_config);
    runtime_a.restore_open_order({
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
    const auto stats_a = replay_service.replay([&runtime_a](const trading::core::EngineEvent& event) {
        runtime_a.on_event(event);
    });

    trading::core::FixedClock clock_b(9999999);
    trading::app::SimulationRuntime runtime_b(clock_b, risk_config, runtime_config);
    runtime_b.restore_open_order({
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
    const auto stats_b = replay_service.replay([&runtime_b](const trading::core::EngineEvent& event) {
        runtime_b.on_event(event);
    });
    expect_equal(stats_a.replayed_records, stats_b.replayed_records, "both replays should process the same record count");

    expect_equal(runtime_a.risk_approved_count(), runtime_b.risk_approved_count(), "replay counts should match across clocks");
    expect_equal(runtime_a.applied_fill_count(), runtime_b.applied_fill_count(), "fill counts should match across clocks");

    const auto position_a = runtime_a.portfolio().get_position("binance:BTCUSDT");
    const auto position_b = runtime_b.portfolio().get_position("binance:BTCUSDT");
    expect_true(position_a.has_value() && position_b.has_value(), "both replays should produce positions");
    expect_equal(position_a->net_quantity, position_b->net_quantity, "position result should match across clocks");

    std::filesystem::remove_all(temp_dir);
}
