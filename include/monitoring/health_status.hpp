#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace trading::monitoring {

// Declares runtime component names tracked by health checks.
enum class RuntimeComponent {
    ingestion,
    market_data,
    risk,
    execution
};

// Declares the supported health states for one component.
enum class HealthStatus {
    unknown,
    healthy,
    degraded,
    unavailable
};

// Tracks component-level health and computes an overall runtime health state.
class RuntimeHealthStatus {
public:
    // Updates the health status for one runtime component.
    void set_status(RuntimeComponent component, HealthStatus status);

    // Returns the latest health status for one runtime component.
    [[nodiscard]] HealthStatus get_status(RuntimeComponent component) const;

    // Returns a roll-up health status across all runtime components.
    [[nodiscard]] HealthStatus overall_status() const;

private:
    static constexpr std::size_t component_count_ {4};
    std::array<HealthStatus, component_count_> component_statuses_ {
        HealthStatus::unknown,
        HealthStatus::unknown,
        HealthStatus::unknown,
        HealthStatus::unknown,
    };
};

// Returns a stable string representation for the provided health status.
[[nodiscard]] std::string to_string(HealthStatus status);

}  // namespace trading::monitoring
