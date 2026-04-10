#include "monitoring/health_status.hpp"

namespace trading::monitoring {

namespace {

// Maps the runtime component enum to an array index.
std::size_t component_index(const RuntimeComponent component) {
    switch (component) {
        case RuntimeComponent::ingestion:
            return 0;
        case RuntimeComponent::market_data:
            return 1;
        case RuntimeComponent::risk:
            return 2;
        case RuntimeComponent::execution:
            return 3;
    }

    return 0;
}

}  // namespace

// Updates the health status for one runtime component.
void RuntimeHealthStatus::set_status(const RuntimeComponent component, const HealthStatus status) {
    component_statuses_[component_index(component)] = status;
}

// Returns the latest health status for one runtime component.
HealthStatus RuntimeHealthStatus::get_status(const RuntimeComponent component) const {
    return component_statuses_[component_index(component)];
}

// Returns a roll-up health status across all runtime components.
HealthStatus RuntimeHealthStatus::overall_status() const {
    bool has_unknown = false;
    bool has_degraded = false;

    for (const auto status : component_statuses_) {
        if (status == HealthStatus::unavailable) {
            return HealthStatus::unavailable;
        }
        if (status == HealthStatus::degraded) {
            has_degraded = true;
        }
        if (status == HealthStatus::unknown) {
            has_unknown = true;
        }
    }

    if (has_degraded) {
        return HealthStatus::degraded;
    }
    return has_unknown ? HealthStatus::unknown : HealthStatus::healthy;
}

// Returns a stable string representation for the provided health status.
std::string to_string(const HealthStatus status) {
    switch (status) {
        case HealthStatus::unknown:
            return "unknown";
        case HealthStatus::healthy:
            return "healthy";
        case HealthStatus::degraded:
            return "degraded";
        case HealthStatus::unavailable:
            return "unavailable";
    }

    return "unknown";
}

}  // namespace trading::monitoring
