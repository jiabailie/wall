#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::monitoring {

// Declares supported structured log levels.
enum class LogLevel {
    info,
    warn,
    error
};

// Stores one structured log record.
struct LogRecord {
    LogLevel level {LogLevel::info};
    std::string message;
    std::unordered_map<std::string, std::string> fields;
};

// Defines the structured logging boundary used by runtime modules.
class IStructuredLogger {
public:
    virtual ~IStructuredLogger() = default;

    // Writes one structured log record.
    virtual void log(LogLevel level,
                     const std::string& message,
                     std::unordered_map<std::string, std::string> fields = {}) = 0;
};

// Writes structured log records to an output stream.
class ConsoleStructuredLogger final : public IStructuredLogger {
public:
    explicit ConsoleStructuredLogger(std::ostream& output_stream);

    void log(LogLevel level,
             const std::string& message,
             std::unordered_map<std::string, std::string> fields = {}) override;

private:
    std::ostream& output_stream_;
};

// Captures structured log records in memory for deterministic tests.
class InMemoryStructuredLogger final : public IStructuredLogger {
public:
    void log(LogLevel level,
             const std::string& message,
             std::unordered_map<std::string, std::string> fields = {}) override;

    [[nodiscard]] const std::vector<LogRecord>& records() const { return records_; }

private:
    std::vector<LogRecord> records_;
};

// Returns a stable string representation for the provided log level.
[[nodiscard]] std::string to_string(LogLevel level);

}  // namespace trading::monitoring
