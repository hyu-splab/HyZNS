CC      := gcc
CFLAGS  := -O2 -g -Wall -Wextra -std=c11 -pthread
LDLIBS  := -lzbd -lpthread -lm

BUILD   := build

.PHONY: all ztree hy-ztree clean
all: ztree hy-ztree

$(BUILD):
	mkdir -p $(BUILD)

# ── ZTree (ZNS-only, ORWC latches) — paper "ZTree" ────────────────
ztree: | $(BUILD)
	$(CC) $(CFLAGS) -Iztree \
	  ztree/ztree_nlt.c ztree/ztree_zone.c ztree/ztree_main.c ztree/bench_main_ztree.c \
	  -o $(BUILD)/ztree $(LDLIBS)

# ── Hy-ZTree (F2FS-on-dm-hyzns CNS + hyznsd grow) — paper "Hy-ZTree" ──
hy-ztree: | $(BUILD)
	$(CC) $(CFLAGS) -Ihyztree \
	  hyztree/hyztree_nlt.c hyztree/hyztree_zone.c hyztree/hyztree_main.c hyztree/bench_main_hyztree.c \
	  -o $(BUILD)/hy-ztree $(LDLIBS)

clean:
	rm -rf $(BUILD)
