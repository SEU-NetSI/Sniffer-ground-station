CC ?= cc

# ==========================================
# 1. 平台与 libusb 环境检测
# ==========================================
ifeq ($(OS),Windows_NT)
PLATFORM := windows
else
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
PLATFORM := macos
else
PLATFORM := unix
endif
endif

# ==========================================
# 2. 编译选项融合
# ==========================================
CPPFLAGS += -I. -I..
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -DSNIFFER_STORAGE_COMPILE
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

ifeq ($(PLATFORM),windows)
CC := gcc
WINDOWS_LIBUSB_PREFIX ?= $(MINGW_PREFIX)
ifneq ($(strip $(WINDOWS_LIBUSB_PREFIX)),)
CPPFLAGS += -I$(WINDOWS_LIBUSB_PREFIX)/include
CPPFLAGS += -I$(WINDOWS_LIBUSB_PREFIX)/include/libusb-1.0
endif
LDLIBS += -lusb-1.0
else ifeq ($(PLATFORM),macos)
PKG_CONFIG ?= pkg-config
LIBUSB_PREFIX := $(shell brew --prefix libusb 2>/dev/null)
LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

CPPFLAGS += -I../AdHocUWB/Inc
CPPFLAGS += $(LIBUSB_CFLAGS)
ifneq ($(strip $(LIBUSB_PREFIX)),)
CPPFLAGS += -I$(LIBUSB_PREFIX)/include
CPPFLAGS += -I$(LIBUSB_PREFIX)/include/libusb-1.0
ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_LIBS := -L$(LIBUSB_PREFIX)/lib -lusb-1.0
endif
endif
ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_LIBS := -lusb-1.0
endif

CPPFLAGS += -DSNIFFER_MACOS_PLOT
LDLIBS += $(LIBUSB_LIBS)
else
$(error Unsupported platform: $(UNAME_S))
endif

# ==========================================
# 3. 目标定义
# ==========================================
TARGET_STORAGE     := sniffer_storage
TARGET_PC_UWB      := pc_to_uwb
TARGET_TWOWAY      := twoway_transport
TARGET_GROUND_CTRL := ground_control_windows
TARGET_GROUND_ADMIN := ground_admin
TEST_TARGET        := test_sniffer_storage

.PHONY: all clean test

ifeq ($(PLATFORM),windows)
ALL_TARGETS := $(TARGET_STORAGE) $(TARGET_PC_UWB) $(TARGET_TWOWAY) $(TARGET_GROUND_CTRL) $(TARGET_GROUND_ADMIN)
else
ALL_TARGETS := $(TARGET_STORAGE) $(TARGET_PC_UWB) $(TARGET_TWOWAY) $(TARGET_GROUND_ADMIN)
endif

all: $(ALL_TARGETS)

# ==========================================
# 4. 原有 sniffer_storage 编译规则
# ==========================================
SOURCES_STORAGE := sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c libusb_transport.c
ifeq ($(PLATFORM),macos)
SOURCES_STORAGE += sniffer_plot_macos.c
endif
OBJECTS_STORAGE := $(SOURCES_STORAGE:.c=.o)

$(TARGET_STORAGE): $(OBJECTS_STORAGE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS_STORAGE) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ==========================================
# 5. 新增：PC 到 UWB 及双向传输工具、地面站控制编译规则
# ==========================================
$(TARGET_PC_UWB): pc_to_uwb.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

ifeq ($(PLATFORM),macos)
$(TARGET_TWOWAY): CFLAGS += -Wno-unused-function
endif
$(TARGET_TWOWAY): twoway_transport.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -pthread

$(TARGET_GROUND_ADMIN): ground_admin.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

ifeq ($(PLATFORM),windows)
$(TARGET_GROUND_CTRL): ground_control_windows.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
endif

# ==========================================
# 6. 测试与清理
# ==========================================
TEST_SOURCES := test_sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c

$(TEST_TARGET): $(TEST_SOURCES) sniffer_capture.h sniffer_buffered_transport.h sniffer_hex.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SOURCES)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET_STORAGE) $(TARGET_PC_UWB) $(TARGET_TWOWAY) $(TARGET_GROUND_CTRL) $(TARGET_GROUND_ADMIN) $(TEST_TARGET) $(OBJECTS_STORAGE)
	rm -rf $(TEST_TARGET).dSYM
	rm -f *.exe
