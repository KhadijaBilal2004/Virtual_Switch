CC = gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pthread -g -O0
LDLIBS ?= -pthread -lrt

PROGRAMS = switch host swctl test_runner

.PHONY: all clean test
all: $(PROGRAMS)

switch: switch.c frame.c frame.h mac_table.c mac_table.h shm_stats.c shm_stats.h signals.c signals.h
	$(CC) $(CFLAGS) -o $@ switch.c frame.c mac_table.c shm_stats.c signals.c $(LDLIBS)

host: host.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ host.c frame.c $(LDLIBS)

swctl: swctl.c shm_stats.c shm_stats.h
	$(CC) $(CFLAGS) -o $@ swctl.c shm_stats.c $(LDLIBS)

test_runner: main.c frame.c frame.h mac_table.c mac_table.h
	$(CC) $(CFLAGS) -o $@ main.c frame.c mac_table.c $(LDLIBS)

test: test_runner
	./test_runner

clean:
	rm -f $(PROGRAMS)
