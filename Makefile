# virte1000.device — Modular Makefile.

CC     = ppc-amigaos-gcc
STRIP  = ppc-amigaos-strip

CFLAGS = -mcrt=newlib -mhard-float -O2 -Wall -Wextra \
         -D__PPC__ -D__USE_OLD_TIMEVAL__ \
         -I./include

DEVLDFLAGS = -nostartfiles \
             -Wl,-z,common-page-size=4096 \
             -Wl,-z,max-page-size=4096

TESTLDFLAGS = -lauto

BUILD    = build
DEV      = $(BUILD)/virte1000.device
DEV_DBG  = $(BUILD)/virte1000.device.debug

TEST_NAMES = testopen testdeviceq testmac testirq testonline testtx testrx testtx_cooked testrx_cooked testrxtask testroadshow testdiag testsizeof testdqbuf testcfgbuf testbsdadd testquery teststat teststress
TESTS      = $(patsubst %,$(BUILD)/%,$(TEST_NAMES))

DEV_SRCS = src/device.c \
           src/Init.c \
           src/Open.c \
           src/Close.c \
           src/Expunge.c \
           src/BeginIO.c \
           src/unit_task.c \
           src/hw/pci.c \
           src/hw/hw_init.c \
           src/hw/irq.c \
           src/hw/rx.c \
           src/hw/tx.c

DEV_OBJS     = $(patsubst src/%.c,$(BUILD)/%.o,$(DEV_SRCS))
DEV_OBJS_DBG = $(patsubst src/%.c,$(BUILD)/%.dbg.o,$(DEV_SRCS))

HEADERS = include/version.h include/virte1000.h include/e1000_regs.h

.PHONY: all clean test help

all: $(DEV) $(DEV_DBG) $(TESTS)

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/hw

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.dbg.o: src/%.c $(HEADERS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DDEBUG -g -c $< -o $@

# Release device: stripped
$(DEV): $(DEV_OBJS)
	$(CC) $(DEV_OBJS) -o $@ $(DEVLDFLAGS)
	$(STRIP) --strip-all $@

# Debug device: symbols kept
$(DEV_DBG): $(DEV_OBJS_DBG)
	$(CC) $(DEV_OBJS_DBG) -o $@ $(DEVLDFLAGS)

# Generic test-binary rule
$(BUILD)/%: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(TESTLDFLAGS)

test: $(TESTS)

clean:
	rm -rf $(BUILD)

help:
	@echo "virte1000.device build system"
	@echo ""
	@echo "  make          — build device + test"
	@echo "  make clean    — remove build/"
