CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS ?=
LDLIBS  := $(shell curl-config --libs)
INCS    := -Isrc -Ivendor $(shell curl-config --cflags)

BUILD   := build
BIN     := $(BUILD)/agent

SRC := \
  src/main.c \
  src/agent.c \
  src/openrouter.c \
  src/tools.c \
  src/memory.c \
  src/http.c \
  src/util.c \
  vendor/cJSON.c

OBJ := $(SRC:%.c=$(BUILD)/%.o)

.PHONY: all clean run

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
