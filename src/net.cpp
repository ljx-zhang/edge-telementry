#include "net.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace edge {
namespace {

void close_socket(NativeSocket socket) {
    if (socket == invalid_socket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

std::runtime_error socket_error(const std::string& prefix) {
#ifdef _WIN32
    return std::runtime_error(prefix + ": WSA error " + std::to_string(WSAGetLastError()));
#else
    return std::runtime_error(prefix + ": errno " + std::to_string(errno));
#endif
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const auto position = line.find('\t', start);
        if (position == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, position - start));
        start = position + 1;
    }
    return fields;
}

}  // namespace

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw socket_error("WSAStartup failed");
    }
#endif
}

NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

TcpSocket::~TcpSocket() { close_socket(socket_); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : socket_(other.release()) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close_socket(socket_);
        socket_ = other.release();
    }
    return *this;
}

NativeSocket TcpSocket::release() noexcept {
    return std::exchange(socket_, invalid_socket);
}

TcpSocket connect_tcp(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0) {
        throw socket_error("address lookup failed");
    }

    TcpSocket connected;
    for (auto* address = result; address != nullptr; address = address->ai_next) {
        TcpSocket candidate(socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!candidate.valid()) {
            continue;
        }
        if (connect(candidate.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            connected = std::move(candidate);
            break;
        }
    }
    freeaddrinfo(result);
    if (!connected.valid()) {
        throw socket_error("connection failed");
    }
    return connected;
}

TcpSocket listen_tcp(std::uint16_t port) {
    TcpSocket listener(socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
    if (!listener.valid()) {
        throw socket_error("socket creation failed");
    }
    int enabled = 1;
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    int disabled = 0;
    setsockopt(listener.get(), IPPROTO_IPV6, IPV6_V6ONLY,
               reinterpret_cast<const char*>(&disabled), sizeof(disabled));

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);
    if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw socket_error("bind failed");
    }
    if (listen(listener.get(), SOMAXCONN) != 0) {
        throw socket_error("listen failed");
    }
    return listener;
}

TcpSocket accept_tcp(const TcpSocket& listener, int timeout_ms) {
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(listener.get(), &sockets);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    const int result = select(0, &sockets, nullptr, nullptr, &timeout);
#else
    const int result = select(listener.get() + 1, &sockets, nullptr, nullptr, &timeout);
#endif
    if (result == 0) {
        return {};
    }
    if (result < 0) {
        throw socket_error("select failed");
    }
    const auto socket = accept(listener.get(), nullptr, nullptr);
    if (socket == invalid_socket) {
        throw socket_error("accept failed");
    }
    return TcpSocket(socket);
}

void set_socket_timeout(const TcpSocket& socket, int timeout_ms) {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

void send_all(const TcpSocket& socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto result = send(
            socket.get(), data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (result <= 0) {
            throw socket_error("send failed");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::string read_line(const TcpSocket& socket, std::size_t max_size) {
    std::string line;
    line.reserve(256);
    while (line.size() < max_size) {
        char character{};
        const auto result = recv(socket.get(), &character, 1, 0);
        if (result == 0) {
            if (line.empty()) {
                return {};
            }
            throw std::runtime_error("connection closed in the middle of a line");
        }
        if (result < 0) {
            throw socket_error("receive failed");
        }
        if (character == '\n') {
            return line;
        }
        if (character != '\r') {
            line.push_back(character);
        }
    }
    throw std::runtime_error("protocol line exceeds maximum size");
}

std::string encode_event(const Event& event) {
    if (event.id.empty() || event.device_id.empty() ||
        event.id.find_first_of("\t\r\n") != std::string::npos ||
        event.device_id.find_first_of("\t\r\n") != std::string::npos) {
        throw std::invalid_argument("event identifiers contain invalid characters");
    }
    std::ostringstream stream;
    // 协议版本放在消息中，未来修改字段时可以明确拒绝不兼容客户端。
    stream << "EVENT\t1\t" << event.id << '\t' << event.device_id << '\t'
           << event.sequence << '\t' << event.timestamp_ms << '\t'
           << std::setprecision(17) << event.value << '\n';
    return stream.str();
}

Event decode_event(const std::string& line) {
    const auto fields = split_tabs(line);
    if (fields.size() != 7 || fields[0] != "EVENT" || fields[1] != "1") {
        throw std::runtime_error("invalid EVENT message");
    }
    if (fields[2].empty() || fields[3].empty() || fields[2].size() > 256 || fields[3].size() > 128) {
        throw std::runtime_error("invalid event identifier length");
    }
    Event event{
        fields[2],
        fields[3],
        std::stoll(fields[4]),
        std::stoll(fields[5]),
        std::stod(fields[6]),
    };
    if (event.sequence <= 0 || event.timestamp_ms <= 0 || !std::isfinite(event.value)) {
        throw std::runtime_error("invalid event field value");
    }
    return event;
}

}  // namespace edge
