#include "sniffer_buffered_transport.h"

#include <string.h>

static SnifferIoStatus buffered_read(void *context, uint8_t *output,
                                     size_t capacity, size_t *received) {
    SnifferBufferedTransport *buffered = context;
    if (capacity == 0) {
        *received = 0;
        return SNIFFER_IO_ERROR;
    }

    if (buffered->offset == buffered->length) {
        buffered->offset = 0;
        buffered->length = 0;
        size_t upstream_received = 0;
        SnifferIoStatus status = buffered->upstream.read(
            buffered->upstream.context, buffered->buffer,
            buffered->capacity, &upstream_received);
        if (upstream_received > buffered->capacity) {
            *received = 0;
            return SNIFFER_IO_ERROR;
        }
        buffered->length = upstream_received;
        buffered->pending_status = status;
        if (upstream_received == 0) {
            *received = 0;
            return status;
        }
    }

    size_t available = buffered->length - buffered->offset;
    size_t chunk = capacity < available ? capacity : available;
    memcpy(output, buffered->buffer + buffered->offset, chunk);
    buffered->offset += chunk;
    *received = chunk;
    SnifferIoStatus status = buffered->pending_status;
    buffered->pending_status = SNIFFER_IO_DATA;
    return status;
}

static SnifferIoStatus buffered_write(void *context, const uint8_t *data,
                                      size_t length, size_t *written) {
    SnifferBufferedTransport *buffered = context;
    if (buffered->upstream.write == NULL) {
        *written = 0;
        return SNIFFER_IO_ERROR;
    }
    return buffered->upstream.write(buffered->upstream.context, data,
                                    length, written);
}

int sniffer_buffered_transport_init(SnifferBufferedTransport *buffered,
                                    SnifferTransport upstream,
                                    uint8_t *buffer, size_t capacity) {
    if (buffered == NULL || upstream.read == NULL || buffer == NULL || capacity == 0) {
        return -1;
    }
    *buffered = (SnifferBufferedTransport){
        .upstream = upstream,
        .buffer = buffer,
        .capacity = capacity,
        .pending_status = SNIFFER_IO_DATA,
    };
    return 0;
}

SnifferTransport sniffer_buffered_transport_adapter(
    SnifferBufferedTransport *buffered) {
    SnifferTransport adapter = {
        .context = buffered,
        .read = buffered_read,
        .write = buffered_write,
    };
    return adapter;
}
