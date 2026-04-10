cmake_minimum_required(VERSION 3.20)
project(wall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(trading_engine
    src/app/application.cpp
    src/app/engine_controller.cpp
    src/app/mock_event_sources.cpp
    src/app/simulation_runtime.cpp
    src/config/config_loader.cpp
    src/exchange/exchange_execution_adapter.cpp
    src/exchange/exchange_market_data_adapter.cpp
    src/execution/simulated_execution_engine.cpp
    src/infrastructure/postgres_repositories.cpp
    src/infrastructure/kafka_transaction_consumer.cpp
    src/infrastructure/redis_cache.cpp
    src/ingestion/transaction_consumer.cpp
    src/ingestion/transaction_ingestor.cpp
    src/market_data/market_state_store.cpp
    src/monitoring/health_status.cpp
    src/monitoring/logger.cpp
    src/portfolio/portfolio_service.cpp
    src/risk/risk_engine.cpp
    src/storage/replay_service.cpp
    src/storage/storage_interfaces.cpp
    src/strategy/sample_threshold_strategy.cpp
)

target_include_directories(trading_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(trading_engine
    PUBLIC
        TRADING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(wall main.cpp)
target_link_libraries(wall PRIVATE trading_engine)

add_executable(wall_tests tests/test_main.cpp)
target_link_libraries(wall_tests PRIVATE trading_engine)

add_executable(wall_replay src/tools/replay_main.cpp)
target_link_libraries(wall_replay PRIVATE trading_engine)

enable_testing()
add_test(NAME wall_tests COMMAND wall_tests)
