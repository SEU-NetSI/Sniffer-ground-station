#ifndef SNIFFER_CAPTURE_H
#define SNIFFER_CAPTURE_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SNIFFER_META_SIZE 18u
#define SNIFFER_MAGIC 0x0000BBBBu
#define SNIFFER_DEFAULT_MAX_PAYLOAD 4096u

typedef enum {
    SNIFFER_IO_ERROR = -1,
    SNIFFER_IO_EOF = 0,
    SNIFFER_IO_DATA = 1,
    SNIFFER_IO_TIMEOUT = 2,
    SNIFFER_IO_INTERRUPTED = 3
} SnifferIoStatus;

typedef SnifferIoStatus (*SnifferReadFn)(void *context, uint8_t *buffer,
                                         size_t capacity, size_t *received);
typedef SnifferIoStatus (*SnifferWriteFn)(void *context, const uint8_t *buffer,
                                          size_t length, size_t *written);

typedef struct {
    void *context;
    SnifferReadFn read;
    SnifferWriteFn write;
} SnifferTransport;

typedef struct {
    uint32_t magic;
    uint16_t sender_address;
    uint16_t sequence;
    uint16_t message_length;
    uint64_t rx_time;
} SnifferMeta;

typedef struct {
    uint64_t records;
    uint64_t received_bytes;
    uint64_t stored_bytes;
    uint64_t magic_errors;
    uint64_t invalid_lengths;
    uint64_t truncated_meta;
    uint64_t truncated_payload;
    uint64_t read_timeouts;
    uint64_t transport_errors;
    uint64_t file_write_failures;
    uint64_t resynchronizations;
    uint64_t discarded_bytes;
} SnifferStats;

typedef struct {
    size_t max_payload;
    uint64_t max_records;
    int flush_each_record;
    int print_records;
    int print_payload;
    FILE *print_stream;
} SnifferCaptureOptions;

void sniffer_decode_meta(const uint8_t raw[SNIFFER_META_SIZE], SnifferMeta *meta);
int sniffer_capture(FILE *output, SnifferTransport *transport,
                    const SnifferCaptureOptions *options,
                    const volatile sig_atomic_t *running,
                    SnifferStats *stats);
int sniffer_transport_write_all(SnifferTransport *transport,
                                const uint8_t *data, size_t length,
                                uint64_t max_timeouts,
                                const volatile sig_atomic_t *running,
                                uint64_t *timeouts);
void sniffer_print_stats(FILE *stream, const SnifferStats *stats);

#endif
