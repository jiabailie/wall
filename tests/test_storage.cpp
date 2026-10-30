#include "test_support.hpp"

using namespace wall_test;

// Verifies order repository save and lifecycle update behavior.
TEST(Storage, in_memory_order_repository_save_and_update) {
    trading::storage::InMemoryOrderRepository repository;
    const auto instrument = make_btc_instrument();

    log_test_step("save initial order and apply lifecycle update");
    repository.save_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        "order-1",
        "client-1");
    repository.save_order_update({
        .order_id = "order-1",
        .client_order_id = "client-1",
        .status = trading::core::OrderStatus::acknowledged,
        .filled_quantity = 0.0,
        .reason = std::nullopt,
    });

    log_test_step("reload saved order and verify updated state");
    const auto order = repository.get_order("order-1");
    expect_true(order.has_value(), "order should exist after save");
    expect_equal(order->strategy_id, std::string("strategy-1"), "strategy id should persist");
    expect_equal(order->status, trading::core::OrderStatus::acknowledged, "order status should update");
}

// Verifies position repository stores and replaces latest position state.
TEST(Storage, in_memory_position_repository_update) {
    trading::storage::InMemoryPositionRepository repository;
    const auto instrument = make_btc_instrument();

    repository.save_position({
        .position_id = "position-1",
        .instrument = instrument,
        .net_quantity = 1.0,
        .average_entry_price = 42000.0,
        .realized_pnl = 0.0,
        .unrealized_pnl = 0.0,
    });
    repository.save_position({
        .position_id = "position-1",
        .instrument = instrument,
        .net_quantity = 0.5,
        .average_entry_price = 42000.0,
        .realized_pnl = 100.0,
        .unrealized_pnl = 50.0,
    });

    const auto position = repository.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.5, "position repository should store latest net quantity");
    expect_equal(position->realized_pnl, 100.0, "position repository should store latest realized pnl");
}

// Verifies market snapshot cache write and read behavior.
TEST(Storage, in_memory_market_state_cache_write_read) {
    trading::storage::InMemoryMarketStateCache cache;
    log_test_step("write market snapshot into in-memory cache");
    cache.upsert_snapshot({
        .instrument_id = "binance:BTCUSDT",
        .best_bid = 42000.0,
        .best_ask = 42010.0,
        .last_trade_price = 42005.0,
        .last_trade_quantity = 0.25,
        .last_process_timestamp = 5000,
    });

    log_test_step("read market snapshot back from cache");
    const auto snapshot = cache.get_snapshot("binance:BTCUSDT");
    expect_true(snapshot.has_value(), "snapshot should exist");
    expect_true(snapshot->best_bid.has_value(), "best bid should be present");
    expect_equal(*snapshot->best_bid, 42000.0, "best bid should persist");
    expect_equal(snapshot->last_process_timestamp, std::int64_t {5000}, "process timestamp should persist");
}

// Verifies the Postgres order adapter persists and reads back order updates.
TEST(Storage, postgres_order_repository_write_read) {
    const auto temp_dir = make_temp_directory("postgres-order-repo");
    {
        trading::infrastructure::PostgresOrderRepository repository(temp_dir);
        const auto instrument = make_btc_instrument();
        log_test_step("persist order and lifecycle update through Postgres-style repository");
        repository.save_order(
            {
                .request_id = "req-1",
                .strategy_id = "strategy-1",
                .instrument = instrument,
                .side = trading::core::OrderSide::buy,
                .type = trading::core::OrderType::limit,
                .quantity = 0.25,
                .price = 42000.0,
            },
            "order-1",
            "client-1");
        repository.save_order_update({
            .order_id = "order-1",
            .client_order_id = "client-1",
            .status = trading::core::OrderStatus::acknowledged,
            .filled_quantity = 0.0,
            .reason = std::nullopt,
        });
    }
    {
        trading::infrastructure::PostgresOrderRepository repository(temp_dir);
        log_test_step("reload persisted order record from repository");
        const auto order = repository.get_order("order-1");
        expect_true(order.has_value(), "postgres order adapter should persist records");
        expect_equal(order->status, trading::core::OrderStatus::acknowledged, "persisted order status should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies Redis-style cache adapters read/write transaction and market state values.
TEST(Storage, redis_cache_adapters_write_read) {
    trading::infrastructure::RedisTransactionCache transaction_cache;
    log_test_step("write transaction status to Redis-style transaction cache");
    transaction_cache.set_status("tx-1", "processed");
    expect_equal(transaction_cache.get_status("tx-1"), std::string("processed"), "redis transaction cache should store statuses");

    trading::infrastructure::RedisMarketStateCache market_cache;
    log_test_step("write and read market snapshot through Redis-style market cache");
    market_cache.upsert_snapshot({
        .instrument_id = "binance:BTCUSDT",
        .best_bid = 42000.0,
        .best_ask = 42010.0,
        .last_trade_price = 42005.0,
        .last_trade_quantity = 0.3,
        .last_process_timestamp = 9000,
    });
    const auto snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(snapshot.has_value(), "redis market cache should store snapshots");
    expect_equal(*snapshot->best_ask, 42010.0, "redis market cache should return stored best ask");
}

// Verifies transaction checkpoints persist across repository restarts.
TEST(Storage, postgres_transaction_checkpoint_persistence) {
    const auto temp_dir = make_temp_directory("postgres-transaction-repo");
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        const auto command = make_transaction("tx-1", 77);
        log_test_step("persist received and processed transaction checkpoint");
        repository.save_received(command);
        repository.save_processed("tx-1", "processed");
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("reload transaction checkpoint from repository");
        const auto record = repository.get_record("tx-1");
        expect_true(record.has_value(), "transaction checkpoint should persist");
        expect_equal(record->status, std::string("processed"), "persisted checkpoint status should reload");
        expect_equal(record->kafka_offset, std::int64_t {77}, "persisted checkpoint offset should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies explicit Kafka checkpoints persist across repository restarts.
TEST(Storage, postgres_explicit_kafka_checkpoint_persistence) {
    const auto temp_dir = make_temp_directory("postgres-explicit-checkpoint-repo");
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("persist explicit Kafka checkpoint");
        repository.save_checkpoint({
            .topic = "trading-transactions",
            .partition = 2,
            .offset = 144,
        });
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("reload explicit Kafka checkpoint");
        const auto checkpoints = repository.all_checkpoints();
        expect_equal(checkpoints.size(), std::size_t {1}, "explicit checkpoint should persist");
        expect_equal(checkpoints[0].partition, 2, "persisted checkpoint partition should reload");
        expect_equal(checkpoints[0].offset, std::int64_t {144}, "persisted explicit checkpoint offset should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies balance snapshots persist across repository restarts.
TEST(Storage, postgres_balance_repository_write_read) {
    const auto temp_dir = make_temp_directory("postgres-balance-repo");
    {
        trading::infrastructure::PostgresBalanceRepository repository(temp_dir);
        log_test_step("persist balance snapshot");
        repository.save_balance({
            .asset = "USDT",
            .total_balance = 10500.5,
            .available_balance = 10400.25,
        });
    }
    {
        trading::infrastructure::PostgresBalanceRepository repository(temp_dir);
        log_test_step("reload balance snapshot");
        const auto balance = repository.get_balance("USDT");
        expect_true(balance.has_value(), "balance snapshot should persist");
        expect_equal(balance->total_balance, 10500.5, "persisted balance total should reload");
        expect_equal(balance->available_balance, 10400.25, "persisted balance available should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies order intents persist across repository restarts.
TEST(Storage, postgres_order_intent_repository_write_read) {
    const auto temp_dir = make_temp_directory("postgres-order-intent-repo");
    {
        trading::infrastructure::PostgresOrderIntentRepository repository(temp_dir);
        log_test_step("persist outbound order intent");
        repository.append_intent({
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument_id = "binance:BTCUSDT",
            .side = trading::core::OrderSide::buy,
            .order_type = trading::core::OrderType::limit,
            .quantity = 0.25,
            .price = 42010.0,
        });
    }
    {
        trading::infrastructure::PostgresOrderIntentRepository repository(temp_dir);
        log_test_step("reload persisted order intent");
        const auto intents = repository.all_intents();
        expect_equal(intents.size(), std::size_t {1}, "order intent should persist");
        expect_equal(intents[0].request_id, std::string("req-1"), "persisted order intent id should reload");
        expect_true(intents[0].price.has_value(), "persisted order intent price should reload");
        expect_equal(*intents[0].price, 42010.0, "persisted order intent price should match");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies execution reports persist across repository restarts.
TEST(Storage, postgres_execution_report_repository_write_read) {
    const auto temp_dir = make_temp_directory("postgres-execution-report-repo");
    {
        trading::infrastructure::PostgresExecutionReportRepository repository(temp_dir);
        log_test_step("persist execution report");
        repository.append_report({
            .report_id = "report-1",
            .order_id = "order-1",
            .client_order_id = "client-1",
            .instrument_id = "binance:BTCUSDT",
            .side = trading::core::OrderSide::buy,
            .status = trading::core::OrderStatus::partially_filled,
            .cumulative_filled_quantity = 0.4,
            .last_fill_price = 42020.0,
            .last_fill_fee = 0.12,
            .reason = std::string("partial_fill"),
            .exchange_timestamp = 9001,
        });
    }
    {
        trading::infrastructure::PostgresExecutionReportRepository repository(temp_dir);
        log_test_step("reload persisted execution report");
        const auto reports = repository.all_reports();
        expect_equal(reports.size(), std::size_t {1}, "execution report should persist");
        expect_equal(reports[0].report_id, std::string("report-1"), "persisted execution report id should reload");
        expect_equal(reports[0].cumulative_filled_quantity, 0.4, "persisted execution quantity should reload");
        expect_true(reports[0].reason.has_value(), "persisted execution reason should reload");
        expect_equal(*reports[0].reason, std::string("partial_fill"), "persisted execution reason should match");
    }
    std::filesystem::remove_all(temp_dir);
}
