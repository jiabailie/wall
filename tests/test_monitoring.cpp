#include "test_support.hpp"

using namespace wall_test;

// Verifies in-memory structured logging captures level, message, and fields.
TEST(Monitoring, in_memory_structured_logger_records) {
    trading::monitoring::InMemoryStructuredLogger logger;
    logger.log(
        trading::monitoring::LogLevel::warn,
        "risk_limit_hit",
        {
            {"request_id", "req-1"},
            {"reason", "max_notional"},
        });

    expect_equal(logger.records().size(), std::size_t {1}, "one log record should be captured");
    expect_equal(logger.records()[0].message, std::string("risk_limit_hit"), "message should match");
    expect_equal(logger.records()[0].level, trading::monitoring::LogLevel::warn, "level should match");
    expect_equal(logger.records()[0].fields.at("request_id"), std::string("req-1"), "field should be captured");
}

// Verifies console logger renders a structured line with key fields.
TEST(Monitoring, console_structured_logger_writes_output) {
    std::ostringstream output;
    trading::monitoring::ConsoleStructuredLogger logger(output);
    logger.log(
        trading::monitoring::LogLevel::info,
        "startup_complete",
        {
            {"mode", "paper"},
            {"exchange", "binance"},
        });

    const auto rendered = output.str();
    expect_true(rendered.find("level=info") != std::string::npos, "console log should include level");
    expect_true(rendered.find("message=startup_complete") != std::string::npos, "console log should include message");
    expect_true(rendered.find("mode=paper") != std::string::npos, "console log should include mode field");
}

// Verifies component health transitions and roll-up logic.
TEST(Monitoring, runtime_health_status_transitions) {
    trading::monitoring::RuntimeHealthStatus health;
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::unknown, "default overall status should be unknown");

    health.set_status(trading::monitoring::RuntimeComponent::ingestion, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::market_data, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::execution, trading::monitoring::HealthStatus::healthy);
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::healthy, "all-healthy should roll up healthy");

    health.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::degraded);
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::degraded, "degraded component should roll up degraded");

    health.set_status(trading::monitoring::RuntimeComponent::execution, trading::monitoring::HealthStatus::unavailable);
    expect_equal(
        health.get_status(trading::monitoring::RuntimeComponent::execution),
        trading::monitoring::HealthStatus::unavailable,
        "execution component should be unavailable");
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::unavailable, "unavailable component should roll up unavailable");
}

// Verifies counters and latency samples are aggregated in memory.
TEST(Monitoring, in_memory_metrics_collector_records_counters_and_latency) {
    trading::monitoring::InMemoryMetricsCollector metrics;
    metrics.increment("fills_applied");
    metrics.increment("fills_applied", 2);
    metrics.observe_latency("runtime_event_latency_ms", 3);
    metrics.observe_latency("runtime_event_latency_ms", 7);

    expect_equal(metrics.counter("fills_applied"), std::int64_t {3}, "metrics counter should aggregate increments");
    const auto latency = metrics.latency("runtime_event_latency_ms");
    expect_equal(latency.sample_count, std::int64_t {2}, "latency metric should record sample count");
    expect_equal(latency.total_ms, std::int64_t {10}, "latency metric should record total latency");
    expect_equal(latency.max_ms, std::int64_t {7}, "latency metric should record max latency");
}
