

# SHA-256 Distributed Hashing Service  
Client–Server System using Thread Pool, FIFO, and Pthreads

This project implements a distributed system for computing the SHA‑256 hash of files.  
The architecture is based on a **client–server model**, where multiple clients can request the hash computation of files, and the server processes these requests concurrently using a **thread pool**, **FIFO communication**, and **mutex/condition variables** for synchronization.

---

## Overview

The system computes the SHA‑256 hash of files using a concurrent server.  
SHA‑256 produces a 256‑bit (32‑byte) digest, typically represented as 64 hexadecimal characters.  
It is commonly used for data integrity verification and hashing.

The project uses:
- **Pthreads** with monitors  
- **FIFO** for inter-process communication  
- **Thread pool** for concurrency  
- **Cache** to avoid repeated computations  

---

## Architecture

### Client
Each client is a separate process. It:
- Reads the input file path and size  
- Creates a unique FIFO for receiving the response  
- Builds a request message following a defined protocol  
- Sends the request to the server’s main FIFO  
- Waits for the SHA‑256 result or an error message  

### Server
The server handles concurrent requests and performs the following tasks:

- **Main FIFO Management**  
  Keeps a FIFO open for receiving client requests.

- **Request Parsing**  
  Extracts file path, size, and client FIFO path.

- **Thread Pool Initialization**  
  Creates a fixed number of worker threads (4 in this implementation).

- **Work Queue Scheduling**  
  Requests are queued and sorted by file size (smaller files first).  
  Access is synchronized using mutex + condition variables.

- **Worker Thread Processing**  
  Each worker:
  - Checks if the hash is already in cache  
  - Detects duplicate requests and waits for ongoing computations  
  - Computes SHA‑256 if needed  
  - Updates the cache  
  - Sends the result to the client FIFO  

- **Synchronization**  
  Ensures thread‑safe access to shared resources (queue, cache, pending requests).

### Communication (FIFO)
All communication between client and server uses FIFO channels.

### Concurrency Model
A **thread pool** allows the server to process multiple requests simultaneously, improving responsiveness.

---

## Difficulties & Solutions

- **Concurrent Synchronization**  
  Solved using mutex + condition variables.

- **Duplicate Requests**  
  Implemented a “pending requests” mechanism to avoid repeated hash computations.

- **Message Parsing & Protocol**  
  Defined a clear protocol (`protocol.h`) with standardized formats.

---

## Future Improvements

- Replace linked‑list cache with **LRU cache**  
- Add **signal handlers** (SIGINT/SIGTERM) for graceful shutdown  
- Limit work queue size under heavy load  
- Make thread count and cache size configurable  
- Add client‑side timeouts to avoid indefinite blocking  

---

## Project Structure

```
client/
  client.c

common/
  protocol.h

server/
  server.c
  sha256_utils.c
  sha256_utils.h

CMakeLists.txt
```

---

## Build Instructions (CMake)

From the project root:

```bash
mkdir build
cd build
cmake ..
make
```

This generates the executables `server` and `client` inside the `build/` directory.

---

## How to Run

### Start the Server
```bash
cd build
./server
```
The server will start and listen on its main FIFO.

### Start One or More Clients
In separate terminals:

```bash
cd build
./client path/to/your/file.txt
```

You can run multiple clients concurrently to test the server’s parallel processing.

---

## Documentation
For further information, please refer to the documentation available in the `doc/` folder.
