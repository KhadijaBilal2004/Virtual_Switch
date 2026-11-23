# Virtual Switch

A Linux TCP-based, multithreaded learning switch written in C. The wire protocol is a 17-byte header followed by the declared payload length: 2-byte network-order payload length, 6-byte source MAC, 6-byte destination MAC, 2-byte network-order VLAN ID, and 1-byte priority (0 high, 1 normal, 2 low). Payloads are limited to 1500 bytes. Reads and writes handle partial I/O.

## Build and test

```sh
make
make test
```

The build uses `gcc -std=c11 -Wall -Wextra -Werror -g -O0 -pthread`. `make clean` removes generated executables.

## Run

Start the switch with the TCP listening port and maximum simultaneous host count (1–32):

```sh
./switch 5555 4
```

Each connected host occupies a port until it disconnects or an administrator shuts down that port. The free-port semaphore limits connections; additional clients wait up to one second and then receive `switch full`.

The included host client connects and broadcasts lines typed on stdin:

```sh
./host 127.0.0.1 5555 02:00:00:00:00:01
```

`swctl` controls and inspects a running switch:

```sh
./swctl show mac
./swctl show ports
./swctl stats
./swctl stats --watch
./swctl shutdown 0
./swctl up 0
./swctl vlan 1 20
./swctl mirror 1 2
./swctl clear mac
./swctl clear stats
```

Port indexes are zero-based. `shutdown N` disables the port, shuts down any active connection, wakes and joins its sender and receiver, and drains queued frames. `up N` enables the port for a future host connection; it does not create a host connection itself. `vlan N ID` assigns a normal port to VLAN 1–4094; the switch applies this VLAN to frames received from normal ports. Trunk ports preserve the incoming VLAN tag. `mirror SOURCE SNIFFER` copies frames sourced from `SOURCE` to the `SNIFFER` port's independent queue. The sniffer must connect as a host.

## Configuration and signals

The optional `vswitch.conf` in the current working directory supports one directive per line; blank lines and lines whose first non-whitespace character is `#` are ignored:

```text
vlan 0 10
vlan 1 10
trunk 2
```

A VLAN directive assigns a normal port's VLAN. A trunk directive marks the port as a trunk (which carries all VLANs). On startup the defaults are VLAN 1 and non-trunk; loading a config replaces these values for every configured port. `SIGHUP` reloads the file without restarting, and a malformed file is rejected as a whole without partially applying it. `SIGUSR1` writes the learned MAC/VLAN table to `khadija_mac_dump.txt`. `SIGINT` cleanly stops the switch.

The switch enforces a 64-frame bounded queue per egress priority, weighted 5:3:1 scheduling, a 100-frame/second token bucket per port, MAC aging after 300 seconds, a 1024-entry least-recently-used MAC table, and a four-learned-MAC per-port security limit. Live counters are stored in process-shared memory (`/vswitch_khadija`) and guarded by a process-shared mutex.

## Concurrency notes

A receiver and sender run per connected port. A shared frame reference count allows one broadcast to be queued to several ports without copying its payload. The maintenance thread refills tokens and ages MAC entries; the signal thread uses `sigwait`; the logger consumes an asynchronous message queue.

Lock order: lifecycle mutex, then port-state mutex; the port-state mutex is released before joining threads. MAC operations take one bucket lock before the table count/LRU mutex; eviction takes every bucket lock in increasing index order and then the count/LRU mutex, releasing in reverse order. Queue, token, logging, and shared-statistics locks are not nested with the MAC locks. Port shutdown never holds a queue/token lock while joining a worker.
