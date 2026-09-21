# Edge Telemetry

A reproducible C++20 project for reliable edge telemetry collection and retry after network interruption.

The project contains two processes:

- `edge_agent` generates deterministic telemetry, writes every event to a local SQLite WAL database, uploads pending rows, and marks a row acknowledged only after receiving an ACK.
- `telemetry_receiver` stores events using an idempotency key and ACKs both new and duplicate deliveries.

Version 1.0 also provides a bounded receiver worker pool, versioned protocol validation, row- and byte-based pending limits, ACK latency metrics, CTest integration, fault injection, and a reproducible benchmark.

If the receiver stores an event and the connection fails before the ACK reaches the agent, the agent retries the event. The receiver's primary key prevents a duplicate row and returns the same ACK.

## Build on Windows

The project requires a C++20 compiler, CMake, and SQLite headers/library/runtime.

On Windows with Visual Studio and Conda SQLite, the helper script discovers the toolchain:

```powershell
.\scripts\build_windows.ps1
```

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DSQLITE3_ROOT=C:\path\to\sqlite-root
cmake --build build
```

For a Conda installation, the SQLite root is commonly `%CONDA_PREFIX%\Library` or the Conda installation directory when it contains `Library\include` and `Library\lib`.

## Run

Start the receiver:

```powershell
.\build\telemetry_receiver.exe --db receiver.db --port 9100
```

Start the edge agent in a second terminal:

```powershell
.\build\edge_agent.exe --db edge.db --host 127.0.0.1 --port 9100 --device device-001 --interval-ms 250
```

Stop either process with `Ctrl+C`. Restarting the edge agent with the same database resumes pending uploads and continues the persistent sequence number.

For an automated finite run:

```powershell
.\build\telemetry_receiver.exe --db receiver.db --port 9100 --runtime-sec 10
.\build\edge_agent.exe --db edge.db --port 9100 --runtime-sec 4 --interval-ms 100
```

Or run the reproducible integration demo after building:

```powershell
.\scripts\integration_demo.ps1
```

The script creates a new timestamped demo directory, runs both processes, and—when `sqlite3` is on `PATH`—checks that every edge row is acknowledged and every receiver event ID is unique.

Run the fault scenarios:

```powershell
.\scripts\fault_injection_demo.ps1
```

This verifies two engineering failure modes: an ACK lost after durable receiver storage, and an unavailable receiver causing the edge queue to reach its configured `--max-pending` bound.

The Chinese learning path is in [`docs/learning-guide.zh-CN.md`](docs/learning-guide.zh-CN.md).

Run all registered automated tests:

```powershell
ctest --test-dir build --output-on-failure
```

Run the local performance measurement:

```powershell
.\scripts\benchmark.ps1
```

The latest measured result and its limitations are documented in [`docs/benchmark-report.md`](docs/benchmark-report.md).

## Current scope

Version 1.0 implements the intended resume-project scope. The remaining production gaps are TLS/authentication, external metrics export, schema migrations, and a batch-transaction protocol. The byte limit is an estimate of active pending payload rather than the physical SQLite file size.
