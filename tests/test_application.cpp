#include "test_support.hpp"

using namespace wall_test;

// Verifies startup refuses to fall back to local adapters when infrastructure.allow_fallback is false.
TEST(Application, refuses_fallback_when_not_allowed) {
    // Step 1: Build a config whose PostgreSQL port is unreachable and fallback is disabled.
    std::ifstream input(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto text = buffer.str();

    const auto replace_once = [&text](const std::string& from, const std::string& to) {
        const auto position = text.find(from);
        ASSERT_NE(position, std::string::npos) << from;
        text.replace(position, from.size(), to);
    };
    replace_once("postgres.port=5432", "postgres.port=1");
    replace_once("infrastructure.allow_fallback=true", "infrastructure.allow_fallback=false");

    const auto path = std::filesystem::temp_directory_path() / ("wall-test-no-fallback-" + std::to_string(::getpid()) + ".cfg");
    {
        std::ofstream output(path);
        output << text;
    }

    // Step 2: Running the application must fail instead of silently using local adapters.
    trading::app::Application application(path.string());
    bool threw = false;
    try {
        application.run();
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("allow_fallback") != std::string::npos;
    }
    std::filesystem::remove(path);
    expect_true(threw, "application should refuse to start without native infrastructure");
}
