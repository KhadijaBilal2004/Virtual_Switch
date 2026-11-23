# Virtual Switch implementation report

## Architecture and thread diagram

The process owns a TCP listener, a UNIX-domain administration listener, a striped MAC table, and a process-shared statistics mapping. The main thread accepts host sockets, assigns them through the free-port semaphore, and periodically reaps ports whose workers have stopped.

```text
                         +--------------------+
 TCP hosts <-----------> | Main accept/reaper | <----> free-port semaphore
                         +---------+----------+
                                   | per connection
                    +--------------+---------------+
                    |                              |
             +------v------+                +------v------+
             | Port receiver| -- shared --> | Port sender |
             | full-frame IO|    refcount    | QoS + tokens|
             +------+-------+                +------+------+
                    |                              |
                    +----- striped MAC table ------+

 swctl <---- UNIX socket ----> Admin thread ---- shared-memory counters
 SIGHUP/SIGINT/SIGUSR1 ------> sigwait thread
 Port workers ----------------> asynchronous log queue ----> Logger thread
 Maintenance thread: token refill and MAC aging
```

Each connected port has a receiver and sender. The receiver parses complete frames, learns source MAC/VLAN entries, checks port security, and enqueues frames without waiting for queue capacity. The sender selects among three bounded priority queues using a 5:3:1 schedule and waits on a token condition when rate-limited. One reference-counted frame may be referenced by multiple egress queues. A maintenance thread refills tokens at 100 frames/second per port and expires entries after 300 seconds. A dedicated signal thread calls `sigwait`; logging is performed by a separate consumer thread. `swctl` communicates over a UNIX socket and obtains live counters from shared memory.

## Locks and lock order

* `port_lock` protects connection, enabled/active, VLAN, trunk, and mirror state. The lifecycle mutex serializes accept/reap and administrative shutdown. When both are needed, acquire lifecycle before port state. Workers never acquire the lifecycle mutex. Shutdown releases the port-state mutex before waking/joining workers.
* Every MAC bucket has its own mutex. Lookup and ordinary learning hold only that bucket; the monotonically increasing LRU use stamp is atomic. Table count and per-port learned-MAC counts use `count_lock`. Operations that need all buckets (LRU eviction and clear) acquire them in ascending bucket order, then `count_lock`, and release in reverse. Ordinary mutations take one bucket before `count_lock`.
* Every priority queue has its own mutex; a per-port work condition wakes the sender across queues. Token waits use a separate mutex/condition. The sender's work-lock-to-queue-lock check has no reverse nested acquisition: producers release a queue lock before signaling work.
* The logger queue, process-shared statistics mutex, and signal wait are separate. Shared counters are copied while holding the process-shared mutex; output occurs after releasing it.

## Correctness issue found and fixed

The original forwarding path did not send an unknown unicast to any egress port: it only queued a frame when the destination MAC was already in the table. This contradicted the specified learning-switch behavior and dropped the first frame from a new host to an unknown destination. The forwarding path now floods unknown destinations (and broadcasts) only within the frame's VLAN, while known destinations are sent only to their learned port. I verified this with two live TCP clients: the first unknown unicast reached the other client; after both source MACs were learned, a unicast reached only its learned destination. The same probe sent a frame in small writes and confirmed the receiver reconstructed the complete frame.

Other related fixes include true least-recently-used eviction at the 1024-entry table cap; atomic table clearing; accurate per-port learned-MAC counts; wake/stop/join/drain handling for live port shutdown; a reusable enabled state for `up`; and race-safe port/config/counter access. Partial reads/writes, mid-frame disconnects, SIGPIPE/EPIPE, shared-frame lifetime, and MAC/VLAN table access are handled explicitly.

## Implemented tiers and validation

Tier 1 items 1–5 are implemented: fixed wire framing with full I/O, bounded nonblocking egress queues, shared frames with reference counts, striped/aged/LRU MAC learning and port security, and timed semaphore admission. Tier 2 items 6–13 are implemented: weighted priority scheduling, token-bucket limiting, VLAN/trunk forwarding, mirroring, live admin commands, shared-memory statistics, `sigwait` signal handling/config reload/MAC dump, and queued timestamped logging. Tier 3 bonus items (two-switch pipe link, crash supervisor, and benchmark comparison) are not implemented.

The build is configured for the course flags (`gcc -std=c11 -Wall -Wextra -Werror -g -O0 -pthread`). Validation performed: `make`, `make test`; the test runner checks frame serialization, VLAN separation, MAC moves, table capacity, LRU victim selection, and per-port counts. Runtime checks exercised host learning, unknown and known unicast forwarding, partial writes, `swctl` show/shutdown/up/VLAN/mirror/clear/stats commands, valid and rejected SIGHUP reloads, SIGUSR1 dump, active-port shutdown, and SIGINT cleanup. ThreadSanitizer and Valgrind were not run in this environment, so their clean status is not claimed.
