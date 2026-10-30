#include "test_support.hpp"

using namespace wall_test;

// Verifies small limit orders are acknowledged and fully filled immediately.
TEST(Execution, simulated_execution_engine_ack_and_fill) {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });

    const auto result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42000.0,
    });

    expect_equal(result.updates.size(), std::size_t {2}, "small order should produce ack and fill updates");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::acknowledged, "first update should acknowledge");
    expect_equal(result.updates[1].status, trading::core::OrderStatus::filled, "second update should fill");
    expect_equal(result.fills.size(), std::size_t {1}, "small order should produce one fill");
    expect_equal(result.fills[0].quantity, 0.25, "fill quantity should match request");
}

// Verifies the book tracks best bid and ask levels across multiple prices.
TEST(Execution, order_book_tracks_best_levels) {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42000.0,
        },
        "order-1",
        "client-1"),
        "first bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.2,
            .price = 42100.0,
        },
        "order-2",
        "client-2"),
        "second bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-3",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.3,
            .price = 42200.0,
        },
        "order-3",
        "client-3"),
        "first ask should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-4",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.6,
            .price = 42300.0,
        },
        "order-4",
        "client-4"),
        "second ask should be accepted");

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    const auto bid_levels = book.bid_levels();
    const auto ask_levels = book.ask_levels();

    expect_true(best_bid.has_value(), "best bid should exist");
    expect_true(best_ask.has_value(), "best ask should exist");
    expect_equal(best_bid->price, 42100.0, "best bid price should be highest bid");
    expect_equal(best_bid->total_quantity, 0.2, "best bid quantity should match resting quantity");
    expect_equal(best_ask->price, 42200.0, "best ask price should be lowest ask");
    expect_equal(best_ask->total_quantity, 0.3, "best ask quantity should match resting quantity");
    expect_equal(bid_levels.size(), std::size_t {2}, "two bid levels should exist");
    expect_equal(ask_levels.size(), std::size_t {2}, "two ask levels should exist");
    expect_equal(book.order_count(), std::size_t {4}, "four resting orders should exist");
}

// Verifies same-price resting orders preserve insertion order inside one level.
TEST(Execution, order_book_preserves_fifo_within_price_level) {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42000.0,
        },
        "order-1",
        "client-1"),
        "first same-price order should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.6,
            .price = 42000.0,
        },
        "order-2",
        "client-2"),
        "second same-price order should be accepted");

    const auto first_order = book.find_order("order-1");
    const auto second_order = book.find_order("order-2");
    const auto best_bid = book.best_bid();

    expect_true(first_order.has_value(), "first order should be findable");
    expect_true(second_order.has_value(), "second order should be findable");
    expect_true(first_order->sequence_number < second_order->sequence_number, "sequence number should preserve FIFO ordering");
    expect_true(best_bid.has_value(), "best bid should exist");
    expect_equal(best_bid->order_count, std::size_t {2}, "best price level should contain both same-price orders");
    expect_equal(best_bid->total_quantity, 1.0, "same-price level quantity should aggregate");
}

// Verifies cancel removes the order and prunes empty levels.
TEST(Execution, order_book_cancel_prunes_empty_level) {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42100.0,
        },
        "order-1",
        "client-1"),
        "top bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.7,
            .price = 42000.0,
        },
        "order-2",
        "client-2"),
        "second bid should be accepted");

    expect_true(book.cancel_order("order-1"), "existing top-of-book order should cancel");
    expect_true(!book.cancel_order("missing-order"), "missing order should not cancel");

    const auto best_bid = book.best_bid();
    const auto removed_order = book.find_order("order-1");
    const auto remaining_order = book.find_order("order-2");
    const auto bid_levels = book.bid_levels();

    expect_true(best_bid.has_value(), "best bid should still exist after one cancel");
    expect_equal(best_bid->price, 42000.0, "best bid should move to the next populated level");
    expect_true(!removed_order.has_value(), "canceled order should no longer be findable");
    expect_true(remaining_order.has_value(), "uncanceled order should remain");
    expect_equal(bid_levels.size(), std::size_t {1}, "empty canceled level should be pruned");
    expect_equal(book.order_count(), std::size_t {1}, "one resting order should remain after cancel");
}

// Verifies a crossing order fully matches the resting top of book.
TEST(Execution, matching_engine_fully_matches_crossing_order) {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell order");
    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    log_test_step("submit crossing buy order");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42050.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);

    log_test_step("verify full match and empty book");
    expect_equal(resting_result.updates.size(), std::size_t {1}, "non-crossing first order should only acknowledge");
    expect_equal(taker_result.updates.size(), std::size_t {2}, "crossing order should ack then fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::filled, "crossing order should fill");
    expect_equal(taker_result.fills.size(), std::size_t {1}, "crossing order should emit one fill");
    expect_equal(taker_result.fills[0].price, 42000.0, "fill price should use resting order price");
    expect_equal(taker_result.fills[0].quantity, 0.4, "fill quantity should match the resting quantity");
    expect_true(book != nullptr, "book should exist after matching");
    expect_equal(book->order_count(), std::size_t {0}, "book should be empty after full match");
}

// Verifies partial matching leaves the residual quantity resting on the book.
TEST(Execution, matching_engine_partially_matches_and_rests_residual) {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell liquidity");
    const auto ignored_resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    static_cast<void>(ignored_resting_result);
    log_test_step("submit larger crossing buy order");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after partial match");

    const auto resting_taker_order = book->find_order(taker_result.order_id);
    const auto best_bid = book->best_bid();
    const auto best_ask = book->best_ask();

    log_test_step("verify partial fill and residual resting quantity");
    expect_equal(taker_result.updates.size(), std::size_t {2}, "partially matched order should ack then partially fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::partially_filled, "terminal update should be partial");
    expect_equal(taker_result.updates[1].filled_quantity, 0.4, "filled quantity should match executed amount");
    expect_equal(taker_result.fills.size(), std::size_t {1}, "partial match should emit one fill");
    expect_true(resting_taker_order.has_value(), "residual quantity should rest on the book");
    expect_equal(resting_taker_order->remaining_quantity, 0.6, "resting residual should equal remaining quantity");
    expect_true(best_bid.has_value(), "residual should become top bid");
    expect_equal(best_bid->price, 42000.0, "resting residual price should remain on the incoming limit price");
    expect_equal(best_bid->total_quantity, 0.6, "residual best bid quantity should match remaining quantity");
    expect_true(!best_ask.has_value(), "all opposing liquidity should be consumed");
}

// Verifies a crossing order sweeps multiple resting levels in price-time order.
TEST(Execution, matching_engine_sweeps_multiple_levels_in_price_time_order) {
    trading::execution::MatchingEngine engine;

    const auto first_resting = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.2,
        .price = 42000.0,
    });
    const auto second_resting = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.3,
        .price = 42000.0,
    });
    const auto ignored_third_resting = engine.submit({
        .request_id = "req-3",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    static_cast<void>(ignored_third_resting);
    const auto taker_result = engine.submit({
        .request_id = "req-4",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.7,
        .price = 42100.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after sweep");

    expect_equal(taker_result.updates.size(), std::size_t {2}, "sweeping order should ack then fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::filled, "sweep should fully fill incoming order");
    expect_equal(taker_result.fills.size(), std::size_t {3}, "sweep should produce one fill per matched resting order");
    expect_near(taker_result.fills[0].quantity, 0.2, 1e-9, "first FIFO match should use the first same-price order");
    expect_near(taker_result.fills[1].quantity, 0.3, 1e-9, "second FIFO match should use the second same-price order");
    expect_near(taker_result.fills[2].quantity, 0.2, 1e-9, "final match should consume part of the next price level");
    expect_equal(taker_result.fills[0].price, 42000.0, "first fill price should match the best resting ask");
    expect_equal(taker_result.fills[1].price, 42000.0, "second fill price should preserve same-price FIFO");
    expect_equal(taker_result.fills[2].price, 42100.0, "third fill price should move to the next ask level");
    expect_true(!book->find_order(first_resting.order_id).has_value(), "first same-price resting order should be consumed");
    expect_true(!book->find_order(second_resting.order_id).has_value(), "second same-price resting order should be consumed");
    const auto best_ask = book->best_ask();
    expect_true(best_ask.has_value(), "partially consumed next level should remain");
    expect_equal(best_ask->price, 42100.0, "remaining ask should stay on the partially consumed level");
    expect_near(best_ask->total_quantity, 0.2, 1e-9, "remaining ask quantity should reflect the residual");
}

// Verifies non-crossing orders rest on the book without fills.
TEST(Execution, matching_engine_leaves_non_crossing_order_resting) {
    trading::execution::MatchingEngine engine;

    const auto ignored_resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    static_cast<void>(ignored_resting_result);
    const auto result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after resting order");

    expect_equal(result.updates.size(), std::size_t {1}, "non-crossing order should only acknowledge");
    expect_equal(result.fills.size(), std::size_t {0}, "non-crossing order should not fill");
    expect_true(book->find_order(result.order_id).has_value(), "non-crossing order should rest on the book");
    expect_equal(book->order_count(), std::size_t {2}, "both sides should remain resting");
}

// Verifies cancel removes a fully resting order and returns the expected lifecycle updates.
TEST(Execution, matching_engine_cancels_resting_order) {
    trading::execution::MatchingEngine engine;

    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    const auto cancel_result = engine.cancel(resting_result.order_id, resting_result.client_order_id);

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    const auto order = engine.get_order(resting_result.order_id);

    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel should produce pending-cancel and canceled updates");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::pending_cancel, "first cancel update should be pending_cancel");
    expect_equal(cancel_result.updates[1].status, trading::core::OrderStatus::canceled, "second cancel update should be canceled");
    expect_true(book != nullptr, "book should still exist");
    expect_equal(book->order_count(), std::size_t {0}, "canceled resting order should be removed from the book");
    expect_true(order.has_value(), "canceled order should remain queryable");
    expect_equal(order->status, trading::core::OrderStatus::canceled, "tracked order state should become canceled");
    expect_equal(order->filled_quantity, 0.0, "canceled resting order should preserve zero filled quantity");
}

// Verifies a partially filled resting order can be canceled without losing cumulative fill state.
TEST(Execution, matching_engine_cancels_partially_filled_resting_order) {
    trading::execution::MatchingEngine engine;

    log_test_step("submit maker sell order");
    const auto maker_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    log_test_step("submit taker buy order for partial maker fill");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    log_test_step("cancel remaining maker quantity");
    const auto cancel_result = engine.cancel(maker_result.order_id, maker_result.client_order_id);

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    const auto maker_order = engine.get_order(maker_result.order_id);
    const auto taker_order = engine.get_order(taker_result.order_id);

    log_test_step("verify cancel preserves cumulative maker fill state");
    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel after partial fill should still produce two cancel updates");
    expect_equal(cancel_result.updates[0].filled_quantity, 0.4, "pending cancel should preserve cumulative filled quantity");
    expect_equal(cancel_result.updates[1].filled_quantity, 0.4, "canceled update should preserve cumulative filled quantity");
    expect_true(book != nullptr, "book should still exist");
    expect_equal(book->order_count(), std::size_t {0}, "residual resting quantity should be removed after cancel");
    expect_true(maker_order.has_value(), "maker order should remain tracked");
    expect_equal(maker_order->status, trading::core::OrderStatus::canceled, "partially filled maker should become canceled");
    expect_equal(maker_order->filled_quantity, 0.4, "maker order should retain its cumulative filled quantity");
    expect_true(taker_order.has_value(), "taker order should remain tracked");
    expect_equal(taker_order->status, trading::core::OrderStatus::filled, "taker order should remain terminal filled");
}

// Verifies internal tracked order state reflects matching outcomes for both maker and taker orders.
TEST(Execution, matching_engine_tracks_order_lifecycle_state) {
    trading::execution::MatchingEngine engine;

    const auto maker_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42010.0,
    });

    const auto maker_order = engine.get_order(maker_result.order_id);
    const auto taker_order = engine.get_order(taker_result.order_id);

    expect_true(maker_order.has_value(), "maker order should be queryable");
    expect_true(taker_order.has_value(), "taker order should be queryable");
    expect_equal(maker_order->status, trading::core::OrderStatus::partially_filled, "maker order should become partially filled after being hit");
    expect_equal(maker_order->filled_quantity, 0.4, "maker cumulative filled quantity should track matched volume");
    expect_equal(taker_order->status, trading::core::OrderStatus::filled, "crossing taker order should become filled");
    expect_equal(taker_order->filled_quantity, 0.4, "taker cumulative filled quantity should match executed volume");
}

// Verifies invalid matching-engine requests are rejected cleanly.
TEST(Execution, matching_engine_rejects_invalid_limit_requests) {
    trading::execution::MatchingEngine engine;

    log_test_step("submit zero-quantity limit order");
    const auto zero_quantity_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.0,
        .price = 42000.0,
    });
    log_test_step("submit zero-price limit order");
    const auto zero_price_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.1,
        .price = 0.0,
    });

    log_test_step("verify rejected lifecycle state for invalid orders");
    expect_equal(zero_quantity_result.updates.size(), std::size_t {1}, "zero-quantity order should reject immediately");
    expect_equal(zero_quantity_result.updates[0].status, trading::core::OrderStatus::rejected, "zero-quantity order should be rejected");
    expect_equal(zero_price_result.updates.size(), std::size_t {1}, "zero-price order should reject immediately");
    expect_equal(zero_price_result.updates[0].status, trading::core::OrderStatus::rejected, "zero-price order should be rejected");
    const auto zero_quantity_order = engine.get_order(zero_quantity_result.order_id);
    const auto zero_price_order = engine.get_order(zero_price_result.order_id);
    expect_true(zero_quantity_order.has_value(), "rejected zero-quantity order should remain queryable");
    expect_true(zero_price_order.has_value(), "rejected zero-price order should remain queryable");
    expect_equal(zero_quantity_order->status, trading::core::OrderStatus::rejected, "tracked zero-quantity order should remain rejected");
    expect_equal(zero_price_order->status, trading::core::OrderStatus::rejected, "tracked zero-price order should remain rejected");
}

// Verifies terminal orders cannot be canceled after they are fully filled.
TEST(Execution, matching_engine_rejects_cancel_for_terminal_order) {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell then fully crossing buy order");
    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    static_cast<void>(taker_result);

    log_test_step("attempt cancel on terminal filled maker order");
    const auto cancel_result = engine.cancel(resting_result.order_id, resting_result.client_order_id);

    log_test_step("verify cancel rejection preserves filled quantity");
    expect_equal(cancel_result.updates.size(), std::size_t {1}, "terminal cancel should return one rejection update");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::rejected, "terminal order cancel should reject");
    expect_true(cancel_result.updates[0].reason.has_value(), "terminal cancel rejection should include a reason");
    expect_equal(*cancel_result.updates[0].reason, std::string("order is not cancelable"), "terminal cancel reason should match");
    expect_equal(cancel_result.updates[0].filled_quantity, 0.4, "terminal cancel rejection should preserve cumulative filled quantity");
}

// Verifies invalid open-order recovery input does not leak into tracked state or the book.
TEST(Execution, matching_engine_restore_open_order_skips_invalid_state) {
    trading::execution::MatchingEngine engine;

    log_test_step("attempt restore with fully filled quantity");
    engine.restore_open_order(
        "restored-order-1",
        "restored-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        0.5);
    log_test_step("attempt restore with invalid price");
    engine.restore_open_order(
        "restored-order-2",
        "restored-client-2",
        {
            .request_id = "req-restore-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 0.0,
        },
        0.0);

    log_test_step("verify invalid restore attempts do not populate tracked order state");
    expect_true(!engine.get_order("restored-order-1").has_value(), "fully filled restore input should be ignored");
    expect_true(!engine.get_order("restored-order-2").has_value(), "invalid-price restore input should be ignored");
    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book == nullptr || book->order_count() == 0, "invalid restore attempts should not leave resting orders");
}

// Verifies market book snapshots can fill resting local orders from external ask liquidity.
TEST(Execution, matching_engine_processes_market_ask_levels_against_resting_bids) {
    trading::execution::MatchingEngine engine;

    log_test_step("restore resting local buy order");
    engine.restore_open_order(
        "resting-buy-1",
        "resting-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        0.0);

    log_test_step("apply external ask-side book snapshot");
    const auto result = engine.process_market_event({
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
            {.price = 42000.0, .quantity = 0.3},
        },
        .process_timestamp = 1000,
    });

    log_test_step("verify external ask liquidity partially fills the resting buy order");
    expect_equal(result.fills.size(), std::size_t {2}, "two executable ask levels should produce two fills");
    expect_equal(result.fills[0].order_id, std::string("resting-buy-1"), "fills should be attributed to the resting local order");
    expect_equal(result.fills[0].side, trading::core::OrderSide::buy, "resting local buy should emit buy-side fill");
    expect_equal(result.fills[0].price, 41990.0, "first fill should use first ask level price");
    expect_equal(result.fills[1].price, 42000.0, "second fill should use second ask level price");
    expect_near(result.fills[0].quantity, 0.4, 1e-9, "first fill quantity should match first ask level quantity");
    expect_near(result.fills[1].quantity, 0.3, 1e-9, "second fill quantity should match second ask level quantity");
    expect_equal(result.updates.size(), std::size_t {1}, "one resting order update should be emitted");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::partially_filled, "resting buy order should become partially filled");
    expect_near(result.updates[0].filled_quantity, 0.7, 1e-9, "cumulative filled quantity should aggregate across ask levels");
    const auto order = engine.get_order("resting-buy-1");
    expect_true(order.has_value(), "resting order should remain tracked after market-driven partial fill");
    expect_equal(order->status, trading::core::OrderStatus::partially_filled, "tracked order state should reflect market-driven partial fill");
    expect_near(order->filled_quantity, 0.7, 1e-9, "tracked filled quantity should aggregate market-driven fills");
}

// Verifies market book snapshots can fill resting local sell orders from external bid liquidity.
TEST(Execution, matching_engine_processes_market_bid_levels_against_resting_asks) {
    trading::execution::MatchingEngine engine;

    log_test_step("restore resting local sell order");
    engine.restore_open_order(
        "resting-sell-1",
        "resting-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42010.0,
        },
        0.0);

    log_test_step("apply external bid-side book snapshot");
    const auto result = engine.process_market_event({
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .bid_levels = {
            {.price = 42020.0, .quantity = 0.5},
        },
        .process_timestamp = 1000,
    });

    log_test_step("verify external bid liquidity fully fills the resting sell order");
    expect_equal(result.fills.size(), std::size_t {1}, "one executable bid level should produce one fill");
    expect_equal(result.fills[0].order_id, std::string("resting-sell-1"), "fill should be attributed to the resting local sell order");
    expect_equal(result.fills[0].side, trading::core::OrderSide::sell, "resting local sell should emit sell-side fill");
    expect_equal(result.fills[0].price, 42020.0, "fill should use external bid level price");
    expect_near(result.fills[0].quantity, 0.5, 1e-9, "fill quantity should match remaining resting sell quantity");
    expect_equal(result.updates.size(), std::size_t {1}, "one resting sell update should be emitted");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::filled, "resting sell order should become filled");
    const auto order = engine.get_order("resting-sell-1");
    expect_true(order.has_value(), "filled resting sell should remain tracked");
    expect_equal(order->status, trading::core::OrderStatus::filled, "tracked resting sell should become filled");
}

// Verifies unsupported market orders are rejected by the simulator.
TEST(Execution, simulated_execution_engine_rejects_market_order) {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });

    const auto result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::market,
        .quantity = 0.25,
        .price = std::nullopt,
    });

    expect_equal(result.updates.size(), std::size_t {1}, "market order should be rejected immediately");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::rejected, "market order should be rejected");
    expect_equal(*result.updates[0].reason, std::string("market orders are not supported by the simulator"), "market-order rejection reason should match");
}

// Verifies larger orders can be partially filled and later completed.
TEST(Execution, simulated_execution_engine_partial_then_full_fill) {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 0.50,
        .partial_fill_ratio = 0.40,
    });

    const auto initial_result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto completion_result = execution_engine.complete_open_order(initial_result.order_id);

    expect_equal(initial_result.updates.size(), std::size_t {2}, "partial order should produce ack and partial-fill updates");
    expect_equal(initial_result.updates[1].status, trading::core::OrderStatus::partially_filled, "second update should be partial fill");
    expect_equal(initial_result.fills[0].quantity, 0.4, "partial fill quantity should respect the configured ratio");
    expect_equal(completion_result.updates.size(), std::size_t {1}, "completion should produce one terminal update");
    expect_equal(completion_result.updates[0].status, trading::core::OrderStatus::filled, "completion should fill the order");
    expect_equal(completion_result.fills[0].quantity, 0.6, "completion fill should use the remaining quantity");
}

// Verifies open orders can be canceled after a partial fill.
TEST(Execution, simulated_execution_engine_cancel_open_order) {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 0.50,
        .partial_fill_ratio = 0.50,
    });

    const auto initial_result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto cancel_result = execution_engine.cancel_open_order(initial_result.order_id);

    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel should produce pending-cancel and canceled updates");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::pending_cancel, "first cancel update should be pending_cancel");
    expect_equal(cancel_result.updates[1].status, trading::core::OrderStatus::canceled, "second cancel update should be canceled");
}

// Verifies live execution reports drive a full local order lifecycle and fill generation.
TEST(Execution, live_execution_tracker_applies_full_lifecycle) {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto ack_result = tracker.apply_report({
        .report_id = "report-ack-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::acknowledged,
        .cumulative_filled_quantity = 0.0,
        .exchange_timestamp = 1000,
    });
    const auto partial_result = tracker.apply_report({
        .report_id = "report-fill-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.4,
        .last_fill_price = 42000.0,
        .last_fill_fee = 1.0,
        .exchange_timestamp = 1010,
    });
    const auto final_result = tracker.apply_report({
        .report_id = "report-fill-2",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::filled,
        .cumulative_filled_quantity = 1.0,
        .last_fill_price = 42010.0,
        .last_fill_fee = 1.5,
        .exchange_timestamp = 1020,
    });

    expect_true(ack_result.applied, "acknowledgement report should apply");
    expect_equal(ack_result.updates.size(), std::size_t {1}, "acknowledgement should emit one order update");
    expect_equal(ack_result.updates[0].status, trading::core::OrderStatus::acknowledged, "acknowledgement status should match");

    expect_true(partial_result.applied, "partial fill report should apply");
    expect_equal(partial_result.updates[0].status, trading::core::OrderStatus::partially_filled, "partial status should match");
    expect_equal(partial_result.fills.size(), std::size_t {1}, "partial fill should emit one fill");
    expect_equal(partial_result.fills[0].quantity, 0.4, "partial fill delta should match cumulative change");

    expect_true(final_result.applied, "final fill report should apply");
    expect_equal(final_result.updates[0].status, trading::core::OrderStatus::filled, "final status should match");
    expect_equal(final_result.fills.size(), std::size_t {1}, "final fill should emit one fill");
    expect_equal(final_result.fills[0].quantity, 0.6, "final fill delta should be remaining quantity");

    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked live order should exist");
    expect_equal(order->status, trading::core::OrderStatus::filled, "tracked live order should reach filled");
    expect_equal(order->filled_quantity, 1.0, "tracked filled quantity should match final cumulative quantity");
}

// Verifies duplicate execution reports are ignored idempotently.
TEST(Execution, live_execution_tracker_ignores_duplicate_reports) {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto first_result = tracker.apply_report({
        .report_id = "report-dup-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.5,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 1000,
    });
    const auto duplicate_result = tracker.apply_report({
        .report_id = "report-dup-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.5,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 1000,
    });

    expect_true(first_result.applied, "first report should apply");
    expect_true(duplicate_result.ignored_duplicate, "duplicate report should be ignored");
    expect_true(!duplicate_result.applied, "duplicate report should not apply twice");

    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked order should still exist");
    expect_equal(order->filled_quantity, 0.5, "duplicate report should not change filled quantity");
}

// Verifies late execution reports do not roll terminal local state backward.
TEST(Execution, live_execution_tracker_ignores_late_terminal_regressions) {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto final_apply = tracker.apply_report({
        .report_id = "report-final-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::filled,
        .cumulative_filled_quantity = 1.0,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 2000,
    });
    expect_true(final_apply.applied, "terminal fill report should apply before late regression test");

    const auto late_result = tracker.apply_report({
        .report_id = "report-late-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.4,
        .last_fill_price = 41990.0,
        .exchange_timestamp = 1500,
    });

    expect_true(late_result.ignored_stale, "late regressive report should be ignored");
    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked order should still exist");
    expect_equal(order->status, trading::core::OrderStatus::filled, "late report should not roll terminal state backward");
    expect_equal(order->filled_quantity, 1.0, "late report should not reduce filled quantity");
}
