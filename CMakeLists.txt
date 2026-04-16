cmake_minimum_required(VERSION 3.20)
project(wall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka)
find_path(LIBPQ_INCLUDE_DIR
    NAMES libpq-fe.h
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

target_include_directories(trading_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${LIBPQ_INCLUDE_DIR}
)

target_compile_definitions(trading_engine
    PUBLIC
        TRADING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(trading_engine
    PRIVATE
        ${LIBPQ_LIBRARY}
        PkgConfig::HIREDIS
        PkgConfig::RDKAFKA
)

add_executable(wall main.cpp)
target_link_libraries(wall PRIVATE trading_engine)

add_executable(wall_tests tests/test_main.cpp)
target_link_libraries(wall_tests PRIVATE trading_engine)

add_executable(wall_replay src/tools/replay_main.cpp)
target_link_libraries(wall_replay PRIVATE trading_engine)

add_executable(wall_tx_producer src/tools/transaction_producer_main.cpp)
target_link_libraries(wall_tx_producer PRIVATE trading_engine)

add_executable(wall_backtest src/tools/backtest_main.cpp)
target_link_libraries(wall_backtest PRIVATE trading_engine)

add_executable(wall_benchmark src/tools/benchmark_main.cpp)
target_link_libraries(wall_benchmark PRIVATE trading_engine)

enable_testing()
add_test(NAME wall_tests COMMAND wall_tests)
