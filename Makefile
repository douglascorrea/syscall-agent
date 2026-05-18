CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS ?=
LDLIBS  := $(shell curl-config --libs) -pthread
INCS    := -Isrc -Ivendor $(shell curl-config --cflags)

BUILD   := build
BIN     := $(BUILD)/cezar

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
  src/openrouter_models.c \
  src/auth.c \
  src/codex_provider.c \
  src/extensions.c \
  src/tools.c \
  src/tools_meta.c \
  src/tools_termux.c \
  src/tools_fs.c \
  src/tools_proc.c \
  src/tools_watch.c \
  src/tools_net.c \
  $(OS_COMPAT_SRC) \
  src/session_store.c \
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

test: $(BUILD)/tui_test $(BUILD)/agent_events_test $(BUILD)/agent_conversation_test $(BUILD)/agent_streaming_test $(BUILD)/security_test $(BUILD)/tools_meta_test $(BUILD)/tools_termux_test $(BUILD)/extensions_test $(BUILD)/session_store_test $(BUILD)/codex_provider_test $(BUILD)/openrouter_models_test $(BUILD)/openrouter_stream_test
	./$(BUILD)/tui_test
	./$(BUILD)/agent_events_test
	./$(BUILD)/agent_conversation_test
	./$(BUILD)/agent_streaming_test
	./$(BUILD)/security_test
	./$(BUILD)/tools_meta_test
	./$(BUILD)/tools_termux_test
	./$(BUILD)/extensions_test
	./$(BUILD)/session_store_test
	./$(BUILD)/codex_provider_test
	./$(BUILD)/openrouter_models_test
	./$(BUILD)/openrouter_stream_test

$(BUILD)/tui_test: tests/tui_test.c src/tui.c src/auth.c src/extensions.c src/session_store.c src/openrouter_models.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tui_test.c src/tui.c src/auth.c src/extensions.c src/session_store.c src/openrouter_models.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/agent_events_test: tests/agent_events_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/agent_events_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/agent_conversation_test: tests/agent_conversation_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/agent_conversation_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/agent_streaming_test: tests/agent_streaming_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/agent_streaming_test.c src/agent.c src/codex_provider.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/security_test: tests/security_test.c src/tools_proc.c src/tools_fs.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/security_test.c src/tools_proc.c src/tools_fs.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/tools_meta_test: tests/tools_meta_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tools_meta_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/tools_termux_test: tests/tools_termux_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/tools_termux_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/extensions_test: tests/extensions_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/extensions_test.c src/tools.c src/tools_meta.c src/auth.c src/extensions.c src/session_store.c src/tools_termux.c src/tools_fs.c src/tools_proc.c src/tools_watch.c src/tools_net.c src/http.c src/memory.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/session_store_test: tests/session_store_test.c src/session_store.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/session_store_test.c src/session_store.c src/util.c vendor/cJSON.c -o $@

$(BUILD)/codex_provider_test: tests/codex_provider_test.c src/codex_provider.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/codex_provider_test.c src/codex_provider.c src/util.c vendor/cJSON.c -o $@

$(BUILD)/openrouter_models_test: tests/openrouter_models_test.c src/openrouter_models.c src/http.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/openrouter_models_test.c src/openrouter_models.c src/http.c src/util.c vendor/cJSON.c $(LDLIBS) -o $@

$(BUILD)/openrouter_stream_test: tests/openrouter_stream_test.c src/openrouter.c src/util.c vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) tests/openrouter_stream_test.c src/openrouter.c src/util.c vendor/cJSON.c -o $@
