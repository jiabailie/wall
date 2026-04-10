#pragma once

#include "core/types.hpp"

#include <optional>
#include <vector>

namespace trading::ingestion {

// Defines the boundary used to fetch transaction messages from Kafka.
class ITransactionConsumer {
public:
    virtual ~ITransactionConsumer() = default;

    // Returns the next transaction command if one is available.
    virtual std::optional<trading::core::TransactionCommand> poll() = 0;

    // Marks the provided offset as processed.
    virtual void commit(const trading::core::TransactionCommand& command) = 0;
};

// Provides a deterministic in-memory consumer for tests.
class MockTransactionConsumer final : public ITransactionConsumer {
public:
    // Builds the mock consumer from a fixed ordered message list.
    explicit MockTransactionConsumer(std::vector<trading::core::TransactionCommand> commands)
        : commands_(std::move(commands)) {}

    // Returns the next ordered mock command, if any remain.
    std::optional<trading::core::TransactionCommand> poll() override;

    // Records the committed command so tests can verify commit behavior.
    void commit(const trading::core::TransactionCommand& command) override;

    // Returns the committed commands in commit order.
    [[nodiscard]] const std::vector<trading::core::TransactionCommand>& committed() const { return committed_; }

private:
    std::vector<trading::core::TransactionCommand> commands_;
    std::vector<trading::core::TransactionCommand> committed_;
    std::size_t next_index_ {0};
};

}  // namespace trading::ingestion
