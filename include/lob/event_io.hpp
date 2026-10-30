#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "lob/order.hpp"

namespace lob {

enum class EventKind : std::uint8_t {
    NewOrder,  // `order` is a new order to submit
    Cancel,    // `order.id` names the resting order to cancel
    Modify     // `order.id` names the order; `order.price` / `order.quantity` are the new values
};

// One line of historical order flow.
struct MarketEvent {
    std::int64_t timestamp = 0;
    EventKind kind = EventKind::NewOrder;
    Order order;
};

struct ParseResult {
    std::vector<MarketEvent> events;
    std::string error;  // empty on success; otherwise "line N: what went wrong"
    bool ok() const { return error.empty(); }
};

// Parses order-flow CSV:
//
//   timestamp,action,id,side,type,tif,price,quantity
//   1000,new,1,buy,limit,gtc,10000,50
//   1001,new,2,sell,market,ioc,,20
//   1002,modify,1,,,,9999,40
//   1003,cancel,1,,,,,
//
// - action: new | cancel | modify;  side: buy | sell (or b | s);
//   type: limit | market;  tif: gtc | ioc | fok (blank means gtc)
// - `cancel` needs only the id; `modify` needs id, price and quantity;
//   `new` needs side, type, quantity, and a price unless it is a market order
// - an optional header row, blank lines and lines starting with '#' are skipped
// - timestamps must not decrease
//
// Parsing stops at the first bad line and reports it; nothing partial is
// returned as success.
ParseResult parse_events(std::istream& in);

} // namespace lob
