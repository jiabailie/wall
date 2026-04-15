#pragma once

#include "core/clock.hpp"
#include "monitoring/health_status.hpp"

#include <cstddef>
#include <cstdint>

namespace trading::market_data {

// Tracks feed connectivity and freshness beyond simple stale-market checks.
class FeedHealthTracker {
public:
    explicit FeedHealthTracker(const trading::core::IClock& clock) : clock_(clock) {}

    void on_connect();
    void on_subscribe_success();
    void on_message(std::int64_t receive_timestamp);
    void on_disconnect();

    [[nodiscard]] trading::monitoring::HealthStatus status(std::int64_t stale_after_ms,
                                                           std::int64_t disconnect_grace_ms) const;
    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] bool subscribed() const { return subscribed_; }
    [[nodiscard]] std::int64_t last_message_timestamp() const { return last_message_timestamp_; }
    [[nodiscard]] std::size_t disconnect_count() const { return disconnect_count_; }

private:
    const trading::core::IClock& clock_;
    bool connected_ {false};
    bool subscribed_ {false};
    std::int64_t last_message_timestamp_ {0};
    std::int64_t last_disconnect_timestamp_ {0};
    std::size_t disconnect_count_ {0};
};

}  // namespace trading::market_data
