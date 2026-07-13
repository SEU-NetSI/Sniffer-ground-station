#ifndef SNIFFER_BUFFERED_TRANSPORT_H
#define SNIFFER_BUFFERED_TRANSPORT_H

#include "sniffer_capture.h"

typedef struct {
    SnifferTransport upstream;
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
    size_t length;
    SnifferIoStatus pending_status;
} SnifferBufferedTransport;

int sniffer_buffered_transport_init(SnifferBufferedTransport *buffered,
                                    SnifferTransport upstream,
                                    uint8_t *buffer, size_t capacity);
SnifferTransport sniffer_buffered_transport_adapter(
    SnifferBufferedTransport *buffered);

#endif
