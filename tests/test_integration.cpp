#include "test_support.hpp"

#include "infrastructure/postgres_repositories.hpp"
#include "infrastructure/redis_cache.hpp"

using namespace wall_test;

namespace {

// Returns true when the opt-in integration environment (docker compose stack) is enabled.
bool integration_enabled() {
    const char* value = std::getenv("WALL_INTEGRATION");
    return value != nullptr && std::string(value) == "1";
}

// Returns the compose-stack PostgreSQL settings, overridable through the environment.
trading::config::PostgresConfig postgres_config() {
    const auto read = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return std::string(value != nullptr ? value : fallback);
    };
    return {
        .host = read("WALL_PG_HOST", "localhost"),
        .port = std::stoi(read("WALL_PG_PORT", "5432")),
        .database = read("WALL_PG_DATABASE", "trading"),
        .user = read("WALL_PG_USER", "trading_user"),
        .password = read("WALL_PG_PASSWORD", "trading_pass"),
    };
}

}  // namespace

// Verifies the libpq transaction repository round-trips a record against a real PostgreSQL.
TEST(Integration, postgres_transaction_round_trip) {
    if (!integration_enabled()) {
        GTEST_SKIP() << "set WALL_INTEGRATION=1 and start wall/docker-compose.yml to run";
    }

    trading::infrastructure::LibpqTransactionRepository repository(postgres_config());
    const auto transaction_id = "it-" + std::to_string(::getpid());
    repository.save_received(make_transaction(transaction_id, 5));
    repository.save_processed(transaction_id, "processed");

    const auto record = repository.get_record(transaction_id);
    ASSERT_TRUE(record.has_value());
    expect_equal(record->status, std::string("processed"), "status should round-trip through PostgreSQL");
}

// Verifies the hiredis transaction cache round-trips a status against a real Redis.
TEST(Integration, redis_transaction_status_round_trip) {
    if (!integration_enabled()) {
        GTEST_SKIP() << "set WALL_INTEGRATION=1 and start wall/docker-compose.yml to run";
    }

    trading::infrastructure::HiredisTransactionCache cache({.host = "localhost", .port = 6379});
    const auto transaction_id = "it-" + std::to_string(::getpid());
    cache.set_status(transaction_id, "processed");
    expect_equal(cache.get_status(transaction_id), std::string("processed"), "status should round-trip through Redis");
}
