CC ?= gcc
CPPFLAGS += $(shell pkg-config --cflags libqb)
CFLAGS += -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs libqb)

.PHONY: all clean
all: lhb-monitor-demo lhb-sender-demo

lhb-monitor-demo: monitor.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ monitor.c $(LDLIBS)

lhb-sender-demo: sender.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ sender.c $(LDLIBS)

clean:
	rm -f lhb-monitor-demo lhb-sender-demo
