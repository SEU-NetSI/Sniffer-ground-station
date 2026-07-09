CC ?= gcc
PKG_CONFIG ?= pkg-config

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)
BREW_LIBUSB_PREFIX := $(shell brew --prefix libusb 2>/dev/null)

ifeq ($(LIBUSB_CFLAGS),)
ifneq ($(BREW_LIBUSB_PREFIX),)
LIBUSB_CFLAGS := -I$(BREW_LIBUSB_PREFIX)/include
LIBUSB_LIBS := -L$(BREW_LIBUSB_PREFIX)/lib -lusb-1.0
else
LIBUSB_CFLAGS := -I/usr/include/libusb-1.0
LIBUSB_LIBS := -lusb-1.0
endif
endif

CFLAGS += $(LIBUSB_CFLAGS)
LDFLAGS += $(LIBUSB_LIBS)

SRC = sniffer_storage.c
TARGET = sniffer_storage

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
