#include "net.hpp"
#include "store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool stop_requested{false};

// 这些计数器只描述“本次运行”，数据库中的状态才是跨重启保存的事实来源。
std::atomic<std::int64_t> generated_this_run{0};
std::atomic<std::int64_t> retry_count{0};
std::atomic<std::int64_t> backpressure_count{0};

void signal_handler(int) { stop_requested.store(true); }

std::string argument(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return fallback;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, signal_handler);
        const auto database_path = std::filesystem::path(argument(argc, argv, "--db", "edge.db"));
        const auto host = argument(argc, argv, "--host", "127.0.0.1");
        const auto port = static_cast<std::uint16_t>(std::stoi(argument(argc, argv, "--port", "9100")));
        const auto device_id = argument(argc, argv, "--device", "device-001");
        const auto interval_ms = std::stoi(argument(argc, argv, "--interval-ms", "250"));
        const auto batch_size = static_cast<std::size_t>(std::stoul(argument(argc, argv, "--batch-size", "32")));
        const auto runtime_seconds = std::stoi(argument(argc, argv, "--runtime-sec", "0"));
        const auto drain_seconds = std::stoi(argument(argc, argv, "--drain-sec", "3"));
        const auto max_pending = static_cast<std::int64_t>(
            std::stoll(argument(argc, argv, "--max-pending", "10000")));
        const auto max_pending_bytes = static_cast<std::int64_t>(
            std::stoll(argument(argc, argv, "--max-pending-bytes", "67108864")));
        if (interval_ms <= 0 || batch_size == 0 || max_pending <= 0 || max_pending_bytes <= 0) {
            throw std::invalid_argument(
                "interval, batch size, max pending, and max pending bytes must be positive");
        }

        edge::NetworkRuntime network;
        edge::EdgeStore initialize(database_path);
        std::atomic_bool generation_done{false};
        std::vector<double> ack_latencies_ms;

        std::cout << "edge agent: device=" << device_id
                  << " max_pending=" << max_pending
                  << " max_pending_bytes=" << max_pending_bytes
                  << " batch_size=" << batch_size << '\n';

        std::thread generator([&] {
            edge::EdgeStore store(database_path);
            while (!stop_requested.load()) {
                const auto stats = store.stats();

                // 有界积压是背压的边界：网络长时间不可用时暂停上游生成，
                // 防止内存或磁盘队列无限增长。这里模拟的是“设备可以被降速”的场景。
                if (stats.pending >= max_pending ||
                    store.pending_payload_bytes() >= max_pending_bytes) {
                    ++backpressure_count;
                    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                    continue;
                }

                const double value = 20.0 + std::sin(static_cast<double>(stats.total + 1) / 10.0) * 5.0;
                store.append(device_id, now_ms(), value);
                ++generated_this_run;
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
            generation_done.store(true);
        });

        std::thread uploader([&] {
            edge::EdgeStore store(database_path);
            auto drain_started = std::chrono::steady_clock::time_point{};
            while (true) {
                const auto stats = store.stats();
                if (generation_done.load() && stats.pending == 0) {
                    break;
                }
                if (generation_done.load() && drain_started == std::chrono::steady_clock::time_point{}) {
                    drain_started = std::chrono::steady_clock::now();
                }
                if (generation_done.load() &&
                    std::chrono::steady_clock::now() - drain_started > std::chrono::seconds(drain_seconds)) {
                    break;
                }

                const auto events = store.pending(batch_size);
                if (events.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                try {
                    auto socket = edge::connect_tcp(host, port);
                    edge::set_socket_timeout(socket, 2000);
                    for (const auto& event : events) {
                        // 事件先在本地 WAL 中存在，再通过网络发送。只有收到与事件 ID
                        // 匹配的 ACK 后才修改本地状态，因此崩溃后仍能找到未确认事件。
                        const auto send_started = std::chrono::steady_clock::now();
                        edge::send_all(socket, edge::encode_event(event));
                        const auto response = edge::read_line(socket);
                        if (response == "ACK\t1\t" + event.id) {
                            const auto elapsed = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - send_started);
                            ack_latencies_ms.push_back(elapsed.count());
                            store.acknowledge(event.id);
                        } else {
                            throw std::runtime_error("unexpected receiver response: " + response);
                        }
                    }
                } catch (const std::exception& error) {
                    ++retry_count;
                    std::cerr << "upload retry: " << error.what() << '\n';
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
        });

        if (runtime_seconds > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(runtime_seconds));
            stop_requested.store(true);
        } else {
            while (!stop_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        generator.join();
        uploader.join();

        const auto stats = initialize.stats();
        std::cout << std::fixed << std::setprecision(3)
                  << "edge summary: total=" << stats.total
                  << " acknowledged=" << stats.acknowledged
                  << " pending=" << stats.pending
                  << " generated_this_run=" << generated_this_run.load()
                  << " retries=" << retry_count.load()
                  << " backpressure=" << backpressure_count.load()
                  << " ack_latency_ms_p50=" << percentile(ack_latencies_ms, 0.50)
                  << " ack_latency_ms_p95=" << percentile(ack_latencies_ms, 0.95)
                  << " ack_latency_ms_p99=" << percentile(ack_latencies_ms, 0.99)
                  << '\n';
        return stats.pending == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "edge agent failed: " << error.what() << '\n';
        return 1;
    }
}
