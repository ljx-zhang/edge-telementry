# Benchmark report

Date: 2026-09-21

This is a reproducible development-machine measurement, not a general hardware claim.

## Configuration

- Windows NT 10.0.26200.0
- MSVC 19.51, C++20 release-style NMake build
- SQLite 3.40.1, WAL mode, `synchronous=FULL`
- Agent and receiver connected through the local loopback interface
- 4 receiver workers and a 64-connection bounded queue
- 5-second generation window, 1 ms requested generation interval
- Upload batch selection size: 256

Command:

```powershell
.\scripts\benchmark.ps1 -Port 19460 -RuntimeSeconds 5 -IntervalMilliseconds 1
```

## Result

| Metric | Measured value |
|---|---:|
| Generated and acknowledged events | 244 |
| End-to-end elapsed time | 5.111 s |
| Acknowledged events per second | 47.74 |
| ACK latency P50 | 2.792 ms |
| ACK latency P95 | 7.718 ms |
| ACK latency P99 | 8.555 ms |
| Retries | 0 |
| Pending at exit | 0 |
| Receiver duplicates | 0 |
| Rejected connections | 0 |

The latency measurement starts immediately before sending an already-persisted event and stops when its ACK arrives. It excludes local generation and the initial SQLite insert. Throughput includes the complete run and drain time, so it reflects the current durable pipeline rather than raw socket speed.

The main bottleneck in this configuration is per-event durable SQLite work on both sides. A future batch-transaction protocol would be the appropriate optimization experiment.
