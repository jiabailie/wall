#pragma once

#include <cstdint>
#include <chrono>

namespace trading::core {

// Provides an abstract time source so runtime code can be tested deterministically.
class IClock {
public:
    virtual ~IClock() = default;

    // Returns the current time in milliseconds since an arbitrary epoch.
    virtual std::int64_t now_ms() const = 0;
};

// Provides a fixed time source for tests.
class FixedClock final : public IClock {
public:
    // Builds a clock with a deterministic starting value.
    explicit FixedClock(std::int64_t now_ms) : now_ms_(now_ms) {}

    // Returns the configured fixed time value.
    std::int64_t now_ms() const override { return now_ms_; }

    // Updates the fixed time value for the next read.
    void set_now_ms(std::int64_t now_ms) { now_ms_ = now_ms; }

private:
    std::int64_t now_ms_;
};

// Provides the current wall-clock time for application runtime code.
class SystemClock final : public IClock {
public:
    // Returns the current system time in milliseconds since the Unix epoch.
    std::int64_t now_ms() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

}  // namespace trading::core
