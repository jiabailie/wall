#include "ingestion/transaction_ingestor.hpp"

namespace trading::ingestion {

// Polls one transaction, validates it, persists state transitions, and publishes it into the runtime.
bool TransactionIngestor::process_next() const {
    const auto started_ms = clock_ != nullptr ? clock_->now_ms() : 0;

    // Step 1: Poll the next transaction command from the consumer.
    const auto command = consumer_.poll();
    if (!command.has_value()) {
        return false;
    }

    // Step 2: Persist receipt before any engine-side processing.
    try {
        repository_.save_received(*command);
    } catch (...) {
        if (metrics_ != nullptr) {
            metrics_->increment("persistence_failures");
        }
        throw;
    }
    cache_.set_status(command->transaction_id, "received");
    if (metrics_ != nullptr) {
        metrics_->increment("transactions_received");
    }

    // Step 3: Reject invalid commands without publishing them into the runtime.
    if (!is_valid(*command)) {
        try {
            repository_.save_processed(command->transaction_id, "rejected");
        } catch (...) {
            if (metrics_ != nullptr) {
                metrics_->increment("persistence_failures");
            }
            throw;
        }
        cache_.set_status(command->transaction_id, "rejected");
        consumer_.commit(*command);
        if (metrics_ != nullptr) {
            metrics_->increment("transactions_rejected");
            if (clock_ != nullptr) {
                metrics_->observe_latency("transaction_processing_latency_ms", clock_->now_ms() - started_ms);
            }
        }
        return true;
    }

    // Step 4: Publish the valid transaction command into the dispatcher.
    dispatcher_.publish(*command);

    // Step 5: Persist successful processing and commit the consumed offset.
    try {
        repository_.save_processed(command->transaction_id, "processed");
    } catch (...) {
        if (metrics_ != nullptr) {
            metrics_->increment("persistence_failures");
        }
        throw;
    }
    cache_.set_status(command->transaction_id, "processed");
    consumer_.commit(*command);
    if (metrics_ != nullptr) {
        metrics_->increment("transactions_processed");
        if (clock_ != nullptr) {
            metrics_->observe_latency("transaction_processing_latency_ms", clock_->now_ms() - started_ms);
        }
    }
    return true;
}

// Verifies that the minimum transaction fields are present and usable.
bool TransactionIngestor::is_valid(const trading::core::TransactionCommand& command) const {
    // Step 1: Require stable identity and Kafka metadata.
    if (command.transaction_id.empty() || command.kafka_topic.empty() || command.kafka_offset < 0) {
        return false;
    }

    // Step 2: Require the command payload fields used by the engine.
    if (command.account_id.empty() || command.command_type.empty() || command.instrument_symbol.empty()) {
        return false;
    }

    // Step 3: Require a positive quantity for processing.
    return command.quantity > 0.0;
}

}  // namespace trading::ingestion
