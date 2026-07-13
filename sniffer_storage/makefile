CC ?= cc
PKG_CONFIG ?= pkg-config

# ==========================================
# 1. Libusb 环境检测 (保留 macOS 兼容逻辑)
# ==========================================
LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

ifeq ($(strip $(LIBUSB_CFLAGS)),)
BREW_LIBUSB_PREFIX := $(shell brew --prefix libusb 2>/dev/null)
ifneq ($(strip $(BREW_LIBUSB_PREFIX)),)
LIBUSB_CFLAGS := -I$(BREW_LIBUSB_PREFIX)/include/libusb-1.0
LIBUSB_LIBS := -L$(BREW_LIBUSB_PREFIX)/lib -lusb-1.0
else
LIBUSB_CFLAGS := -I/usr/include/libusb-1.0
LIBUSB_LIBS := -lusb-1.0
endif
endif

# ==========================================
# 2. 编译选项融合
# ==========================================
# 加入了你的 -I.. 和 -DSNIFFER_STORAGE_COMPILE
CPPFLAGS += -I. -I.. $(LIBUSB_CFLAGS)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -DSNIFFER_STORAGE_COMPILE
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(LIBUSB_LIBS)

# ==========================================
# 3. 目标定义
# ==========================================
TARGET_STORAGE := sniffer_storage
TARGET_PC_UWB  := pc_to_uwb
TARGET_TWOWAY  := twoway_transport
TEST_TARGET    := test_sniffer_storage

.PHONY: all clean test

# 默认一次性编译所有工具
all: $(TARGET_STORAGE) $(TARGET_PC_UWB) $(TARGET_TWOWAY)

# ==========================================
# 4. 原有 sniffer_storage 编译规则
# ==========================================
SOURCES_STORAGE := sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c libusb_transport.c
ifeq ($(shell uname -s),Darwin)
SOURCES_STORAGE += sniffer_plot_macos.c
CPPFLAGS += -DSNIFFER_MACOS_PLOT
endif
OBJECTS_STORAGE := $(SOURCES_STORAGE:.c=.o)

$(TARGET_STORAGE): $(OBJECTS_STORAGE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS_STORAGE) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ==========================================
# 5. 新增：PC 到 UWB 及双向传输工具编译规则
# ==========================================
$(TARGET_PC_UWB): pc_to_uwb.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# 注意：twoway_transport 链接时追加了 -pthread
$(TARGET_TWOWAY): twoway_transport.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -pthread

# ==========================================
# 6. 测试与清理
# ==========================================
TEST_SOURCES := test_sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c

$(TEST_TARGET): $(TEST_SOURCES) sniffer_capture.h sniffer_buffered_transport.h sniffer_hex.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SOURCES)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET_STORAGE) $(TARGET_PC_UWB) $(TARGET_TWOWAY) $(TEST_TARGET) $(OBJECTS_STORAGE)
	rm -rf $(TEST_TARGET).dSYM
	rm -f *.exe