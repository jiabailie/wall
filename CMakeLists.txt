cmake_minimum_required(VERSION 3.20)
project(wall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WALL_WARNINGS "Enable extra compiler warnings" ON)
option(WALL_WERROR "Treat compiler warnings as errors" OFF)
option(WALL_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Default to an optimized build with debug info so benchmarks are meaningful.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WALL_WARNINGS)
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(WALL_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

if(WALL_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka)
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
    set(LIBPQ_INCLUDE_DIRS ${PostgreSQL_INCLUDE_DIRS})
    set(LIBPQ_LINK_TARGET PostgreSQL::PostgreSQL)
else()
    find_path(LIBPQ_INCLUDE_DIR
        NAMES libpq-fe.h
        PATH_SUFFIXES postgresql
        PATHS
            /opt/homebrew/opt/libpq/include
            /usr/local/opt/libpq/include
    )
    find_library(LIBPQ_LIBRARY
        NAMES pq libpq
        PATHS
            /opt/homebrew/opt/libpq/lib
            /usr/local/opt/libpq/lib
    )

    if(NOT LIBPQ_INCLUDE_DIR OR NOT LIBPQ_LIBRARY)
        message(FATAL_ERROR "libpq headers or library were not found")
    endif()

    set(LIBPQ_INCLUDE_DIRS ${LIBPQ_INCLUDE_DIR})
    set(LIBPQ_LINK_TARGET ${LIBPQ_LIBRARY})
endif()

add_library(trading_engine
    src/app/application.cpp
    src/app/backtest_runner.cpp
    src/app/engine_controller.cpp
    src/app/mock_event_sources.cpp
    src/app/recovery_service.cpp
    src/app/simulation_runtime.cpp
    src/app/transaction_publisher_application.cpp
    src/config/config_loader.cpp
    src/exchange/exchange_execution_adapter.cpp
    src/exchange/exchange_market_data_adapter.cpp
    src/exchange/exchange_router.cpp
    src/exchange/live_market_data_feed.cpp
    src/execution/live_execution_tracker.cpp
    src/execution/matching_engine.cpp
    src/execution/order_book.cpp
    src/execution/simulated_execution_engine.cpp
    src/infrastructure/postgres_repositories.cpp
    src/infrastructure/kafka_transaction_consumer.cpp
    src/infrastructure/kafka_transaction_producer.cpp
    src/infrastructure/redis_cache.cpp
    src/ingestion/transaction_consumer.cpp
    src/ingestion/transaction_ingestor.cpp
    src/market_data/feed_health_tracker.cpp
    src/market_data/market_state_store.cpp
    src/monitoring/health_status.cpp
    src/monitoring/logger.cpp
    src/monitoring/metrics.cpp
    src/portfolio/portfolio_service.cpp
    src/risk/risk_engine.cpp
    src/storage/replay_service.cpp
    src/storage/storage_interfaces.cpp
    src/strategy/sample_threshold_strategy.cpp
    src/strategy/spread_capture_strategy.cpp
    src/strategy/strategy_coordinator.cpp
)

# librdkafka's RD_KAFKA_V_* macros use GNU statement expressions.
set_source_files_properties(src/infrastructure/kafka_transaction_producer.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-gnu-statement-expression-from-macro-expansion")

target_include_directories(trading_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${LIBPQ_INCLUDE_DIRS}
)

target_compile_definitions(trading_engine
    PUBLIC
        TRADING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(trading_engine
    PRIVATE
        ${LIBPQ_LINK_TARGET}
        PkgConfig::HIREDIS
        PkgConfig::RDKAFKA
)

add_executable(wall main.cpp)
target_link_libraries(wall PRIVATE trading_engine)

find_package(GTest REQUIRED)
file(GLOB WALL_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(wall_tests ${WALL_TEST_SOURCES})
target_include_directories(wall_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(wall_tests PRIVATE trading_engine GTest::gtest)

add_executable(wall_replay src/tools/replay_main.cpp)
target_link_libraries(wall_replay PRIVATE trading_engine)

add_executable(wall_tx_producer src/tools/transaction_producer_main.cpp)
target_link_libraries(wall_tx_producer PRIVATE trading_engine)

add_executable(wall_backtest src/tools/backtest_main.cpp)
target_link_libraries(wall_backtest PRIVATE trading_engine)

add_executable(wall_benchmark src/tools/benchmark_main.cpp)
target_link_libraries(wall_benchmark PRIVATE trading_engine)

enable_testing()
include(GoogleTest)
gtest_discover_tests(wall_tests DISCOVERY_TIMEOUT 60)
