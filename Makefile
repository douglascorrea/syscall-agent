CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS ?=
LDLIBS  := $(shell curl-config --libs)
INCS    := -Isrc -Ivendor $(shell curl-config --cflags)

BUILD   := build
BIN     := $(BUILD)/agent

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  OS_COMPAT_SRC := src/os_compat_darwin.c
else
  OS_COMPAT_SRC := src/os_compat_linux.c
endif

SRC := \
  src/main.c \
  src/tui.c \
  src/agent.c \
  src/openrouter.c \
  src/tools.c \
  src/tools_meta.c \
  src/tools_termux.c \
  src/tools_fs.c \
  src/tools_proc.c \
  src/tools_watch.c \
  src/tools_net.c \
  $(OS_COMPAT_SRC) \
  src/memory.c \
  src/http.c \
  src/util.c \
  vendor/cJSON.c

OBJ := $(SRC:%.c=$(BUILD)/%.o)

.PHONY: all clean run test

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "built $@"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

clean:
	rm -rf $(BUILD)

run: $(BIN)
	./$(BIN) $(ARGS)

test: $(BUILD)/tui_test $(BUILD)/agent_events_test $(BUILD)/security_test $(BUILD)/tools_meta_test $(BUILD)/tools_termux_test
	./$(BUILD)/tui_test
	./$(BUILD)/agent_events_test
	./$(BUILD)/security_test
	./$(BUILD)/tools_meta_test
	./$(BUILD)/tools_termux_test

$(BUILD)/tui_test: tests/tui_test.c src/tui.c src/util.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tui_test.c src/tui.c src/util.c -o $@

$(BUILD)/agent_events_test: tests/agent_events_test.c src/agent.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/agent_events_test.c src/agent.c src/util.c vendor/cJSON.c -o $@

$(BUILD)/security_test: tests/security_test.c src/tools_proc.c src/tools_fs.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/security_test.c src/tools_proc.c src/tools_fs.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/tools_meta_test: tests/tools_meta_test.c src/tools.c src/tools_meta.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tools_meta_test.c src/tools.c src/tools_meta.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/tools_termux_test: tests/tools_termux_test.c src/tools.c src/tools_meta.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tools_termux_test.c src/tools.c src/tools_meta.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@
