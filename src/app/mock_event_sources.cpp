#include "app/mock_event_sources.hpp"

namespace trading::app {

// Returns the next configured market event, if one remains.
std::optional<trading::core::EngineEvent> MockMarketDataSource::poll() {
    if (next_index_ >= events_.size()) {
        return std::nullopt;
    }

    return events_[next_index_++];
}

// Returns the next configured transaction command, if one remains.
std::optional<trading::core::EngineEvent> MockTransactionEventSource::poll() {
    if (next_index_ >= commands_.size()) {
        return std::nullopt;
    }

    return commands_[next_index_++];
}

}  // namespace trading::app
