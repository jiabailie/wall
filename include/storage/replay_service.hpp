#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace trading::storage {

// Stores replay execution counters.
struct ReplayStats {
    std::size_t total_records {0};
    std::size_t replayed_records {0};
    std::size_t skipped_records {0};
};

// Persists append-only engine events and replays them later.
class ReplayService {
public:
    explicit ReplayService(std::filesystem::path log_path);

    // Removes any existing persisted event log.
    void reset_log() const;

    // Appends one normalized runtime event to the event log.
    void append_event(const trading::core::EngineEvent& event) const;

    // Replays persisted events in file order.
    [[nodiscard]] ReplayStats replay(const std::function<void(const trading::core::EngineEvent&)>& on_event) const;

private:
    [[nodiscard]] std::string serialize_event(const trading::core::EngineEvent& event) const;
    [[nodiscard]] std::optional<trading::core::EngineEvent> deserialize_event(const std::string& line) const;

    std::filesystem::path log_path_;
};

}  // namespace trading::storage
