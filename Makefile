CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=
MINGW_CC ?= x86_64-w64-mingw32-gcc
SOURCE_DATE_EPOCH ?= 1700000000
export SOURCE_DATE_EPOCH

ifeq ($(OS),Windows_NT)
ifeq ($(origin CC),default)
CC := gcc
endif
TARGET := dnsbench.exe
LDLIBS += -lws2_32
RM_CMD := del /Q
RUN := .\dnsbench.exe
else
TARGET := dnsbench
RM_CMD := rm -f
RUN := ./dnsbench
endif

.PHONY: all clean test windows

all: $(TARGET)

$(TARGET): dnsbench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: $(TARGET)
	$(RUN) -n 1 -t 100 servers.example.txt

windows: dnsbench.c
	$(MINGW_CC) $(CFLAGS) $(LDFLAGS) -o dnsbench.exe $< -lws2_32

clean:
	-$(RM_CMD) dnsbench dnsbench.exe
