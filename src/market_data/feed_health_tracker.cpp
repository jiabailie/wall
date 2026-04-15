#include "market_data/feed_health_tracker.hpp"

namespace trading::market_data {

void FeedHealthTracker::on_connect() {
    connected_ = true;
    subscribed_ = false;
}

void FeedHealthTracker::on_subscribe_success() {
    connected_ = true;
    subscribed_ = true;
}

void FeedHealthTracker::on_message(const std::int64_t receive_timestamp) {
    connected_ = true;
    subscribed_ = true;
    last_message_timestamp_ = receive_timestamp;
}

void FeedHealthTracker::on_disconnect() {
    connected_ = false;
    subscribed_ = false;
    last_disconnect_timestamp_ = clock_.now_ms();
    ++disconnect_count_;
}

trading::monitoring::HealthStatus FeedHealthTracker::status(const std::int64_t stale_after_ms,
                                                            const std::int64_t disconnect_grace_ms) const {
    if (!connected_) {
        if (last_disconnect_timestamp_ == 0) {
            return trading::monitoring::HealthStatus::unknown;
        }
        return (clock_.now_ms() - last_disconnect_timestamp_) > disconnect_grace_ms
            ? trading::monitoring::HealthStatus::unavailable
            : trading::monitoring::HealthStatus::degraded;
    }

    if (!subscribed_) {
        return trading::monitoring::HealthStatus::degraded;
    }
    if (last_message_timestamp_ == 0) {
        return trading::monitoring::HealthStatus::unknown;
    }

    return (clock_.now_ms() - last_message_timestamp_) > stale_after_ms
        ? trading::monitoring::HealthStatus::degraded
        : trading::monitoring::HealthStatus::healthy;
}

}  // namespace trading::market_data
