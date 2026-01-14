# Makefile for _memcpy_s_chk Hook Library (MIPS 24Kc)

# Cross-compiler configuration
# Adjust this to your MIPS toolchain path
CROSS_COMPILE ?= mips-openwrt-linux-

CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

# Target architecture flags for MIPS 24Kc
ARCH_FLAGS = -march=24kc -mtune=24kc -mabi=32

# Compiler flags
CFLAGS = $(ARCH_FLAGS) -fPIC -Wall -O2
LDFLAGS = $(ARCH_FLAGS) -shared -ldl

# Output
TARGET = fifo_hook.so
SOURCE = fifo_hook.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built $(TARGET) successfully"
	@file $(TARGET)

strip: $(TARGET)
	$(STRIP) $(TARGET)
	@echo "Stripped $(TARGET)"

clean:
	rm -f $(TARGET)

# Example usage message
help:
	@echo "_memcpy_s_chk Hook Library Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make                  - Build the hook library"
	@echo "  make strip            - Strip debug symbols"
	@echo "  make clean            - Remove build artifacts"
	@echo ""
	@echo "To use the hook library:"
	@echo "  LD_PRELOAD=./$(TARGET) /path/to/your/application"
	@echo ""
	@echo "Cross-compiler settings:"
	@echo "  CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo ""
	@echo "To use a different toolchain:"
	@echo "  make CROSS_COMPILE=mips-linux-gnu-"
