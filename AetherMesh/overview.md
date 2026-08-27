# AetherMesh — System Overview

> **AetherMesh** is an offline-first, ephemeral, end-to-end encrypted peer-to-peer mesh chat system.
> It operates entirely on local networks with **zero internet dependency**, stores **nothing on disk**,
> and cryptographically wipes all keys and messages from RAM on exit.

---

## Table of Contents

1. [How It Works — One Line](#1-how-it-works--one-line)
2. [Component Architecture](#2-component-architecture)
3. [Project Folder Structure](#3-project-folder-structure)
4. [Core Flows](#4-core-flows)
5. [Security Properties](#5-security-properties)
6. [How to Run](#6-how-to-run)
7. [Technology Stack](#7-technology-stack)

---

## 1. How It Works — One Line

> Three programs running together: a **Zig crypto library** encrypts every message, a **C++ daemon** moves encrypted bytes over the LAN, and a **Go TUI client** shows the chat interface — all without touching the internet or writing anything to disk.

---

## 2. Component Architecture

```
  ┌──────────────────────────────────┐
  │         Go Client                │
  │        (aether-cli)              │
  │                                  │
  │  • Bubble Tea split-screen TUI   │
  │  • Gossip routing engine         │
  │  • UUID deduplication cache      │
  └───────────────┬──────────────────┘
                  │
          Unix Domain Socket
           /tmp/aether.sock
                  │
  ┌───────────────▼──────────────────┐
  │         C++ Daemon               │
  │          (aetherd)               │
  │                                  │
  │  • TCP multi-peer socket pool    │
  │  • UDP multicast auto-discovery  │
  │  • IPC server for Go client      │
  └───────────────┬──────────────────┘
                  │
          C FFI / dynamic link
         libaethercrypto.so
                  │
  ┌───────────────▼──────────────────┐
  │         Zig Crypto Core          │
  │      (libaethercrypto.so)        │
  │                                  │
  │  • ChaCha20-Poly1305 AEAD        │
  │  • Per-message CSPRNG nonces     │
  │  • Volatile secure RAM zeroing   │
  └──────────────────────────────────┘
```

### 2.1 Zig Crypto Core (`crypto/`)

**Role:** All cryptography and secure memory operations.

- Compiles to a **shared library** (`libaethercrypto.so`) exposing three C-ABI functions
- `aether_encrypt()` — encrypts plaintext using **ChaCha20-Poly1305** with a fresh CSPRNG nonce per message
- `aether_decrypt()` — decrypts and verifies the Poly1305 authentication tag; rejects tampered packets
- `aether_secure_zero()` — zeroes memory using Zig's volatile semantics, guaranteeing the compiler cannot optimize it out

### 2.2 C++ Transport Daemon (`daemon/`)

**Role:** All network I/O and concurrency.

- Runs as a background process (`aetherd`)
- Manages a **multi-threaded TCP connection pool** — one thread per active remote peer
- Broadcasts and listens on **UDP multicast** (`224.0.0.1:9999`) for automatic LAN peer discovery
- Verifies discovery beacons using **HMAC-SHA256** derived from the PSK (prevents unauthorized nodes)
- Hosts a **Unix Domain Socket server** at `/tmp/aether.sock` for local IPC with the Go client
- Calls into `libaethercrypto.so` for every encrypt/decrypt operation

### 2.3 Go Client (`client/`)

**Role:** User interface and distributed routing logic.

- Connects to the daemon's Unix socket
- Renders a **split-screen terminal UI** using the Bubble Tea framework
- Maintains a thread-safe **UUID deduplication cache** (last 1,000 message IDs) to prevent re-displaying or re-gossiping already-seen messages
- Commands available in chat: `/help`, `/peers`, `/quit`

---

## 3. Project Folder Structure

```
AetherMesh/
│
├── overview.md                     ← THIS FILE
├── Makefile                        ← Build all 3 components
├── README.md
├── .gitignore
├── docker-compose.yml              ← 3-node gossip test topology
├── dockerfile
│
├── docs/
│   └── architecture.md             ← Deep technical architecture with diagrams
│
├── crypto/                         ← [ZIG] Cryptographic shared library
│   ├── build.zig
│   ├── build.zig.zon
│   └── src/
│       ├── lib.zig                 ← C-ABI export entry points
│       ├── aead.zig                ← ChaCha20-Poly1305 encrypt / decrypt
│       └── memory.zig              ← Volatile secure RAM zeroing
│
├── daemon/                         ← [C++] Network transport daemon
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp                ← Entry point, CLI parsing, thread spawning
│       ├── socket_manager.hpp/cpp  ← TCP connection pool, framed read/write
│       ├── discovery.hpp/cpp       ← UDP multicast beacon TX/RX + HMAC verify
│       ├── ipc.hpp/cpp             ← Unix socket IPC server
│       ├── sha256.hpp/cpp          ← SHA-256 for HMAC and key derivation
│       └── crypto_ffi.hpp          ← FFI interface to libaethercrypto.so
│
└── client/                         ← [GO] Gossip router + TUI client
    ├── go.mod
    ├── main.go                     ← CLI flags, key derivation, IPC connect
    ├── gossip/
    │   └── router.go               ← Message struct, dedup cache, frame parsing
    └── ui/
        └── tui.go                  ← Bubble Tea model: viewport + input + hints
```

---

## 4. Core Flows

### 4.1 Peer Discovery (Fully Automatic — No Config Needed)

```
Node A starts
  → Opens TCP port 7331
  → Starts broadcasting UDP beacon to 224.0.0.1:9999 every 5 seconds
    Beacon contains: [magic][port][nodeUUID][HMAC-SHA256(PSK)]

Node B starts
  → Joins multicast group 224.0.0.1:9999
  → Receives Node A's beacon
  → Verifies HMAC using its own PSK
  → If valid: dials TCP to Node A's IP:7331
  → Both nodes add each other to their peer pools
  → Peer count in TUI increments to 1
```

### 4.2 Sending a Message

```
User types "hello" → presses Enter

Go Client (tui.go)
  → Captures input
  → Creates Message{UUID: random16, Sender: "alice", Text: "hello"}
  → Marks UUID in dedup cache
  → Serializes and writes frame to /tmp/aether.sock

C++ Daemon (ipc.cpp)
  → Reads frame from Unix socket
  → Calls aether_encrypt(key, "hello") → nonce + tag + ciphertext
  → Assembles wire frame: [length][UUID][timestamp][sender][nonce][tag][ciphertext]
  → Broadcasts frame to all connected TCP peers
```

### 4.3 Receiving a Gossiped Message

```
C++ Daemon (socket_manager.cpp)
  → Receives encrypted wire frame from a remote peer
  → Calls aether_decrypt(key, nonce, tag, ciphertext) → plaintext
  → If decryption fails (wrong key / tampered): silently drops frame
  → Forwards plaintext + metadata through /tmp/aether.sock to Go client

Go Client (gossip/router.go)
  → Reads frame from Unix socket
  → Checks UUID in dedup cache
    → Already seen? → Discard silently
    → New? → Display in TUI viewport
           → Mark UUID in cache
           → Re-gossip encrypted frame to daemon for forwarding to other peers
```

### 4.4 Shutdown & Memory Wipe

```
User presses Ctrl+C or types /quit

Go Client
  → Sends SHUTDOWN signal to daemon via IPC
  → Zeroes own key bytes: for i := range key { key[i] = 0 }

C++ Daemon
  → Receives SHUTDOWN
  → Calls aether_secure_zero(key, 32) in Zig library
  → Closes all TCP and UDP sockets
  → Deletes Unix socket file
  → Exits cleanly

Result: No keys, no messages, no trace in RAM or on disk
```

---

## 5. Security Properties

| Property | How It's Achieved |
|----------|------------------|
| **Confidentiality** | ChaCha20-Poly1305 — unreadable without the PSK |
| **Integrity** | Poly1305 authentication tag — tampered bytes cause frame rejection |
| **Replay protection** | Fresh CSPRNG nonce per message — identical plaintext → different ciphertext every time |
| **Unauthorized peer rejection** | Discovery beacon HMAC — wrong PSK nodes are silently ignored |
| **No disk persistence** | All data lives in RAM only — never written to any file |
| **Memory sanitization** | Zig volatile `@memset` — guaranteed zeroing that compilers cannot optimize out |

---

## 6. How to Run

### Build Everything

```bash
# Build Zig crypto library first
cd crypto && zig build -Doptimize=ReleaseSafe && cd ..

# Build C++ daemon (links against crypto library)
cd daemon/build && make && cd ../..

# Build Go client
cd client && go build -o ../bin/aether-cli . && cd ..
```

### Run a Node (2 terminals needed)

**Terminal 1 — Start the Daemon:**
```bash
./daemon/build/aetherd --psk "your-secret" --name alice
```

**Terminal 2 — Start the Chat Client:**
```bash
./bin/aether-cli --name alice --psk "your-secret"
```

### Run Two Nodes Locally (4 terminals)

```bash
# Terminal 1
./daemon/build/aetherd --psk "your-secret" --name alice --port 7331 --ipc-socket /tmp/aether-alice.sock

# Terminal 2
./bin/aether-cli --name alice --psk "your-secret" --socket /tmp/aether-alice.sock

# Terminal 3
./daemon/build/aetherd --psk "your-secret" --name bob --port 7332 --ipc-socket /tmp/aether-bob.sock

# Terminal 4
./bin/aether-cli --name bob --psk "your-secret" --socket /tmp/aether-bob.sock
```

> Alice and Bob will auto-discover each other within 5 seconds via UDP multicast.
> The peer count in each TUI will change from `0` to `1`.
> Messages typed in Alice's window will appear in Bob's window and vice versa.

---

## 7. Technology Stack

| Component | Language | Version | Key Libraries |
|-----------|----------|---------|---------------|
| Crypto Library | **Zig** | 0.16.0 | `std.crypto` (stdlib) |
| Network Daemon | **C++** | C++20 | POSIX sockets, `pthread` |
| Chat Client | **Go** | 1.22+ | Bubble Tea, Lip Gloss, Bubbles |
| Build System | GNU Make | 4.x | Orchestrates all three |
| C++ Build | CMake | 3.20+ | Links to Zig `.so` |
| Containerized Testing | Docker Compose | v2 | 3-node chain topology |

---

*AetherMesh — Built for the mesh, not the cloud.*
