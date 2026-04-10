#pragma once

#include "config/app_config.hpp"

#include <string>

namespace trading::config {

// Loads a simple key-value configuration file and applies environment overrides.
class ConfigLoader {
public:
    // Reads the file from disk, applies overrides, and returns the parsed config.
    AppConfig load_from_file(const std::string& path) const;

    // Validates that the minimum required settings are present.
    ConfigValidationResult validate(const AppConfig& config) const;
};

}  // namespace trading::config
