#include "app/engine_controller.hpp"

#include <utility>

namespace trading::app {

// Stores the timer so it can be emitted later in registration order.
void EngineController::schedule_timer(std::string timer_id, const std::int64_t due_timestamp_ms) {
    timers_.push_back({
        .timer_id = std::move(timer_id),
        .due_timestamp_ms = due_timestamp_ms,
        .emitted = false,
    });
}

// Polls each source once and appends any produced events to the queue.
std::size_t EngineController::poll_sources_once() {
    std::size_t enqueued_count = 0;

    // Step 1: Poll every source in deterministic registration order.
    for (auto* source : sources_) {
        if (const auto event = source->poll(); event.has_value()) {
            queue_.push_back(*event);
            ++enqueued_count;
        }
    }

    return enqueued_count;
}

// Enqueues all due timers once, preserving timer registration order.
std::size_t EngineController::enqueue_due_timers() {
    std::size_t enqueued_count = 0;
    const auto now_ms = clock_.now_ms();

    // Step 1: Scan the timers in insertion order.
    for (auto& timer : timers_) {
        if (!timer.emitted && timer.due_timestamp_ms <= now_ms) {
            queue_.push_back(trading::core::TimerEvent {
                .timer_id = timer.timer_id,
                .timestamp = timer.due_timestamp_ms,
            });
            timer.emitted = true;
            ++enqueued_count;
        }
    }

    return enqueued_count;
}

// Publishes queued events one by one in FIFO order.
std::size_t EngineController::drain() {
    std::size_t processed_count = 0;

    // Step 1: Drain until no queued event remains.
    while (!queue_.empty()) {
        const auto event = queue_.front();
        queue_.pop_front();
        dispatcher_.publish(event);
        ++processed_count;
    }

    return processed_count;
}

// Runs the controller's deterministic processing cycle.
std::size_t EngineController::run_once() {
    const auto due_timer_count = enqueue_due_timers();
    const auto polled_source_count = poll_sources_once();
    const auto drained_event_count = drain();

    static_cast<void>(due_timer_count);
    static_cast<void>(polled_source_count);
    return drained_event_count;
}

}  // namespace trading::app
