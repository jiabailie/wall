#include "core/event_dispatcher.hpp"
#include "core/types.hpp"
#include "ingestion/transaction_consumer.hpp"
#include "ingestion/transaction_ingestor.hpp"
#include "storage/storage_interfaces.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct BenchmarkStepStats {
    std::int64_t call_count {0};
    std::chrono::nanoseconds total_duration {0};
    std::chrono::nanoseconds max_duration {0};
};

class BenchmarkRecorder final {
public:
    void record(const std::string& step_name, const std::chrono::nanoseconds duration) {
        auto& stats = stats_[step_name];
        ++stats.call_count;
        stats.total_duration += duration;
        if (duration > stats.max_duration) {
            stats.max_duration = duration;
        }
    }

    [[nodiscard]] const BenchmarkStepStats& step(const std::string& step_name) const {
        const auto iterator = stats_.find(step_name);
        if (iterator == stats_.end()) {
            throw std::runtime_error("missing benchmark step: " + step_name);
        }

        return iterator->second;
    }

private:
    std::unordered_map<std::string, BenchmarkStepStats> stats_;
};

class BenchmarkingTransactionConsumer final : public trading::ingestion::ITransactionConsumer {
public:
    BenchmarkingTransactionConsumer(std::vector<trading::core::TransactionCommand> commands,
                                    BenchmarkRecorder& recorder)
        : inner_(std::move(commands)),
          recorder_(recorder) {}

    std::optional<trading::core::TransactionCommand> poll() override {
        const auto started_at = std::chrono::steady_clock::now();
        auto command = inner_.poll();
        recorder_.record("step_1_poll", std::chrono::steady_clock::now() - started_at);
        return command;
    }

    void commit(const trading::core::TransactionCommand& command) override {
        const auto started_at = std::chrono::steady_clock::now();
        inner_.commit(command);
        recorder_.record("step_5_commit", std::chrono::steady_clock::now() - started_at);
    }

    [[nodiscard]] const std::vector<trading::core::TransactionCommand>& committed() const {
        return inner_.committed();
    }

private:
    trading::ingestion::MockTransactionConsumer inner_;
    BenchmarkRecorder& recorder_;
};

class BenchmarkingTransactionRepository final : public trading::storage::ITransactionRepository {
public:
    explicit BenchmarkingTransactionRepository(BenchmarkRecorder& recorder) : recorder_(recorder) {}

    void save_received(const trading::core::TransactionCommand& command) override {
        const auto started_at = std::chrono::steady_clock::now();
        inner_.save_received(command);
        recorder_.record("step_2_save_received", std::chrono::steady_clock::now() - started_at);
    }

    void save_processed(const std::string& transaction_id, const std::string& status) override {
        const auto started_at = std::chrono::steady_clock::now();
        inner_.save_processed(transaction_id, status);
        recorder_.record("step_5_save_processed", std::chrono::steady_clock::now() - started_at);
    }

    [[nodiscard]] std::vector<trading::storage::TransactionRecord> all_records() const override {
        return inner_.all_records();
    }

    void save_checkpoint(const trading::storage::KafkaCheckpoint& checkpoint) override {
        inner_.save_checkpoint(checkpoint);
    }

    [[nodiscard]] std::vector<trading::storage::KafkaCheckpoint> all_checkpoints() const override {
        return inner_.all_checkpoints();
    }

    [[nodiscard]] const std::vector<trading::storage::TransactionRecord>& records() const {
        return inner_.records();
    }

private:
    trading::storage::InMemoryTransactionRepository inner_;
    BenchmarkRecorder& recorder_;
};

class BenchmarkingTransactionCache final : public trading::storage::ITransactionCache {
public:
    explicit BenchmarkingTransactionCache(BenchmarkRecorder& recorder) : recorder_(recorder) {}

    void set_status(const std::string& transaction_id, const std::string& status) override {
        const auto started_at = std::chrono::steady_clock::now();
        inner_.set_status(transaction_id, status);
        const auto duration = std::chrono::steady_clock::now() - started_at;
        if (status == "received") {
            recorder_.record("step_2_cache_received", duration);
            return;
        }

        if (status == "processed" || status == "rejected") {
            recorder_.record("step_5_cache_final_status", duration);
        }
    }

    [[nodiscard]] std::string get_status(const std::string& transaction_id) const {
        return inner_.get_status(transaction_id);
    }

private:
    trading::storage::InMemoryTransactionCache inner_;
    BenchmarkRecorder& recorder_;
};

trading::core::TransactionCommand make_transaction(const std::string& transaction_id,
                                                   const std::int64_t kafka_offset) {
    return {
        .transaction_id = transaction_id,
        .user_id = "user-1",
        .account_id = "account-1",
        .command_type = "place_order",
        .instrument_symbol = "BTCUSDT",
        .quantity = 1.5,
        .price = 42000.0,
        .kafka_topic = "trading-transactions",
        .kafka_partition = 0,
        .kafka_offset = kafka_offset,
    };
}

void expect_true(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void print_step_summary(BenchmarkRecorder& recorder, const std::string& step_name) {
    const auto& stats = recorder.step(step_name);
    const auto average_ns = stats.call_count > 0 ? stats.total_duration.count() / stats.call_count : 0;
    std::cout << std::fixed << std::setprecision(3)
              << "[BENCH] " << step_name
              << " calls=" << stats.call_count
              << " avg_us=" << (static_cast<double>(average_ns) / 1000.0)
              << " max_us=" << (static_cast<double>(stats.max_duration.count()) / 1000.0)
              << " total_ms=" << (static_cast<double>(stats.total_duration.count()) / 1000000.0)
              << '\n';
}

int run_transaction_ingestor_benchmark(const std::size_t benchmark_iterations) {
    BenchmarkRecorder recorder;
    trading::core::EventDispatcher dispatcher;
    std::size_t published_count = 0;
    dispatcher.subscribe([&published_count, &recorder](const trading::core::EngineEvent& event) {
        const auto started_at = std::chrono::steady_clock::now();
        if (std::holds_alternative<trading::core::TransactionCommand>(event)) {
            ++published_count;
        }
        recorder.record("step_4_dispatch", std::chrono::steady_clock::now() - started_at);
    });

    std::vector<trading::core::TransactionCommand> commands;
    commands.reserve(benchmark_iterations);
    for (std::size_t index = 0; index < benchmark_iterations; ++index) {
        commands.push_back(make_transaction("bench-tx-" + std::to_string(index + 1), static_cast<std::int64_t>(index)));
    }

    BenchmarkingTransactionConsumer consumer(std::move(commands), recorder);
    BenchmarkingTransactionRepository repository(recorder);
    BenchmarkingTransactionCache cache(recorder);
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher);

    for (std::size_t index = 0; index < benchmark_iterations; ++index) {
        const auto started_at = std::chrono::steady_clock::now();
        expect_true(ingestor.process_next(), "benchmark iteration should process one transaction");
        recorder.record("process_next_total", std::chrono::steady_clock::now() - started_at);
    }

    expect_true(published_count == benchmark_iterations, "every benchmark transaction should be dispatched");
    expect_true(consumer.committed().size() == benchmark_iterations, "every benchmark transaction should be committed");
    expect_true(repository.records().size() == benchmark_iterations, "every benchmark transaction should persist");
    expect_true(cache.get_status("bench-tx-1") == "processed", "benchmark cache should retain final status");

    std::cout << "[BENCH] transaction_ingestor_step_benchmark iterations=" << benchmark_iterations << '\n';
    print_step_summary(recorder, "process_next_total");
    print_step_summary(recorder, "step_1_poll");
    print_step_summary(recorder, "step_2_save_received");
    print_step_summary(recorder, "step_2_cache_received");
    print_step_summary(recorder, "step_4_dispatch");
    print_step_summary(recorder, "step_5_save_processed");
    print_step_summary(recorder, "step_5_cache_final_status");
    print_step_summary(recorder, "step_5_commit");

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::size_t benchmark_iterations = 2000;
    if (argc > 1) {
        benchmark_iterations = static_cast<std::size_t>(std::stoull(argv[1]));
    }

    try {
        return run_transaction_ingestor_benchmark(benchmark_iterations);
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] benchmark_main: " << exception.what() << '\n';
        return 1;
    }
}
