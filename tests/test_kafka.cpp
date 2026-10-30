#include "test_support.hpp"

using namespace wall_test;

// Verifies raw Kafka payloads are adapted into normalized transaction commands.
TEST(Kafka, kafka_transaction_consumer_parses_payload) {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 1,
            .offset = 42,
            .payload =
                "transaction_id=tx-1;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=1.25;"
                "price=42000.5",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll and parse valid Kafka payload");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "adapter should parse valid kafka payload");
    expect_equal(command->transaction_id, std::string("tx-1"), "transaction id should match payload");
    expect_equal(command->kafka_partition, 1, "kafka partition should propagate");
    expect_equal(command->kafka_offset, std::int64_t {42}, "kafka offset should propagate");
    expect_true(command->price.has_value(), "price should parse when provided");
}

// Verifies malformed messages are safely skipped and committed as rejected.
TEST(Kafka, kafka_transaction_consumer_skips_malformed_messages) {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 10,
            .payload = "transaction_id=tx-bad;user_id=user-1;quantity=broken",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 11,
            .payload =
                "transaction_id=tx-good;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=0.50",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll malformed then valid Kafka payloads");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "consumer should continue to next message after malformed payload");
    expect_equal(command->transaction_id, std::string("tx-good"), "consumer should return the first valid payload");
    expect_equal(fake_client.commits().size(), std::size_t {1}, "malformed payload should be committed");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {10}, "committed malformed offset should match");
}

// Verifies bursts of malformed Kafka payloads are drained safely before the next valid message.
TEST(Kafka, kafka_transaction_consumer_skips_malformed_payload_burst) {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 20,
            .payload = "broken",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 21,
            .payload = "transaction_id=tx-bad-2;quantity=oops",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 22,
            .payload =
                "transaction_id=tx-good-burst;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=0.75",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("drain malformed burst until the next valid Kafka payload");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "consumer should recover after malformed burst");
    expect_equal(command->transaction_id, std::string("tx-good-burst"), "first valid message after malformed burst should be returned");
    expect_equal(fake_client.commits().size(), std::size_t {2}, "all malformed payloads should be committed");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {20}, "first malformed offset should be committed");
    expect_equal(fake_client.commits()[1].offset, std::int64_t {21}, "second malformed offset should be committed");
}

// Verifies commit checkpoints are controlled by ingestor processing, not by poll.
TEST(Kafka, kafka_transaction_consumer_commit_checkpoint_behavior) {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 77,
            .payload =
                "transaction_id=tx-1;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=1.00",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll valid payload without auto-commit");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "valid payload should be returned");
    expect_equal(fake_client.commits().size(), std::size_t {0}, "poll should not commit valid payload");

    log_test_step("commit payload explicitly through consumer");
    consumer.commit(*command);
    expect_equal(fake_client.commits().size(), std::size_t {1}, "explicit commit should forward offset to client");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {77}, "committed offset should match payload");
}

// Verifies producer payload serialization matches the consumer-side key-value schema.
TEST(Kafka, kafka_transaction_producer_serializes_payload) {
    const auto command = make_transaction("tx-serializer", 0);

    log_test_step("serialize transaction command into Kafka payload format");
    const auto payload = trading::infrastructure::serialize_transaction_command_payload(command);
    expect_true(payload.find("transaction_id=tx-serializer") != std::string::npos, "payload should contain transaction id");
    expect_true(payload.find("user_id=user-1") != std::string::npos, "payload should contain user id");
    expect_true(payload.find("instrument_symbol=BTCUSDT") != std::string::npos, "payload should contain instrument symbol");
    expect_true(payload.find("quantity=1.5") != std::string::npos, "payload should contain quantity");
    expect_true(payload.find("price=42000") != std::string::npos, "payload should contain price");
}

// Verifies producer key serialization uses transaction_id|yyyyMMddHHmmSS.
TEST(Kafka, kafka_transaction_producer_serializes_key) {
    const auto command = make_transaction("tx-serializer", 0);
    log_test_step("serialize transaction key with UTC timestamp suffix");
    const auto key = trading::infrastructure::serialize_transaction_command_key(command, std::int64_t {1713254950000});

    expect_equal(key, std::string("tx-serializer|20240416080910"), "key should concatenate transaction id, separator, and UTC timestamp");
}

// Verifies the transaction publisher can parse one flat JSON input line.
TEST(Kafka, transaction_publisher_parses_json_line) {
    log_test_step("parse valid flat JSON transaction line");
    const auto command = trading::app::parse_json_transaction_command_line(
        R"({"transaction_id":"tx-2001","user_id":"user-9","account_id":"account-9","command_type":"place_order","instrument_symbol":"ETHUSDT","quantity":1.25,"price":3200.5})");

    expect_true(command.has_value(), "json transaction line should parse");
    expect_equal(command->transaction_id, std::string("tx-2001"), "parsed transaction id should match");
    expect_equal(command->user_id, std::string("user-9"), "parsed user id should match");
    expect_equal(command->instrument_symbol, std::string("ETHUSDT"), "parsed instrument should match");
    expect_equal(command->quantity, 1.25, "parsed quantity should match");
    expect_true(command->price.has_value(), "parsed price should exist");
    expect_equal(*command->price, 3200.5, "parsed price should match");
}

// Verifies malformed JSON transaction lines are rejected safely.
TEST(Kafka, transaction_publisher_rejects_invalid_json_line) {
    log_test_step("reject malformed JSON transaction line");
    const auto command = trading::app::parse_json_transaction_command_line(
        R"({"transaction_id":"tx-2001","user_id":"user-9","quantity":"oops"})");

    expect_true(!command.has_value(), "invalid json transaction line should be rejected");
}
