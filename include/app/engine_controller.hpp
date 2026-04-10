#pragma once

#include "core/clock.hpp"
#include "core/event_dispatcher.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace trading::app {

// Defines a source that can provide engine events to the runtime controller.
class IEngineEventSource {
public:
    virtual ~IEngineEventSource() = default;

    // Returns the next available event, or an empty value when no event is ready.
    virtual std::optional<trading::core::EngineEvent> poll() = 0;
};

// Owns the deterministic runtime queue and feeds events to subscribers.
class EngineController {
public:
    // Builds the controller using an injected clock for timer handling.
    explicit EngineController(const trading::core::IClock& clock) : clock_(clock) {}

    using Handler = trading::core::EventDispatcher::Handler;

    // Registers one event handler in deterministic subscription order.
    void subscribe(Handler handler) { dispatcher_.subscribe(std::move(handler)); }

    // Registers one external event source in deterministic polling order.
    void add_source(IEngineEventSource& source) { sources_.push_back(&source); }

    // Enqueues one event at the end of the runtime queue.
    void enqueue(const trading::core::EngineEvent& event) { queue_.push_back(event); }

    // Schedules a timer to be emitted once when the clock reaches the provided timestamp.
    void schedule_timer(std::string timer_id, std::int64_t due_timestamp_ms);

    // Polls each registered source once in registration order.
    [[nodiscard]] std::size_t poll_sources_once();

    // Enqueues all timers that are due and not yet emitted.
    [[nodiscard]] std::size_t enqueue_due_timers();

    // Publishes queued events in FIFO order until the queue is empty.
    [[nodiscard]] std::size_t drain();

    // Runs one deterministic runtime cycle: timers, then sources, then queue drain.
    [[nodiscard]] std::size_t run_once();

    // Returns the number of queued events waiting to be processed.
    [[nodiscard]] std::size_t queued_event_count() const { return queue_.size(); }

private:
    struct ScheduledTimer {
        std::string timer_id;
        std::int64_t due_timestamp_ms {0};
        bool emitted {false};
    };

    const trading::core::IClock& clock_;
    trading::core::EventDispatcher dispatcher_;
    std::deque<trading::core::EngineEvent> queue_;
    std::vector<IEngineEventSource*> sources_;
    std::vector<ScheduledTimer> timers_;
};

}  // namespace trading::app
