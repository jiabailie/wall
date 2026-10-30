#include "test_support.hpp"

using namespace wall_test;

// Verifies market state updates and stale detection use deterministic timestamps.
TEST(MarketData, market_state_store) {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);

    const auto instrument = make_btc_instrument();
    const trading::core::MarketEvent event {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42010.0,
        .quantity = 0.5,
        .bid_price = 42000.0,
        .ask_price = 42020.0,
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 4500,
    };

    // Step 1: Apply the market event to the state store.
    store.apply(event);

    // Step 2: Read the stored state and verify the updated values.
    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist");
    expect_true(state->best_bid.has_value(), "best bid should exist");
    expect_equal(*state->best_bid, 42000.0, "best bid should match");
    expect_equal(*state->last_trade_price, 42010.0, "last trade price should match");
    expect_equal(state->last_process_timestamp, std::int64_t {4500}, "last process timestamp should match");

    // Step 3: Verify stale checks respect the fixed clock.
    expect_true(!store.is_stale(instrument.instrument_id, 1000), "state should not be stale yet");
    clock.set_now_ms(7001);
    expect_true(store.is_stale(instrument.instrument_id, 2000), "state should become stale");
}

// Verifies older market events cannot overwrite newer stored state.
TEST(MarketData, market_state_store_ignores_out_of_order_events) {
    trading::core::FixedClock clock(9000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "newer",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 43000.0,
        .quantity = 1.0,
        .bid_price = 42990.0,
        .ask_price = 43010.0,
        .exchange_timestamp = 5000,
        .receive_timestamp = 5100,
        .process_timestamp = 5200,
    });
    store.apply(trading::core::MarketEvent {
        .event_id = "older",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41000.0,
        .quantity = 2.0,
        .bid_price = 40990.0,
        .ask_price = 41010.0,
        .exchange_timestamp = 4000,
        .receive_timestamp = 4100,
        .process_timestamp = 4200,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist after out-of-order update");
    expect_equal(*state->best_bid, 42990.0, "older event should not overwrite best bid");
    expect_equal(*state->last_trade_price, 43000.0, "older event should not overwrite trade price");
    expect_equal(state->last_process_timestamp, std::int64_t {5200}, "newer process timestamp should remain");
}

// Verifies quote-only updates preserve the last known trade state.
TEST(MarketData, market_state_store_quote_update_preserves_trade_state) {
    trading::core::FixedClock clock(6000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "trade",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.8,
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 1200,
    });
    store.apply(trading::core::MarketEvent {
        .event_id = "quote",
        .type = trading::core::MarketEventType::ticker,
        .instrument = instrument,
        .bid_price = 42040.0,
        .ask_price = 42060.0,
        .exchange_timestamp = 1300,
        .receive_timestamp = 1400,
        .process_timestamp = 1500,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist after quote update");
    expect_equal(*state->best_bid, 42040.0, "best bid should update from quote");
    expect_equal(*state->last_trade_price, 42050.0, "quote update should preserve prior trade price");
    expect_equal(*state->last_trade_quantity, 0.8, "quote update should preserve prior trade quantity");
}

// Verifies book snapshots populate richer depth state and best prices.
TEST(MarketData, market_state_store_tracks_book_depth) {
    trading::core::FixedClock clock(6000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = instrument,
        .bid_levels = {
            {.price = 42000.0, .quantity = 1.2},
            {.price = 41999.5, .quantity = 2.0},
        },
        .ask_levels = {
            {.price = 42001.0, .quantity = 1.4},
            {.price = 42002.0, .quantity = 2.2},
        },
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 1200,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "book state should exist");
    expect_equal(state->bid_levels.size(), std::size_t {2}, "bid depth should be tracked");
    expect_equal(state->ask_levels.size(), std::size_t {2}, "ask depth should be tracked");
    expect_equal(*state->best_bid, 42000.0, "best bid should follow top depth level");
    expect_equal(*state->best_ask, 42001.0, "best ask should follow top depth level");
}

// Verifies feed health degrades on stale data and disconnects beyond pure timestamp checks.
TEST(MarketData, feed_health_tracker_detects_stale_and_disconnect_states) {
    trading::core::FixedClock clock(1000);
    trading::market_data::FeedHealthTracker tracker(clock);

    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::unknown,
        "brand-new tracker should be unknown");

    tracker.on_connect();
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "connected but unsubscribed feed should be degraded");

    tracker.on_subscribe_success();
    tracker.on_message(1000);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::healthy,
        "fresh subscribed feed should be healthy");

    clock.set_now_ms(1700);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "stale feed should degrade before disconnect");

    tracker.on_disconnect();
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "recent disconnect should degrade first");

    clock.set_now_ms(2001);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::unavailable,
        "extended disconnect should become unavailable");
}

// Verifies the live feed controller reconnects and resubscribes after disconnects.
TEST(MarketData, live_market_data_feed_controller_reconnects_and_resubscribes) {
    trading::exchange::MockMarketDataFeedSession session;
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::exchange::LiveMarketDataFeedController controller(
        session,
        {"BTCUSDT", "ETHUSDT"},
        2,
        &metrics);

    controller.start();
    expect_true(controller.connected(), "controller should be connected after start");
    expect_equal(session.connect_calls(), std::size_t {1}, "start should connect once");
    expect_equal(session.subscribe_calls(), std::size_t {1}, "start should subscribe once");
    expect_equal(session.last_symbols().size(), std::size_t {2}, "subscription symbols should be forwarded");

    controller.handle_disconnect();
    expect_true(controller.connected(), "controller should reconnect after disconnect");
    expect_equal(controller.reconnect_attempts(), std::size_t {1}, "disconnect should increment reconnect attempts");
    expect_equal(session.disconnect_calls(), std::size_t {1}, "disconnect should be forwarded");
    expect_equal(session.connect_calls(), std::size_t {2}, "reconnect should call connect again");
    expect_equal(session.subscribe_calls(), std::size_t {2}, "reconnect should resubscribe");
    expect_equal(metrics.counter("market_data_disconnects"), std::int64_t {1}, "disconnect metric should increment");
    expect_equal(metrics.counter("market_data_reconnects"), std::int64_t {1}, "reconnect metric should increment");
}

// Verifies the live feed controller stops after exhausting reconnect attempts.
TEST(MarketData, live_market_data_feed_controller_stops_after_retry_budget) {
    trading::exchange::MockMarketDataFeedSession session;
    trading::exchange::LiveMarketDataFeedController controller(
        session,
        {"BTCUSDT"},
        0);

    controller.start();

    bool threw = false;
    try {
        controller.handle_disconnect();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    expect_true(threw, "disconnect beyond retry budget should throw");
}
