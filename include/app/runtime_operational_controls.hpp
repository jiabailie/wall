#pragma once

namespace trading::app {

// Exposes simple runtime controls for live operations.
class RuntimeOperationalControls {
public:
    void pause_trading() { trading_paused_ = true; }
    void resume_trading() { trading_paused_ = false; }

    [[nodiscard]] bool trading_paused() const { return trading_paused_; }

private:
    bool trading_paused_ {false};
};

}  // namespace trading::app
