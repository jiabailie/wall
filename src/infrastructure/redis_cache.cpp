#include "infrastructure/redis_cache.hpp"

#include <hiredis/hiredis.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace trading::infrastructure {

namespace {

std::string encode_optional_double(const std::optional<double>& value) {
    return value.has_value() ? std::to_string(*value) : "";
}

std::optional<double> decode_optional_double(const redisReply* reply) {
    if (reply == nullptr || reply->type == REDIS_REPLY_NIL || reply->str == nullptr || reply->str[0] == '\0') {
        return std::nullopt;
    }

    return std::stod(reply->str);
}

std::string transaction_status_key(const std::string& transaction_id) {
    return "wall:transaction_status:" + transaction_id;
}

std::string market_snapshot_key(const std::string& instrument_id) {
    return "wall:market_snapshot:" + instrument_id;
}

void ensure_reply_ok(redisContext* context, redisReply* reply, const std::string& command_name) {
    if (reply == nullptr) {
        throw std::runtime_error("Redis command failed for " + command_name + ": " + context->errstr);
    }

    if (reply->type == REDIS_REPLY_ERROR) {
        const std::string message = reply->str == nullptr ? "unknown redis error" : reply->str;
        freeReplyObject(reply);
        throw std::runtime_error("Redis command failed for " + command_name + ": " + message);
    }
}

}  // namespace

class RedisConnection {
public:
    explicit RedisConnection(const trading::config::RedisConfig& config) {
        context_ = redisConnect(config.host.c_str(), config.port);
        if (context_ == nullptr || context_->err != 0) {
            const std::string message = context_ == nullptr ? "null connection" : context_->errstr;
            if (context_ != nullptr) {
                redisFree(context_);
                context_ = nullptr;
            }
            throw std::runtime_error("failed to connect to Redis: " + message);
        }
    }

    ~RedisConnection() {
        if (context_ != nullptr) {
            redisFree(context_);
        }
    }

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    [[nodiscard]] redisContext* context() const { return context_; }

private:
    redisContext* context_ {nullptr};
};

// Stores or replaces one transaction status in the cache.
void RedisTransactionCache::set_status(const std::string& transaction_id, const std::string& status) {
    statuses_[transaction_id] = status;
}

// Returns the cached status for the provided transaction id.
std::string RedisTransactionCache::get_status(const std::string& transaction_id) const {
    if (const auto iterator = statuses_.find(transaction_id); iterator != statuses_.end()) {
        return iterator->second;
    }

    return "";
}

// Stores or replaces one market snapshot in the cache.
void RedisMarketStateCache::upsert_snapshot(const trading::storage::MarketSnapshot& snapshot) {
    snapshots_[snapshot.instrument_id] = snapshot;
}

// Returns the cached market snapshot for the provided instrument id.
std::optional<trading::storage::MarketSnapshot> RedisMarketStateCache::get_snapshot(const std::string& instrument_id) const {
    if (const auto iterator = snapshots_.find(instrument_id); iterator != snapshots_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

HiredisTransactionCache::HiredisTransactionCache(const trading::config::RedisConfig& config)
    : connection_(std::make_shared<RedisConnection>(config)) {}

HiredisTransactionCache::~HiredisTransactionCache() = default;

void HiredisTransactionCache::set_status(const std::string& transaction_id, const std::string& status) {
    auto* reply = static_cast<redisReply*>(
        redisCommand(connection_->context(), "SET %s %s", transaction_status_key(transaction_id).c_str(), status.c_str()));
    ensure_reply_ok(connection_->context(), reply, "SET");
    freeReplyObject(reply);
}

std::string HiredisTransactionCache::get_status(const std::string& transaction_id) const {
    auto* reply = static_cast<redisReply*>(
        redisCommand(connection_->context(), "GET %s", transaction_status_key(transaction_id).c_str()));
    ensure_reply_ok(connection_->context(), reply, "GET");

    std::string status;
    if (reply->type == REDIS_REPLY_STRING && reply->str != nullptr) {
        status = reply->str;
    }

    freeReplyObject(reply);
    return status;
}

HiredisMarketStateCache::HiredisMarketStateCache(const trading::config::RedisConfig& config)
    : connection_(std::make_shared<RedisConnection>(config)) {}

HiredisMarketStateCache::~HiredisMarketStateCache() = default;

void HiredisMarketStateCache::upsert_snapshot(const trading::storage::MarketSnapshot& snapshot) {
    auto* reply = static_cast<redisReply*>(
        redisCommand(
            connection_->context(),
            "HSET %s best_bid %s best_ask %s last_trade_price %s last_trade_quantity %s last_process_timestamp %lld",
            market_snapshot_key(snapshot.instrument_id).c_str(),
            encode_optional_double(snapshot.best_bid).c_str(),
            encode_optional_double(snapshot.best_ask).c_str(),
            encode_optional_double(snapshot.last_trade_price).c_str(),
            encode_optional_double(snapshot.last_trade_quantity).c_str(),
            static_cast<long long>(snapshot.last_process_timestamp)));
    ensure_reply_ok(connection_->context(), reply, "HSET");
    freeReplyObject(reply);
}

std::optional<trading::storage::MarketSnapshot> HiredisMarketStateCache::get_snapshot(const std::string& instrument_id) const {
    auto* reply = static_cast<redisReply*>(
        redisCommand(
            connection_->context(),
            "HMGET %s best_bid best_ask last_trade_price last_trade_quantity last_process_timestamp",
            market_snapshot_key(instrument_id).c_str()));
    ensure_reply_ok(connection_->context(), reply, "HMGET");

    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 5) {
        freeReplyObject(reply);
        return std::nullopt;
    }

    const auto* timestamp_reply = reply->element[4];
    if (timestamp_reply == nullptr || timestamp_reply->type == REDIS_REPLY_NIL || timestamp_reply->str == nullptr) {
        freeReplyObject(reply);
        return std::nullopt;
    }

    trading::storage::MarketSnapshot snapshot {
        .instrument_id = instrument_id,
        .best_bid = decode_optional_double(reply->element[0]),
        .best_ask = decode_optional_double(reply->element[1]),
        .last_trade_price = decode_optional_double(reply->element[2]),
        .last_trade_quantity = decode_optional_double(reply->element[3]),
        .last_process_timestamp = std::stoll(timestamp_reply->str),
    };
    freeReplyObject(reply);
    return snapshot;
}

}  // namespace trading::infrastructure
