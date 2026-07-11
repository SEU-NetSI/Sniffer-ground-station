CC ?= cc
PKG_CONFIG ?= pkg-config

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

CPPFLAGS += -I. $(LIBUSB_CFLAGS)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(LIBUSB_LIBS)

TARGET := sniffer_storage
SOURCES := sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c libusb_transport.c
ifeq ($(shell uname -s),Darwin)
SOURCES += sniffer_plot_macos.c
CPPFLAGS += -DSNIFFER_MACOS_PLOT
endif
OBJECTS := $(SOURCES:.c=.o)
TEST_TARGET := test_sniffer_storage
TEST_SOURCES := test_sniffer_storage.c sniffer_capture.c sniffer_buffered_transport.c sniffer_hex.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TEST_TARGET): $(TEST_SOURCES) sniffer_capture.h sniffer_buffered_transport.h sniffer_hex.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SOURCES)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJECTS)
	rm -rf $(TEST_TARGET).dSYM
