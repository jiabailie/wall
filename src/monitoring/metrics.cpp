#include "monitoring/metrics.hpp"

#include <algorithm>

namespace trading::monitoring {

void InMemoryMetricsCollector::increment(const std::string& name, const std::int64_t delta) {
    counters_[name] += delta;
}

void InMemoryMetricsCollector::observe_latency(const std::string& name, const std::int64_t duration_ms) {
    auto& metric = latencies_[name];
    ++metric.sample_count;
    metric.total_ms += duration_ms;
    metric.max_ms = std::max(metric.max_ms, duration_ms);
}

std::int64_t InMemoryMetricsCollector::counter(const std::string& name) const {
    if (const auto iterator = counters_.find(name); iterator != counters_.end()) {
        return iterator->second;
    }

    return 0;
}

LatencyMetric InMemoryMetricsCollector::latency(const std::string& name) const {
    if (const auto iterator = latencies_.find(name); iterator != latencies_.end()) {
        return iterator->second;
    }

    return {};
}

}  // namespace trading::monitoring
