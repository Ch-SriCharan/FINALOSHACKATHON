# ==============================================================================
# Makefile for Multi-Patient Monitoring Simulator
# Targets: patient_monitor, race_demo, test, clean, review-alerts
# ==============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -O2 -Iinclude -D_GNU_SOURCE
LDFLAGS = -pthread -lrt

SRC_DIR = src
INC_DIR = include
OBJ_DIR = build
BIN_MAIN = patient_monitor
BIN_RACE = race_demo

OBJS = $(OBJ_DIR)/main.o \
       $(OBJ_DIR)/common.o \
       $(OBJ_DIR)/sensor.o \
       $(OBJ_DIR)/alert_detector.o \
       $(OBJ_DIR)/fifo_cmd.o \
       $(OBJ_DIR)/monitor.o

.PHONY: all clean test review-alerts help

all: $(BIN_MAIN) $(BIN_RACE)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(INC_DIR)/common.h $(INC_DIR)/sync_helpers.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_MAIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BIN_RACE): tests/race_demo.c
	$(CC) $(CFLAGS) -o $@ tests/race_demo.c $(LDFLAGS)

test: all
	chmod +x tests/run_tests.sh
	./tests/run_tests.sh

review-alerts: $(BIN_MAIN)
	./$(BIN_MAIN) --review-alerts

clean:
	rm -rf $(OBJ_DIR) $(BIN_MAIN) $(BIN_RACE) alerts.log
	rm -f /dev/shm/patient_monitor_shm /dev/shm/race_demo_shm /tmp/patient_monitor_fifo

help:
	@echo "Available Makefile targets:"
	@echo "  make              - Compile all binaries (patient_monitor and race_demo)"
	@echo "  make test         - Run the comprehensive automated test suite"
	@echo "  make review-alerts- Display past abnormal alerts from alerts.log"
	@echo "  make clean        - Remove build artifacts, binaries, and IPC files"
