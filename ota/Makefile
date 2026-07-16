CC ?= cc

TARGET := ota_flasher
SOURCES := ota_flasher.c
OBJECTS := $(SOURCES:.c=.o)
DEPS := $(OBJECTS:.o=.d)

CPPFLAGS += -I. -I../AdHocUWB/Inc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -MMD -MP

.PHONY: all debug release clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

debug: CFLAGS := $(filter-out -O%,$(CFLAGS)) -O0 -g3
debug: clean all

release: CFLAGS := $(filter-out -O% -g%,$(CFLAGS)) -O2 -DNDEBUG
release: clean all

clean:
	$(RM) $(TARGET) $(OBJECTS) $(DEPS)

-include $(DEPS)
