#include "monitoring/logger.hpp"

#include <ostream>

namespace trading::monitoring {

ConsoleStructuredLogger::ConsoleStructuredLogger(std::ostream& output_stream) : output_stream_(output_stream) {}

// Writes one structured log line as key=value tokens.
void ConsoleStructuredLogger::log(const LogLevel level,
                                  const std::string& message,
                                  std::unordered_map<std::string, std::string> fields) {
    output_stream_ << "level=" << to_string(level) << " message=" << message;
    for (const auto& [key, value] : fields) {
        output_stream_ << ' ' << key << '=' << value;
    }
    output_stream_ << '\n';
}

// Stores one structured log record in memory.
void InMemoryStructuredLogger::log(const LogLevel level,
                                   const std::string& message,
                                   std::unordered_map<std::string, std::string> fields) {
    records_.push_back({
        .level = level,
        .message = message,
        .fields = std::move(fields),
    });
}

// Returns a stable string representation for the provided log level.
std::string to_string(const LogLevel level) {
    switch (level) {
        case LogLevel::info:
            return "info";
        case LogLevel::warn:
            return "warn";
        case LogLevel::error:
            return "error";
    }

    return "unknown";
}

}  // namespace trading::monitoring
