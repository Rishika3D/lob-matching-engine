#include "lob/event_io.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <istream>
#include <optional>
#include <string_view>

namespace lob {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields.push_back(trim(line.substr(start)));
            return fields;
        }
        fields.push_back(trim(line.substr(start, comma - start)));
        start = comma + 1;
    }
}

template <typename T>
std::optional<T> to_number(std::string_view s) {
    T value{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

ParseResult parse_events(std::istream& in) {
    ParseResult result;
    std::string raw;
    std::size_t line_no = 0;
    bool seen_data = false;
    std::int64_t last_ts = 0;

    auto fail = [&](const std::string& what) {
        result.events.clear();
        result.error = "line " + std::to_string(line_no) + ": " + what;
        return result;
    };

    while (std::getline(in, raw)) {
        ++line_no;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        const auto f = split(line);

        // Optional header row: only recognised before any data.
        if (!seen_data && lower(f[0]) == "timestamp") {
            seen_data = true;
            continue;
        }
        seen_data = true;

        if (f.size() != 8) {
            return fail("expected 8 comma-separated fields, found " + std::to_string(f.size()));
        }

        MarketEvent ev;

        const auto ts = to_number<std::int64_t>(f[0]);
        if (!ts) return fail("bad timestamp '" + std::string(f[0]) + "'");
        if (!result.events.empty() && *ts < last_ts) {
            return fail("timestamp goes backwards (" + std::to_string(*ts) + " after " +
                        std::to_string(last_ts) + ")");
        }
        ev.timestamp = *ts;
        last_ts = *ts;

        const std::string action = lower(f[1]);
        if (action == "new") ev.kind = EventKind::NewOrder;
        else if (action == "cancel") ev.kind = EventKind::Cancel;
        else if (action == "modify") ev.kind = EventKind::Modify;
        else return fail("unknown action '" + std::string(f[1]) + "' (expected new, cancel or modify)");

        const auto id = to_number<OrderId>(f[2]);
        if (!id) return fail("bad order id '" + std::string(f[2]) + "'");
        ev.order.id = *id;

        if (ev.kind == EventKind::NewOrder) {
            const std::string side = lower(f[3]);
            if (side == "buy" || side == "b") ev.order.side = Side::Buy;
            else if (side == "sell" || side == "s") ev.order.side = Side::Sell;
            else return fail("bad side '" + std::string(f[3]) + "' (expected buy or sell)");

            const std::string type = lower(f[4]);
            if (type == "limit") ev.order.type = OrderType::Limit;
            else if (type == "market") ev.order.type = OrderType::Market;
            else return fail("bad type '" + std::string(f[4]) + "' (expected limit or market)");

            const std::string tif = lower(f[5]);
            if (tif.empty() || tif == "gtc") ev.order.tif = TimeInForce::GoodTilCancel;
            else if (tif == "ioc") ev.order.tif = TimeInForce::ImmediateOrCancel;
            else if (tif == "fok") ev.order.tif = TimeInForce::FillOrKill;
            else return fail("bad time in force '" + std::string(f[5]) + "' (expected gtc, ioc or fok)");

            if (ev.order.type == OrderType::Limit) {
                const auto price = to_number<Price>(f[6]);
                if (!price) return fail("limit order needs a numeric price, got '" + std::string(f[6]) + "'");
                ev.order.price = *price;
            }
            const auto qty = to_number<Quantity>(f[7]);
            if (!qty) return fail("bad quantity '" + std::string(f[7]) + "'");
            ev.order.quantity = *qty;
        } else if (ev.kind == EventKind::Modify) {
            const auto price = to_number<Price>(f[6]);
            if (!price) return fail("modify needs a numeric price, got '" + std::string(f[6]) + "'");
            const auto qty = to_number<Quantity>(f[7]);
            if (!qty) return fail("modify needs a numeric quantity, got '" + std::string(f[7]) + "'");
            ev.order.price = *price;
            ev.order.quantity = *qty;
        }

        result.events.push_back(ev);
    }
    return result;
}

} // namespace lob
