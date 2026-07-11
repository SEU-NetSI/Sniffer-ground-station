#include "sniffer_capture.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    EXACT_ERROR = -1,
    EXACT_EOF = 0,
    EXACT_OK = 1,
    EXACT_STOPPED = 2
} ExactResult;

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t *data) {
    uint64_t value = 0;
    for (unsigned int index = 0; index < 8; index++) {
        value |= (uint64_t)data[index] << (index * 8);
    }
    return value;
}

void sniffer_decode_meta(const uint8_t raw[SNIFFER_META_SIZE], SnifferMeta *meta) {
    meta->magic = read_le32(raw);
    meta->sender_address = read_le16(raw + 4);
    meta->sequence = read_le16(raw + 6);
    meta->message_length = read_le16(raw + 8);
    meta->rx_time = read_le64(raw + 10);
}

static int is_running(const volatile sig_atomic_t *running) {
    return running == NULL || *running != 0;
}

static ExactResult read_exact(SnifferTransport *transport, uint8_t *buffer,
                              size_t length, const volatile sig_atomic_t *running,
                              SnifferStats *stats, size_t *total_read) {
    size_t total = 0;
    while (total < length && is_running(running)) {
        size_t received = 0;
        SnifferIoStatus status = transport->read(
            transport->context, buffer + total, length - total, &received);
        if (received > length - total) {
            stats->transport_errors++;
            *total_read = total;
            return EXACT_ERROR;
        }
        if (received > 0) {
            total += received;
            stats->received_bytes += received;
        }
        if (status == SNIFFER_IO_DATA) {
            if (received == 0) {
                stats->transport_errors++;
                *total_read = total;
                return EXACT_ERROR;
            }
            continue;
        }
        if (status == SNIFFER_IO_TIMEOUT) {
            stats->read_timeouts++;
            continue;
        }
        if (status == SNIFFER_IO_INTERRUPTED) {
            continue;
        }
        *total_read = total;
        if (status == SNIFFER_IO_EOF) {
            return EXACT_EOF;
        }
        stats->transport_errors++;
        return EXACT_ERROR;
    }
    *total_read = total;
    return total == length ? EXACT_OK : EXACT_STOPPED;
}

static int magic_matches(const uint8_t window[4]) {
    static const uint8_t magic[4] = {0xBB, 0xBB, 0x00, 0x00};
    return memcmp(window, magic, sizeof(magic)) == 0;
}

static ExactResult read_next_meta(SnifferTransport *transport,
                                  uint8_t raw[SNIFFER_META_SIZE],
                                  const volatile sig_atomic_t *running,
                                  SnifferStats *stats) {
    size_t count = 0;
    ExactResult result = read_exact(transport, raw, 4, running, stats, &count);
    if (result != EXACT_OK) {
        if ((result == EXACT_EOF || result == EXACT_ERROR) && count > 0) {
            stats->truncated_meta++;
        }
        return result;
    }

    if (!magic_matches(raw)) {
        stats->magic_errors++;
        uint64_t discarded = 0;
        while (is_running(running) && !magic_matches(raw)) {
            memmove(raw, raw + 1, 3);
            count = 0;
            result = read_exact(transport, raw + 3, 1, running, stats, &count);
            if (result != EXACT_OK) {
                if (result == EXACT_EOF && count > 0) {
                    stats->truncated_meta++;
                }
                stats->discarded_bytes += discarded + 4;
                return result;
            }
            discarded++;
        }
        stats->discarded_bytes += discarded;
        if (!is_running(running)) {
            return EXACT_STOPPED;
        }
        stats->resynchronizations++;
    }

    count = 0;
    result = read_exact(transport, raw + 4, SNIFFER_META_SIZE - 4,
                        running, stats, &count);
    if (result == EXACT_EOF || result == EXACT_ERROR) {
        stats->truncated_meta++;
    }
    return result;
}

static int write_file_all(FILE *output, const uint8_t *data, size_t length,
                          SnifferStats *stats) {
    size_t total = 0;
    while (total < length) {
        size_t written = fwrite(data + total, 1, length - total, output);
        if (written == 0) {
            stats->file_write_failures++;
            return -1;
        }
        total += written;
        stats->stored_bytes += written;
    }
    return 0;
}

static void print_meta(FILE *stream, const SnifferMeta *meta,
                       uint64_t packet_count) {
    fprintf(stream, "Packet %" PRIu64 "\n", packet_count);
    fprintf(stream, "Sniffer_Meta_t:\n");
    fprintf(stream, "  Magic: 0x%08" PRIX32 "\n", meta->magic);
    fprintf(stream, "  Sender Address: %" PRIu16 "\n", meta->sender_address);
    fprintf(stream, "  Sequence Number: %" PRIu16 "\n", meta->sequence);
    fprintf(stream, "  Message Length: %" PRIu16 "\n", meta->message_length);
    fprintf(stream, "  RX Time: 0x%016" PRIX64 "\n", meta->rx_time);
    fflush(stream);
}

static void print_payload_hex(FILE *stream, const uint8_t *payload,
                              uint16_t length) {
    fputs("Payload: ", stream);
    for (uint16_t index = 0; index < length; index++) {
        fprintf(stream, "%02X", payload[index]);
    }
    fputc('\n', stream);
    fflush(stream);
}

int sniffer_capture(FILE *output, SnifferTransport *transport,
                    const SnifferCaptureOptions *options,
                    const volatile sig_atomic_t *running,
                    SnifferStats *stats) {
    if (output == NULL || transport == NULL || transport->read == NULL ||
        options == NULL || stats == NULL || options->max_payload == 0 ||
        options->max_payload > UINT16_MAX ||
        (options->print_records && options->print_stream == NULL) ||
        (options->print_payload && !options->print_records)) {
        return -1;
    }

    uint8_t *payload = malloc(options->max_payload);
    if (payload == NULL) {
        return -1;
    }

    int status = 0;
    uint64_t packet_count = 0;
    while (is_running(running) &&
           (options->max_records == 0 || stats->records < options->max_records)) {
        uint8_t raw[SNIFFER_META_SIZE];
        SnifferMeta meta;
        ExactResult result = read_next_meta(transport, raw, running, stats);
        if (result == EXACT_EOF || result == EXACT_STOPPED) {
            break;
        }
        if (result == EXACT_ERROR) {
            status = -1;
            break;
        }

        sniffer_decode_meta(raw, &meta);
        packet_count++;
        if (options->print_records) {
            print_meta(options->print_stream, &meta, packet_count);
        }
        if (meta.message_length > options->max_payload) {
            stats->invalid_lengths++;
            continue;
        }

        size_t payload_read = 0;
        result = read_exact(transport, payload, meta.message_length, running,
                            stats, &payload_read);
        if (result != EXACT_OK) {
            if (result == EXACT_EOF || result == EXACT_ERROR) {
                stats->truncated_payload++;
            }
            if (result == EXACT_ERROR) {
                status = -1;
            }
            break;
        }
        if (options->print_payload) {
            print_payload_hex(options->print_stream, payload,
                              meta.message_length);
        }

        if (write_file_all(output, raw, sizeof(raw), stats) != 0 ||
            write_file_all(output, payload, meta.message_length, stats) != 0) {
            status = -1;
            break;
        }
        if (options->flush_each_record && fflush(output) != 0) {
            stats->file_write_failures++;
            status = -1;
            break;
        }
        stats->records++;
    }

    free(payload);
    return status;
}

int sniffer_transport_write_all(SnifferTransport *transport,
                                const uint8_t *data, size_t length,
                                uint64_t max_timeouts,
                                const volatile sig_atomic_t *running,
                                uint64_t *timeouts) {
    if (transport == NULL || transport->write == NULL ||
        (length > 0 && data == NULL)) {
        return -1;
    }
    size_t total = 0;
    uint64_t timeout_count = 0;
    while (total < length && is_running(running)) {
        size_t written = 0;
        SnifferIoStatus status = transport->write(
            transport->context, data + total, length - total, &written);
        if (written > length - total) {
            return -1;
        }
        total += written;
        if (status == SNIFFER_IO_DATA) {
            if (written == 0) {
                return -1;
            }
            continue;
        }
        if (status == SNIFFER_IO_TIMEOUT) {
            timeout_count++;
            if (timeouts != NULL) {
                (*timeouts)++;
            }
            if (timeout_count > max_timeouts) {
                return -1;
            }
            continue;
        }
        if (status == SNIFFER_IO_INTERRUPTED) {
            continue;
        }
        return -1;
    }
    return total == length ? 0 : -1;
}

void sniffer_print_stats(FILE *stream, const SnifferStats *stats) {
    fprintf(stream,
            "records=%" PRIu64 " received_bytes=%" PRIu64
            " stored_bytes=%" PRIu64 " magic_errors=%" PRIu64
            " invalid_lengths=%" PRIu64 " truncated_meta=%" PRIu64
            " truncated_payload=%" PRIu64 " read_timeouts=%" PRIu64
            " transport_errors=%" PRIu64 " file_write_failures=%" PRIu64
            " resynchronizations=%" PRIu64 " discarded_bytes=%" PRIu64 "\n",
            stats->records, stats->received_bytes, stats->stored_bytes,
            stats->magic_errors, stats->invalid_lengths, stats->truncated_meta,
            stats->truncated_payload, stats->read_timeouts,
            stats->transport_errors, stats->file_write_failures,
            stats->resynchronizations, stats->discarded_bytes);
}
