# Deadlock Recovery with Minimal Work Loss
**Operating Systems Course Project | Course Code: IASWE204**  
*Reg. No: 25MIS0108, 25MIS0223*

A simulated operating systems deadlock detection and minimal-work-loss recovery engine. Features a high-performance C backend (C99 standard library only, with cross-platform socket support for Windows and POSIX) coupled with an interactive web frontend featuring dynamic inline SVG Wait-For Graph (WFG) visualization, minimal recovery candidate subset analysis, and comparative benchmarking against traditional greedy heuristics.

---

## 1. Project Structure

| File | Description |
| :--- | :--- |
| `deadlock.h` | Header file defining PCB, System, RecoveryPlan, Edge structures, limits, and analysis prototypes. |
| `deadlock.c` | Core logic: input validation, wait-for graph builder, Warshall cycle detection, bitmask subset simulation, minimality filters, and manual JSON builder. |
| `server.c` | Lightweight HTTP/1.1 server (`GET /`, `POST /api/analyze`) and CLI mode runner (`--cli`), supporting Windows Winsock2 and POSIX sockets. |
| `sample.txt` | Standard course project benchmark dataset (`LIMITS 60 2`, 6 processes, cycles P1-P2-P3 and P3-P4-P5). |
| `Makefile` | Build configuration targeting GCC on Windows (MinGW with `-lws2_32`) and Linux/macOS. |
| `README.md` | Complete architectural documentation, compilation instructions, and format guide. |
| `frontend/index.html` | Self-contained web interface with configuration editor, SVG Wait-For Graph generator, and plan comparison tables. |

---

## 2. Compilation and Execution

### Prerequisites
- GCC supporting C99 (`gcc`)
- On Windows: MinGW or MSYS2 (`-lws2_32` for Winsock)
- On Linux / macOS / WSL: Standard GCC and POSIX sockets

### Build Commands

#### Option A: Using Makefile
```bash
# Build binary
mingw32-make build       # On Windows (MinGW)
make build               # On Linux / macOS / WSL

# Clean build artifacts
mingw32-make clean       # On Windows
make clean               # On Linux / macOS / WSL
```

#### Option B: Direct GCC Compilation

**Windows (MinGW):**
```bash
gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -o deadlock_server.exe server.c deadlock.c -lws2_32
```

**Linux / macOS / WSL:**
```bash
gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -o deadlock_server server.c deadlock.c
```

---

## 3. Running the Application

### 1. Web Server Mode (Default)
Run the server executable from the project root directory:
```bash
# Windows
.\deadlock_server.exe

# Linux / macOS
./deadlock_server
```

Optionally specify a custom port as the first argument:
```bash
.\deadlock_server.exe 9000
```

Once running, navigate to:
```
http://localhost:8080
```
in any modern web browser.

### 2. Command-Line (CLI) Mode
The server supports batch/scripted execution directly from standard input without opening a network socket:
```bash
# Windows PowerShell
Get-Content sample.txt | .\deadlock_server.exe --cli

# Linux / macOS / bash
./deadlock_server --cli < sample.txt
```
This prints the resulting JSON analysis to stdout.

---

## 4. Input Specification Format

Input data consists of line-delimited records. Lines beginning with `#` and empty lines are treated as comments and ignored.

```
LIMITS <maxCost> <maxVictims>
<name> <completed> <restart> <priority> <holds> <waits>
...
```

### Parameter Details
- **`LIMITS <maxCost> <maxVictims>`**:
  - `maxCost`: Maximum acceptable cumulative work-loss cost for recovery (integer $\ge 0$).
  - `maxVictims`: Maximum number of processes that may be terminated (integer $\ge 0$).
- **Process Line**:
  - `<name>`: Alphanumeric process identifier (1–15 characters, `[A-Za-z0-9_]`).
  - `<completed>`: Units of work completed so far (integer $\ge 0$).
  - `<restart>`: Computational cost penalty required to restart the process (integer $\ge 0$).
  - `<priority>`: Process priority (integer $\ge 0$; higher values denote higher priority / importance).
  - `<holds>`: Comma-separated list of resources currently exclusively held (e.g. `R1,R2`) or `-` if none.
  - `<waits>`: Comma-separated list of resources requested / waited for (e.g. `R3`) or `-` if none.

### Example (`sample.txt`):
```text
LIMITS 60 2
P1 12 8  4 R1 R2
P2 30 15 1 R2 R3
P3 20 10 5 R3 R1,R4
P4 14 8  3 R4 R5
P5 35 15 2 R5 R3
P6 6  6  3 R6 R1
```

---

## 5. Architectural & Algorithmic Design

### A. Graph Construction & Deadlock Detection
1. **Resource Ownership**: Each resource can be held by at most one process.
2. **Wait-For Graph (WFG)**: A directed edge $P_i \to P_j$ exists if $P_i$ is waiting for a resource held by $P_j$. Self-edges ($i = j$) and requests for free (unheld) resources are ignored.
3. **Deadlock Identification (Warshall's Algorithm)**:
   Transitive closure $T$ is computed over the adjacency matrix. A process $P_i$ is in a deadlock state if and only if $T[i][i] = 1$ (it can reach itself via a directed cycle of length $\ge 1$).

### B. Minimal Work-Loss Recovery Optimization
To find the optimal recovery plan:
1. **Bitmask Exhaustive Search**: Evaluates every non-empty subset of processes $S \subseteq \{P_0, \dots, P_{n-1}\}$.
2. **Termination Simulation**: In subset $S$, all member processes are terminated; their held resources become available (free), and all outgoing/incoming edges involving $S$ are eliminated.
3. **Feasibility Criteria**:
   - The remaining subgraph contains zero directed cycles.
   - Total work-loss cost ($\sum_{p \in S} (completed_p + restart_p)$) $\le maxCost$.
   - Number of victims ($|S|$) $\le maxVictims$.
4. **Minimality Verification**: A plan $S$ is considered minimal if removing any single victim $v \in S$ restores at least one deadlock cycle. This eliminates redundant terminations.
5. **Ranking**: Minimal plans are sorted with feasible plans first, ascending cost, and ascending victim count. The top feasible plan is selected as optimal.

### C. Greedy Baseline Comparisons
The engine contrasts the optimal exhaustive plan against two standard single-victim greedy heuristics:
1. **Lowest Individual Cost**: Iteratively terminates the deadlocked process with the lowest individual work loss until no cycles remain.
2. **Lowest Priority**: Iteratively terminates the deadlocked process with the lowest priority (tie-breaking by lower cost) until no cycles remain.
