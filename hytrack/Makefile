# HYTRACK Monitoring Tool Makefile

CC = gcc
CFLAGS = -Wall -Wextra -g -O2
LDFLAGS = -lncurses

TARGET = hytrack
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj

# Source files
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/core/device_manager.c \
       $(SRC_DIR)/core/nvme_cmd.c \
       $(SRC_DIR)/common/ui_common.c \
       $(SRC_DIR)/common/history.c \
       $(SRC_DIR)/common/log_utils.c \
       $(SRC_DIR)/common/global_state.c \
       $(SRC_DIR)/modes/hyssd_mode.c \
       $(SRC_DIR)/modes/bbssd_mode.c \
       $(SRC_DIR)/modes/znssd_mode.c \
       $(SRC_DIR)/modes/multi_mode.c \
       $(SRC_DIR)/views/view_manager.c \
       $(SRC_DIR)/views/view_interface.c \
       $(SRC_DIR)/views/dashboard_view.c \
       $(SRC_DIR)/views/stats_view.c \
       $(SRC_DIR)/views/graph_view.c \
       $(SRC_DIR)/views/line_view.c \
       $(SRC_DIR)/views/debug_view.c \
       $(SRC_DIR)/views/log_view.c \
       $(SRC_DIR)/views/multi_view.c

# Object files
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Create required directories
DIRS = $(OBJ_DIR) \
       $(OBJ_DIR)/core \
       $(OBJ_DIR)/common \
       $(OBJ_DIR)/modes \
       $(OBJ_DIR)/views

.PHONY: all clean install uninstall run

all: $(DIRS) $(TARGET)

$(DIRS):
	mkdir -p $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)

# Create logs directory on first run
run: $(TARGET)
	@mkdir -p logs
	./$(TARGET)