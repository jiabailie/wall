#pragma once

#include "config/app_config.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>

namespace trading::app {

// Parses one flat JSON object line into a transaction command used by the producer app.
[[nodiscard]] std::optional<trading::core::TransactionCommand> parse_json_transaction_command_line(const std::string& line);

// Publishes transaction commands from stdin or a JSONL file into Kafka.
class TransactionPublisherApplication {
public:
    // Runs the producer-side application. argv[1] is an optional JSONL input file path.
    void run(int argc, char** argv) const;

private:
    [[nodiscard]] trading::config::AppConfig load_config() const;
};

}  // namespace trading::app
