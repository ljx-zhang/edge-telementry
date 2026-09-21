#include "net.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void require_throws(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        const edge::Event original{"device-01:42", "device-01", 42, 1'700'000'000'123, 23.75};
        const auto encoded = edge::encode_event(original);
        const auto decoded = edge::decode_event(encoded.substr(0, encoded.size() - 1));

        require(decoded.id == original.id, "event ID changed during round trip");
        require(decoded.device_id == original.device_id, "device ID changed during round trip");
        require(decoded.sequence == original.sequence, "sequence changed during round trip");
        require(decoded.timestamp_ms == original.timestamp_ms, "timestamp changed during round trip");
        require(std::abs(decoded.value - original.value) < 1e-12, "value changed during round trip");

        require_throws(
            [] { edge::decode_event("EVENT\t2\tid\tdevice\t1\t1700000000000\t1.0"); },
            "unknown protocol version was accepted");
        require_throws(
            [] { edge::decode_event("EVENT\t1\tid\tdevice\t0\t1700000000000\t1.0"); },
            "invalid sequence was accepted");
        require_throws(
            [] { edge::encode_event(edge::Event{"bad\tid", "device", 1, 1, 1.0}); },
            "invalid event ID was encoded");

        std::cout << "protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol test failed: " << error.what() << '\n';
        return 1;
    }
}

