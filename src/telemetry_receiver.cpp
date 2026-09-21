#include "net.hpp"
#include "store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool stop_requested{false};
std::atomic<std::int64_t> inserted{0};
std::atomic<std::int64_t> duplicates{0};
std::atomic<std::int64_t> delivery_attempts{0};
std::atomic<std::int64_t> dropped_acks{0};
std::atomic<std::int64_t> rejected_connections{0};

void signal_handler(int) { stop_requested.store(true); }

std::string argument(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return fallback;
}

class SocketQueue {
public:
    explicit SocketQueue(std::size_t capacity) : capacity_(capacity) {}

    bool push(edge::TcpSocket socket) {
        std::lock_guard lock(mutex_);
        if (closed_ || sockets_.size() >= capacity_) {
            return false;
        }
        sockets_.push(std::move(socket));
        ready_.notify_one();
        return true;
    }

    bool pop(edge::TcpSocket& socket) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !sockets_.empty(); });
        if (sockets_.empty()) {
            return false;
        }
        socket = std::move(sockets_.front());
        sockets_.pop();
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<edge::TcpSocket> sockets_;
    bool closed_{false};
};

void handle_client(edge::TcpSocket client,
                   const std::filesystem::path& database_path,
                   int processing_delay_ms,
                   int drop_ack_every) {
    try {
        edge::set_socket_timeout(client, 5000);
        edge::ReceiverStore store(database_path);
        for (;;) {
            const auto line = edge::read_line(client);
            if (line.empty()) {
                break;
            }
            try {
                const auto event = edge::decode_event(line);
                if (store.insert_if_new(event)) {
                    ++inserted;
                } else {
                    ++duplicates;
                }

                const auto attempt = ++delivery_attempts;
                if (processing_delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(processing_delay_ms));
                }

                // 故障注入点：数据已经提交到接收端数据库，但故意关闭连接而不回 ACK。
                // 客户端会重发同一个 event_id，INSERT OR IGNORE 保证不会产生重复行。
                if (drop_ack_every > 0 && attempt % drop_ack_every == 0) {
                    ++dropped_acks;
                    return;
                }
                edge::send_all(client, "ACK\t1\t" + event.id + "\n");
            } catch (const std::exception& error) {
                edge::send_all(client, std::string("ERR\t1\t") + error.what() + "\n");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "client disconnected: " << error.what() << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, signal_handler);
        const auto database_path = std::filesystem::path(argument(argc, argv, "--db", "receiver.db"));
        const auto port = static_cast<std::uint16_t>(std::stoi(argument(argc, argv, "--port", "9100")));
        const auto runtime_seconds = std::stoi(argument(argc, argv, "--runtime-sec", "0"));
        const auto processing_delay_ms = std::stoi(
            argument(argc, argv, "--processing-delay-ms", "0"));
        const auto drop_ack_every = std::stoi(
            argument(argc, argv, "--drop-ack-every", "0"));
        const auto worker_count = static_cast<std::size_t>(
            std::stoul(argument(argc, argv, "--workers", "4")));
        const auto connection_queue_size = static_cast<std::size_t>(
            std::stoul(argument(argc, argv, "--connection-queue", "64")));
        if (processing_delay_ms < 0 || drop_ack_every < 0 ||
            worker_count == 0 || connection_queue_size == 0) {
            throw std::invalid_argument("receiver configuration values are invalid");
        }

        edge::NetworkRuntime network;
        edge::ReceiverStore store(database_path);
        auto listener = edge::listen_tcp(port);
        SocketQueue socket_queue(connection_queue_size);
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back([&] {
                edge::TcpSocket client;
                while (socket_queue.pop(client)) {
                    handle_client(
                        std::move(client), database_path, processing_delay_ms, drop_ack_every);
                }
            });
        }

        const auto started = std::chrono::steady_clock::now();
        std::cout << "receiver listening on port " << port
                  << " processing_delay_ms=" << processing_delay_ms
                  << " drop_ack_every=" << drop_ack_every
                  << " workers=" << worker_count
                  << " connection_queue=" << connection_queue_size << '\n';

        while (!stop_requested.load()) {
            if (runtime_seconds > 0 &&
                std::chrono::steady_clock::now() - started >= std::chrono::seconds(runtime_seconds)) {
                break;
            }
            auto client = edge::accept_tcp(listener, 200);
            if (client.valid() && !socket_queue.push(std::move(client))) {
                ++rejected_connections;
            }
        }

        // 停止接收新连接后，关闭队列并等待所有 worker 完成手头连接。
        // 这样主函数退出时不会遗留仍在访问全局状态或数据库的分离线程。
        socket_queue.close();
        for (auto& worker : workers) {
            worker.join();
        }

        std::cout << "receiver summary: stored=" << store.count()
                  << " inserted_this_run=" << inserted.load()
                  << " duplicates_this_run=" << duplicates.load()
                  << " delivery_attempts=" << delivery_attempts.load()
                  << " dropped_acks=" << dropped_acks.load()
                  << " rejected_connections=" << rejected_connections.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "receiver failed: " << error.what() << '\n';
        return 1;
    }
}
