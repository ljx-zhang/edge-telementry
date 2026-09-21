#pragma once

#include "model.hpp"

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

namespace edge {

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(NativeSocket socket) : socket_(socket) {}
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    NativeSocket get() const noexcept { return socket_; }
    bool valid() const noexcept { return socket_ != invalid_socket; }
    NativeSocket release() noexcept;

private:
    NativeSocket socket_{invalid_socket};
};

TcpSocket connect_tcp(const std::string& host, std::uint16_t port);
TcpSocket listen_tcp(std::uint16_t port);
TcpSocket accept_tcp(const TcpSocket& listener, int timeout_ms);
void set_socket_timeout(const TcpSocket& socket, int timeout_ms);
void send_all(const TcpSocket& socket, const std::string& data);
std::string read_line(const TcpSocket& socket, std::size_t max_size = 4096);

std::string encode_event(const Event& event);
Event decode_event(const std::string& line);

}  // namespace edge
