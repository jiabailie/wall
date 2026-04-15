#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading::monitoring {

// Stores aggregate latency observations for one metric key.
struct LatencyMetric {
    std::int64_t sample_count {0};
    std::int64_t total_ms {0};
    std::int64_t max_ms {0};
};

// Defines the metrics boundary used by runtime and ingestion code.
class IMetricsCollector {
public:
    virtual ~IMetricsCollector() = default;

    // Increments one named counter by the provided delta.
    virtual void increment(const std::string& name, std::int64_t delta = 1) = 0;

    // Records one latency sample in milliseconds.
    virtual void observe_latency(const std::string& name, std::int64_t duration_ms) = 0;
};

// Captures metrics in memory for deterministic tests and local observability.
class InMemoryMetricsCollector final : public IMetricsCollector {
public:
    void increment(const std::string& name, std::int64_t delta = 1) override;
    void observe_latency(const std::string& name, std::int64_t duration_ms) override;

    [[nodiscard]] std::int64_t counter(const std::string& name) const;
    [[nodiscard]] LatencyMetric latency(const std::string& name) const;
    [[nodiscard]] const std::unordered_map<std::string, std::int64_t>& counters() const { return counters_; }

private:
    std::unordered_map<std::string, std::int64_t> counters_;
    std::unordered_map<std::string, LatencyMetric> latencies_;
};

}  // namespace trading::monitoring
