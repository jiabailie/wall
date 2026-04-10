#pragma once

#include "app/engine_controller.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <vector>

namespace trading::app {

// Provides deterministic market events for development and tests.
class MockMarketDataSource final : public IEngineEventSource {
public:
    // Builds the source from a fixed ordered list of market events.
    explicit MockMarketDataSource(std::vector<trading::core::MarketEvent> events)
        : events_(std::move(events)) {}

    // Returns the next market event in the configured order.
    std::optional<trading::core::EngineEvent> poll() override;

private:
    std::vector<trading::core::MarketEvent> events_;
    std::size_t next_index_ {0};
};

// Provides deterministic transaction commands for development and tests.
class MockTransactionEventSource final : public IEngineEventSource {
public:
    // Builds the source from a fixed ordered list of transaction commands.
    explicit MockTransactionEventSource(std::vector<trading::core::TransactionCommand> commands)
        : commands_(std::move(commands)) {}

    // Returns the next transaction command in the configured order.
    std::optional<trading::core::EngineEvent> poll() override;

private:
    std::vector<trading::core::TransactionCommand> commands_;
    std::size_t next_index_ {0};
};

}  // namespace trading::app
