#include "ingestion/transaction_consumer.hpp"

namespace trading::ingestion {

// Returns the next mock transaction in fixed order.
std::optional<trading::core::TransactionCommand> MockTransactionConsumer::poll() {
    // Step 1: Stop when every command has been consumed.
    if (next_index_ >= commands_.size()) {
        return std::nullopt;
    }

    // Step 2: Return the next ordered command and advance the cursor.
    return commands_[next_index_++];
}

// Records the committed command in commit order.
void MockTransactionConsumer::commit(const trading::core::TransactionCommand& command) {
    // Step 1: Store the committed command for later verification.
    committed_.push_back(command);
}

}  // namespace trading::ingestion
