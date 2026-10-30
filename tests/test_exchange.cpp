#include "test_support.hpp"

using namespace wall_test;

// Verifies stub exchange adapters satisfy the market/execution interface contracts.
TEST(Exchange, exchange_adapter_stub_integration) {
    const auto instrument = make_btc_instrument();

    trading::exchange::MockExchangeMarketDataAdapter market_adapter(instrument);
    trading::exchange::IExchangeMarketDataAdapter& market_interface = market_adapter;
    const auto market_result = market_interface.normalize(
        "type=trade;event_id=fixture-1;symbol=BTCUSDT;price=42010.5;quantity=0.75;bid=42010.0;ask=42011.0;exchange_ts=123456",
        123500,
        123501);
    const auto* event = std::get_if<trading::core::MarketEvent>(&market_result);
    expect_true(event != nullptr, "market adapter should normalize fixture payload");
    expect_equal(event->event_id, std::string("fixture-1"), "event id should be parsed");
    expect_equal(event->instrument.instrument_id, instrument.instrument_id, "normalized event should keep configured instrument");

    trading::exchange::SimulatedExchangeExecutionAdapter execution_adapter({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });
    trading::exchange::IExchangeExecutionAdapter& execution_interface = execution_adapter;
    const auto submit_result = execution_interface.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(submit_result.updates.size(), std::size_t {2}, "execution adapter should proxy simulator lifecycle events");
    expect_equal(submit_result.updates[0].status, trading::core::OrderStatus::acknowledged, "first update should acknowledge");

    trading::exchange::MockLiveExchangeExecutionAdapter live_execution_adapter;
    const auto live_submit = live_execution_adapter.submit({
        .request_id = "req-live-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(live_submit.updates.size(), std::size_t {1}, "live adapter submit should produce one pending update");
    expect_equal(live_submit.updates[0].status, trading::core::OrderStatus::pending_submit, "live adapter should enter pending_submit before exchange ack");

    const auto reconciliation = live_execution_adapter.apply_exchange_report({
        .report_id = "live-report-1",
        .order_id = live_submit.order_id,
        .client_order_id = live_submit.client_order_id,
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::acknowledged,
        .cumulative_filled_quantity = 0.0,
        .exchange_timestamp = 1000,
    });
    expect_true(reconciliation.applied, "live adapter should reconcile exchange acknowledgements");
}

// Verifies market-data and execution routing resolve adapters by exchange name.
TEST(Exchange, exchange_router_routes_multi_exchange_requests) {
    trading::exchange::ExchangeMarketDataRouter market_router;
    market_router.register_adapter(
        "binance",
        std::make_unique<trading::exchange::MockExchangeMarketDataAdapter>(make_btc_instrument()));
    market_router.register_adapter(
        "coinbase",
        std::make_unique<trading::exchange::MockExchangeMarketDataAdapter>(make_coinbase_eth_instrument()));

    const auto binance_event = market_router.normalize(
        "binance",
        "type=trade;event_id=fixture-1;symbol=BTCUSDT;price=42010.5;quantity=0.75;exchange_ts=123456",
        123500,
        123501);
    const auto* normalized_binance_event = std::get_if<trading::core::MarketEvent>(&binance_event);
    expect_true(normalized_binance_event != nullptr, "registered exchange should normalize market payload");
    expect_equal(normalized_binance_event->instrument.exchange, std::string("binance"), "normalized event should keep binance identity");

    const auto missing_market = market_router.normalize("kraken", "type=trade", 1, 1);
    const auto* missing_market_error = std::get_if<trading::exchange::ExchangeAdapterError>(&missing_market);
    expect_true(missing_market_error != nullptr, "unknown market exchange should return an adapter error");

    trading::exchange::ExchangeExecutionRouter execution_router;
    execution_router.register_adapter(
        "binance",
        std::make_unique<trading::exchange::SimulatedExchangeExecutionAdapter>(
            trading::execution::SimulatedExecutionConfig {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            }));

    const auto routed_submit = execution_router.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(routed_submit.updates.size(), std::size_t {2}, "registered execution adapter should receive routed submit");

    const auto missing_submit = execution_router.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-2",
        .instrument = make_coinbase_eth_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 3000.0,
    });
    expect_equal(missing_submit.updates.size(), std::size_t {1}, "missing execution route should return one rejection update");
    expect_equal(missing_submit.updates[0].status, trading::core::OrderStatus::rejected, "missing execution route should reject");

    const auto capabilities = execution_router.capabilities("binance");
    expect_true(capabilities.has_value(), "registered execution route should expose capabilities");
    expect_true(capabilities->supports_cancel, "simulated execution route should support cancel");
}

// Verifies payload normalization keeps internal market-event semantics stable.
TEST(Exchange, exchange_market_payload_normalization_contract) {
    trading::exchange::MockExchangeMarketDataAdapter market_adapter(make_btc_instrument());

    const auto result = market_adapter.normalize(
        "type=ticker;event_id=tick-1;symbol=BTCUSDT;bid=42001.0;ask=42003.0;exchange_ts=777",
        800,
        900);
    const auto* event = std::get_if<trading::core::MarketEvent>(&result);
    expect_true(event != nullptr, "ticker payload should normalize");
    expect_equal(event->type, trading::core::MarketEventType::ticker, "event type should map from payload type");
    expect_true(event->bid_price.has_value(), "ticker should carry best bid");
    expect_true(event->ask_price.has_value(), "ticker should carry best ask");
    expect_equal(*event->bid_price, 42001.0, "best bid should parse");
    expect_equal(*event->ask_price, 42003.0, "best ask should parse");
    expect_equal(event->exchange_timestamp, std::int64_t {777}, "exchange timestamp should parse");
    expect_equal(event->receive_timestamp, std::int64_t {800}, "receive timestamp should be adapter input");
    expect_equal(event->process_timestamp, std::int64_t {900}, "process timestamp should be adapter input");
}

// Verifies exchange-specific reasons map to stable internal adapter error codes.
TEST(Exchange, exchange_error_mapping_behavior) {
    expect_equal(
        trading::exchange::map_exchange_error("insufficient margin for order"),
        trading::exchange::ExchangeAdapterErrorCode::rejected_by_exchange,
        "insufficient margin should map to rejected_by_exchange");
    expect_equal(
        trading::exchange::map_exchange_error("connection timeout while submitting"),
        trading::exchange::ExchangeAdapterErrorCode::transport_failure,
        "connection timeout should map to transport_failure");
    expect_equal(
        trading::exchange::map_exchange_error("unsupported order type"),
        trading::exchange::ExchangeAdapterErrorCode::unsupported_message_type,
        "unsupported type should map to unsupported_message_type");
}
