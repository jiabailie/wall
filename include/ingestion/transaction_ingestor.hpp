#pragma once

#include "core/event_dispatcher.hpp"
#include "core/clock.hpp"
#include "ingestion/transaction_consumer.hpp"
#include "monitoring/metrics.hpp"
#include "storage/storage_interfaces.hpp"

#include <string>

namespace trading::ingestion {

// Consumes ordered transaction commands and injects them into the engine runtime.
class TransactionIngestor {
public:
    // Builds the ingestor from the consumer, repository, cache, and dispatcher boundaries.
    TransactionIngestor(ITransactionConsumer& consumer,
                        trading::storage::ITransactionRepository& repository,
                        trading::storage::ITransactionCache& cache,
                        trading::core::EventDispatcher& dispatcher,
                        const trading::core::IClock* clock = nullptr,
                        trading::monitoring::IMetricsCollector* metrics = nullptr)
        : consumer_(consumer),
          repository_(repository),
          cache_(cache),
          dispatcher_(dispatcher),
          clock_(clock),
          metrics_(metrics) {}

    // Polls and processes one transaction message, returning true when a message was handled.
    bool process_next() const;

private:
    // Validates the minimum required fields for one transaction command.
    [[nodiscard]] bool is_valid(const trading::core::TransactionCommand& command) const;

    ITransactionConsumer& consumer_;
    trading::storage::ITransactionRepository& repository_;
    trading::storage::ITransactionCache& cache_;
    trading::core::EventDispatcher& dispatcher_;
    const trading::core::IClock* clock_ {nullptr};
    trading::monitoring::IMetricsCollector* metrics_ {nullptr};
};

}  // namespace trading::ingestion
