# MiniDB — Redis-Inspired Key-Value Database

A lightweight key-value database built from scratch using **C11 and C++20**, featuring a custom TCP protocol, persistent storage, Write-Ahead Logging (WAL), concurrent client handling and key expiration (TTL).

MiniDB combines a C-based storage engine with a C++ networking layer to explore database internals, systems programming, concurrency and network protocol design.

## Features

- **C Storage Engine:** Custom binary storage engine implemented in C11 with a public C API.
- **Persistent Storage:** Append-only disk-based record storage with an in-memory hash index.
- **Write-Ahead Logging:** WAL-based write recovery and replay after interrupted operations.
- **TCP Server:** C++20 TCP server supporting remote database clients.
- **Concurrent Clients:** Multiple clients handled independently using worker threads.
- **Thread-Safe Storage:** Mutex-protected access to shared database state.
- **Key Expiration (TTL):** Expiring keys with persisted expiration metadata.
- **Automatic Expiration:** Background expiration checks in the server.
- **Database Compaction:** Rewrites active records to reduce storage overhead.
- **Batch Writes:** Supports writing multiple key-value pairs efficiently.
- **Custom TCP Protocol:** RESP-inspired request/response protocol.
- **Automated Testing:** Storage, C API, concurrency and integration stress tests.

## Architecture

MiniDB separates storage operations from network communication.

```text
             TCP Client
                  |
                  | Custom TCP Protocol
                  |
          +-------v--------+
          |  C++20 Server  |
          |                |
          | Client Threads |
          | Command Parser |
          +-------+--------+
                  |
          +-------v--------+
          | C++ Storage    |
          | Wrapper        |
          +-------+--------+
                  |
             C API
                  |
          +-------v--------+
          | C11 Storage    |
          | Engine         |
          +-------+--------+
                  |
          +-------v--------+
          | Binary Storage |
          | + WAL          |
          +----------------+
```

### Technology Stack

| Component | Technology |
|---|---|
| Storage engine | C11 |
| Application and networking | C++20 |
| Networking | TCP sockets |
| Indexing | C++ unordered_map |
| Persistence | Custom binary file format |
| Recovery | Write-Ahead Logging |
| Build system | CMake |
| Build generator | Ninja |
| Testing | CTest |
| HTTP libraries | None required |

## Supported Commands

MiniDB provides key-value operations through its TCP client.

| Command | Description |
|---|---|
| `PING` | Check server connectivity |
| `SET key value` | Store a key-value pair |
| `GET key` | Retrieve a value |
| `DELETE key` | Delete a key |
| `SIZE` | Return the number of live keys |
| `SET key value EX seconds` | Store a key with expiration |
| `EXPIRE key seconds` | Set expiration on an existing key |
| `TTL key` | Get remaining expiration time |
| `PERSIST key` | Remove expiration from a key |
| `QUIT` | Close client connection |

### Example

```text
minidb-net> PING
+PONG

minidb-net> SET name sooraj
+OK

minidb-net> GET name
$6
sooraj

minidb-net> SET session hello EX 60
+OK

minidb-net> TTL session
:59

minidb-net> DELETE name
:1
```

TTL response conventions:

- `-1`: Key exists without expiration.
- `-2`: Key does not exist.
- Non-negative integer: Remaining TTL in seconds.

## Getting Started

### Prerequisites

- CMake 3.20 or later
- C11-compatible compiler
- C++20-compatible compiler
- Ninja (optional)

### Windows Build

The following instructions use MSYS2 UCRT64 GCC and PowerShell.

Add the compiler to PATH:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
```

Configure the project:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Build:

```powershell
cmake --build build
```

Run automated tests:

```powershell
ctest --test-dir build --output-on-failure
```

### Start the Server

```powershell
.\build\minidb_server.exe
```

### Connect a Client

Open another terminal:

```powershell
.\build\minidb_client.exe
```

You can now execute database commands through the TCP client.

## Testing

MiniDB includes four automated test suites:

| Test Suite | Coverage |
|---|---|
| Storage Tests | CRUD, persistence, recovery, WAL and compaction |
| C API Tests | Direct testing of the C storage interface |
| Concurrency Tests | Concurrent storage access |
| Integration Stress Tests | Mixed operations across multiple threads |

Run all tests:

```powershell
ctest --test-dir build --output-on-failure
```

Run the integration stress test independently:

```powershell
.\build\minidb_integration_stress.exe
```

The integration stress test exercises:

- 8 concurrent threads.
- 2,000 mixed key workloads.
- Concurrent reads and writes.
- Updates, deletes and expiration operations.
- Compaction invariants.
- Database restart and value verification.

## Performance

MiniDB includes a benchmark executable for measuring individual writes, batch writes, reads and compaction.

Run:

```powershell
.\build\minidb_bench.exe 10000
```

The benchmark reports:

- Individual write throughput.
- Batch write throughput.
- Batch write speedup.
- Read throughput.
- Compaction duration.
- Storage size before and after compaction.

Performance depends on hardware, workload and build configuration. Benchmark results should be interpreted within those conditions.

## Storage and Recovery

MiniDB uses a custom append-only binary record format.

The storage engine maintains an in-memory hash index reconstructed from persistent records during startup.

The WAL records pending changes so that recovery can replay operations after an interrupted write sequence.

Compaction rewrites active records to reduce the size of the database file.

Expiration metadata is persisted so that TTL behavior survives database restarts.

## Project Structure

```text
MiniDB/
├── CMakeLists.txt
├── README.md
├── .gitignore
│
├── src/
│   ├── C storage engine
│   ├── C++ storage wrapper
│   ├── TCP server
│   ├── TCP client
│   └── command handling
│
├── tests/
│   ├── storage tests
│   ├── C API tests
│   ├── concurrency tests
│   └── integration stress tests
│
├── data/
│   ├── minidb.db
│   └── minidb.db.wal
│
└── build/
```

*The build directory and generated database files are runtime artifacts, not source files.*

## Design Limitations

MiniDB is an educational systems-programming project, not a production database.

Current design trade-offs include:

- Storage operations are serialized using a mutex for thread safety.
- The server uses a thread-per-client model.
- WAL recovery is implemented, but guaranteed power-loss durability using OS-level disk synchronization is not yet provided.
- The protocol is RESP-inspired and is not fully Redis protocol compatible.
- The server does not implement authentication or encrypted transport.

## Project Goals

MiniDB was developed to explore:

- Database storage engine design.
- Binary file formats and persistence.
- Write-Ahead Logging and crash recovery.
- C and C++ interoperability.
- TCP networking and custom protocols.
- Concurrent client handling.
- Thread safety and synchronization.
- Database indexing, TTL and compaction.
