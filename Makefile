CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c11 -pthread -O2
LDLIBS ?= -lcjson

BUILD_DIR := build
SERVER_BIN := $(BUILD_DIR)/server
CLIENT_BIN := $(BUILD_DIR)/client
JSON_FILES := data/users.json data/books.json \
	data/borrow_records.json data/operation_logs.json

.DEFAULT_GOAL := all

all: $(SERVER_BIN) $(CLIENT_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SERVER_BIN): server/main.c server/app.c server/app.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) server/main.c server/app.c -o $@ $(LDLIBS)

$(CLIENT_BIN): client/main.c client/app.c client/app.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) client/main.c client/app.c -o $@ $(LDLIBS)

debug: CFLAGS := -Wall -Wextra -Wpedantic -std=c11 -pthread -O0 -g3
debug: clean all

check-json:
	@for file in $(JSON_FILES); do \
		python3 -m json.tool $$file >/dev/null || exit 1; \
	done
	@echo "JSON validation passed."

test: all check-json
	python3 tests/integration_test.py

run-server: $(SERVER_BIN)
	./$(SERVER_BIN)

run-client: $(CLIENT_BIN)
	./$(CLIENT_BIN)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean debug check-json test run-server run-client
