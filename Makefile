
CC ?= gcc

CPPFLAGS += $(shell pkg-config --cflags libqb)
CFLAGS += -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs libqb)

# [新增] systemd Watchdog 依赖
SYSTEMD_CFLAGS := $(shell pkg-config --cflags libsystemd)
SYSTEMD_LIBS := $(shell pkg-config --libs libsystemd)

.PHONY: all clean

# [保留] 仍然只编译 KCSM 和模拟发送端
all: lhb-monitor-demo lhb-sender-demo

# [修改] KCSM 链接 libsystemd
lhb-monitor-demo: monitor.c protocol.h
	$(CC) $(CPPFLAGS) $(SYSTEMD_CFLAGS) $(CFLAGS) -o $@ monitor.c $(LDLIBS) $(SYSTEMD_LIBS)

# [保留] 模拟发送端不需要 libsystemd
lhb-sender-demo: sender.c protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ sender.c $(LDLIBS)

# [保留] 清理编译产物
clean:
	rm -f lhb-monitor-demo lhb-sender-demo