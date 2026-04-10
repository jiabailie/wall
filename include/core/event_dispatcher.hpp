#pragma once

#include "core/types.hpp"

#include <functional>
#include <vector>

namespace trading::core {

// Dispatches engine events to registered handlers in registration order.
class EventDispatcher {
public:
    using Handler = std::function<void(const EngineEvent&)>;

    // Registers one event handler to receive future events.
    void subscribe(Handler handler) { handlers_.push_back(std::move(handler)); }

    // Delivers the event to every handler in deterministic registration order.
    void publish(const EngineEvent& event) const {
        // Step 1: Iterate handlers in insertion order.
        for (const auto& handler : handlers_) {
            // Step 2: Invoke the current handler with the shared event object.
            handler(event);
        }
    }

private:
    std::vector<Handler> handlers_;
};

}  // namespace trading::core
