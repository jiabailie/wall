#pragma once

#include "config/app_config.hpp"

#include <string>

namespace trading::app {

// Wires the current skeleton components together for local startup.
class Application {
public:
    // Uses the default development config unless a path is provided.
    explicit Application(std::string config_path = {});

    // Starts the current application bootstrap flow.
    void run();

private:
    std::string config_path_;
};

}  // namespace trading::app
