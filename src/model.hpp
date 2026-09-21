#pragma once

#include <cstdint>
#include <string>

namespace edge {

struct Event {
    std::string id;
    std::string device_id;
    std::int64_t sequence{};
    std::int64_t timestamp_ms{};
    double value{};
};

struct EdgeStats {
    std::int64_t total{};
    std::int64_t acknowledged{};
    std::int64_t pending{};
};

}  // namespace edge

