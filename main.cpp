#include "app/application.hpp"

#include <exception>
#include <iostream>

// Starts the application and reports a non-zero exit code on failure.
int main() {
    try {
        // Step 1: Build the application with default dependencies.
        trading::app::Application application;

        // Step 2: Run the startup flow.
        application.run();
        return 0;
    } catch (const std::exception& exception) {
        // Step 3: Print the failure so local debugging is straightforward.
        std::cerr << "Application failed: " << exception.what() << '\n';
        return 1;
    }
}
