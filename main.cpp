#include "app/application.hpp"

#include <exception>
#include <iostream>
#include <string>

// Starts the application and reports a non-zero exit code on failure.
int main(int argc, char** argv) {
    try {
        // Step 1: Read the optional --config <path> argument.
        std::string config_path;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--config" && i + 1 < argc) {
                config_path = argv[++i];
            } else {
                std::cerr << "Usage: " << argv[0] << " [--config <path>]\n";
                return 2;
            }
        }

        // Step 2: Build the application with default dependencies.
        trading::app::Application application(config_path);

        // Step 3: Run the startup flow.
        application.run();
        return 0;
    } catch (const std::exception& exception) {
        // Step 4: Print the failure so local debugging is straightforward.
        std::cerr << "Application failed: " << exception.what() << '\n';
        return 1;
    }
}
