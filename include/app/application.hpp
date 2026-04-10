#pragma once

#include "config/app_config.hpp"

namespace trading::app {

// Wires the current skeleton components together for local startup.
class Application {
public:
    // Starts the current application bootstrap flow.
    void run();
};

}  // namespace trading::app
