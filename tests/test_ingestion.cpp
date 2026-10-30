#include "test_support.hpp"

using namespace wall_test;

// Verifies Kafka-style transaction consumption preserves order and commit checkpoints.
TEST(Ingestion, transaction_ingestor_order_and_commit) {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> processed_ids;
    dispatcher.subscribe([&processed_ids](const trading::core::EngineEvent& event) {
        if (const auto* command = std::get_if<trading::core::TransactionCommand>(&event)) {
            processed_ids.push_back(command->transaction_id);
        }
    });

    trading::ingestion::MockTransactionConsumer consumer({
        make_transaction("tx-1", 10),
        make_transaction("tx-2", 11),
    });
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher);

    log_test_step("process both mock transactions through ingestor");
    expect_true(ingestor.process_next(), "first transaction should be processed");
    expect_true(ingestor.process_next(), "second transaction should be processed");
    expect_true(!ingestor.process_next(), "no third transaction should exist");

    log_test_step("verify publish order, commits, cache, and repository status");
    expect_equal(processed_ids.size(), std::size_t {2}, "two transactions should be published");
    expect_equal(processed_ids[0], std::string("tx-1"), "first transaction order should match");
    expect_equal(processed_ids[1], std::string("tx-2"), "second transaction order should match");
    expect_equal(consumer.committed().size(), std::size_t {2}, "two transactions should be committed");
    expect_equal(consumer.committed()[0].transaction_id, std::string("tx-1"), "first committed transaction should match");
    expect_equal(cache.get_status("tx-2"), std::string("processed"), "processed status should be cached");
    expect_equal(repository.records()[1].status, std::string("processed"), "repository status should be updated");
}

// Verifies malformed transactions are rejected without dispatcher publication.
TEST(Ingestion, transaction_ingestor_rejects_invalid_messages) {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> processed_ids;
    dispatcher.subscribe([&processed_ids](const trading::core::EngineEvent& event) {
        if (const auto* command = std::get_if<trading::core::TransactionCommand>(&event)) {
            processed_ids.push_back(command->transaction_id);
        }
    });

    auto invalid = make_transaction("tx-bad", 20);
    invalid.quantity = 0.0;

    trading::ingestion::MockTransactionConsumer consumer({invalid});
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher);

    log_test_step("process invalid transaction message");
    expect_true(ingestor.process_next(), "invalid transaction should still be handled");

    log_test_step("verify invalid message rejection and commit behavior");
    expect_true(processed_ids.empty(), "invalid transaction should not be published");
    expect_equal(cache.get_status("tx-bad"), std::string("rejected"), "invalid transaction should be rejected");
    expect_equal(repository.records()[0].status, std::string("rejected"), "repository should record rejection");
    expect_equal(consumer.committed().size(), std::size_t {1}, "invalid transaction should still be committed");
}

// Verifies ingestion metrics capture throughput, rejects, and latency.
TEST(Ingestion, transaction_ingestor_records_metrics) {
    trading::core::EventDispatcher dispatcher;
    trading::core::FixedClock clock(1000);
    trading::monitoring::InMemoryMetricsCollector metrics;

    auto invalid = make_transaction("tx-bad", 31);
    invalid.quantity = 0.0;

    trading::ingestion::MockTransactionConsumer consumer({
        make_transaction("tx-good", 30),
        invalid,
    });
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher, &clock, &metrics);

    expect_true(ingestor.process_next(), "first message should be processed");
    expect_true(ingestor.process_next(), "second message should be handled as rejected");

    expect_equal(metrics.counter("transactions_received"), std::int64_t {2}, "received counter should include all handled messages");
    expect_equal(metrics.counter("transactions_processed"), std::int64_t {1}, "processed counter should include valid messages");
    expect_equal(metrics.counter("transactions_rejected"), std::int64_t {1}, "rejected counter should include invalid messages");
    expect_equal(metrics.latency("transaction_processing_latency_ms").sample_count, std::int64_t {2}, "latency samples should be recorded per handled message");
}

// Verifies persistence failures are surfaced and counted during ingestion.
TEST(Ingestion, transaction_ingestor_counts_persistence_failures) {
    trading::core::EventDispatcher dispatcher;
    trading::core::FixedClock clock(1000);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::ingestion::MockTransactionConsumer consumer({make_transaction("tx-1", 40)});
    ThrowingTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher, &clock, &metrics);

    bool threw = false;
    try {
        ingestor.process_next();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    expect_true(threw, "persistence failure should propagate");
    expect_equal(metrics.counter("persistence_failures"), std::int64_t {1}, "persistence failure metric should increment");
}
