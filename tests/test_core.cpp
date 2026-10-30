#include "test_support.hpp"

using namespace wall_test;

// Verifies the core model fixtures can be constructed with stable defaults.
TEST(Core, core_models) {
    const auto instrument = make_btc_instrument();
    const trading::core::OrderRequest request {
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    };

    // Step 1: Verify the instrument fixture survived construction.
    expect_equal(instrument.symbol, std::string("BTCUSDT"), "instrument symbol should match");

    // Step 2: Verify the request fields hold the expected values.
    expect_equal(request.request_id, std::string("req-1"), "request id should match");
    expect_true(request.price.has_value(), "limit order price should exist");
}

// Verifies event handlers receive events in the same order they were subscribed.
TEST(Core, event_dispatcher_order) {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> calls;

    // Step 1: Register two handlers in a known order.
    dispatcher.subscribe([&calls](const trading::core::EngineEvent&) { calls.push_back("first"); });
    dispatcher.subscribe([&calls](const trading::core::EngineEvent&) { calls.push_back("second"); });

    // Step 2: Publish a timer event into the dispatcher.
    dispatcher.publish(trading::core::TimerEvent {.timer_id = "t-1", .timestamp = 1000});

    // Step 3: Assert the handlers ran in registration order.
    expect_equal(calls.size(), std::size_t {2}, "two handlers should run");
    expect_equal(calls[0], std::string("first"), "first handler should run first");
    expect_equal(calls[1], std::string("second"), "second handler should run second");
}

// Verifies the controller drains queued events in FIFO order.
TEST(Core, engine_controller_queue_order) {
    trading::core::FixedClock clock(1000);
    trading::app::EngineController controller(clock);
    std::vector<std::string> calls;

    controller.subscribe([&calls](const trading::core::EngineEvent& event) {
        std::visit([&calls](const auto& concrete_event) {
            using EventType = std::decay_t<decltype(concrete_event)>;

            if constexpr (std::is_same_v<EventType, trading::core::TimerEvent>) {
                calls.push_back("timer:" + concrete_event.timer_id);
            } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
                calls.push_back("tx:" + concrete_event.transaction_id);
            } else if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
                calls.push_back("market:" + concrete_event.event_id);
            }
        }, event);
    });

    controller.enqueue(trading::core::TimerEvent {.timer_id = "first", .timestamp = 1000});
    controller.enqueue(make_transaction("tx-1", 10));
    controller.enqueue(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 42010.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    expect_equal(controller.drain(), std::size_t {3}, "three events should be drained");
    expect_equal(calls.size(), std::size_t {3}, "three events should be observed");
    expect_equal(calls[0], std::string("timer:first"), "timer should be first");
    expect_equal(calls[1], std::string("tx:tx-1"), "transaction should be second");
    expect_equal(calls[2], std::string("market:market-1"), "market event should be third");
}

// Verifies due timers are enqueued once in timer registration order.
TEST(Core, engine_controller_due_timers) {
    trading::core::FixedClock clock(5000);
    trading::app::EngineController controller(clock);
    std::vector<std::string> timer_ids;

    controller.subscribe([&timer_ids](const trading::core::EngineEvent& event) {
        if (const auto* timer = std::get_if<trading::core::TimerEvent>(&event)) {
            timer_ids.push_back(timer->timer_id);
        }
    });

    controller.schedule_timer("early", 4000);
    controller.schedule_timer("late", 5000);

    expect_equal(controller.enqueue_due_timers(), std::size_t {2}, "two timers should be due");
    expect_equal(controller.drain(), std::size_t {2}, "two timer events should be drained");
    expect_equal(timer_ids[0], std::string("early"), "first due timer should preserve registration order");
    expect_equal(timer_ids[1], std::string("late"), "second due timer should preserve registration order");
    expect_equal(controller.enqueue_due_timers(), std::size_t {0}, "timers should not be emitted twice");
}

// Verifies source polling is deterministic and safe when handlers are absent.
TEST(Core, engine_controller_mock_sources_and_empty_dispatch) {
    trading::core::FixedClock clock(1000);
    trading::app::EngineController controller(clock);

    trading::app::MockMarketDataSource market_source({
        trading::core::MarketEvent {
            .event_id = "market-1",
            .type = trading::core::MarketEventType::trade,
            .instrument = make_btc_instrument(),
            .price = 42010.0,
            .quantity = 0.25,
            .process_timestamp = 1000,
        },
    });
    trading::app::MockTransactionEventSource transaction_source({
        make_transaction("tx-1", 10),
    });

    controller.add_source(market_source);
    controller.add_source(transaction_source);

    expect_equal(controller.poll_sources_once(), std::size_t {2}, "two source events should be polled");
    expect_equal(controller.queued_event_count(), std::size_t {2}, "two source events should be queued");
    expect_equal(controller.drain(), std::size_t {2}, "drain should be safe without subscribers");
    expect_equal(controller.poll_sources_once(), std::size_t {0}, "sources should be exhausted");
}
