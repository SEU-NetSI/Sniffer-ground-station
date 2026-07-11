#include "sniffer_capture.h"
#include "sniffer_buffered_transport.h"
#include "sniffer_hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#define CLOSE_FD close
#define STREAM_FD fileno

typedef struct {
    const uint8_t *input;
    size_t input_length;
    size_t input_offset;
    size_t max_read;
    unsigned int timeouts_before_data;
    size_t read_error_at;
    int read_error_enabled;
    uint8_t output[256];
    size_t output_length;
    size_t max_write;
    unsigned int write_timeouts_before_data;
} MockTransport;

typedef struct {
    const uint8_t *packets[4];
    size_t lengths[4];
    SnifferIoStatus statuses[4];
    size_t count;
    size_t index;
} PacketTransport;

static int failures = 0;
static int tests = 0;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        failures++;                                                             \
        return;                                                                 \
    }                                                                           \
} while (0)

#define CHECK_CAPTURE(condition) do {                                           \
    if (!(condition)) {                                                         \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        failures++;                                                             \
        return (SnifferStats){0};                                               \
    }                                                                           \
} while (0)

static void write_le16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *data, uint32_t value) {
    for (unsigned int index = 0; index < 4; index++) {
        data[index] = (uint8_t)(value >> (index * 8));
    }
}

static void write_le64(uint8_t *data, uint64_t value) {
    for (unsigned int index = 0; index < 8; index++) {
        data[index] = (uint8_t)(value >> (index * 8));
    }
}

static size_t append_frame(uint8_t *data, size_t offset, uint16_t source,
                           uint16_t sequence, const uint8_t *payload,
                           uint16_t payload_length, uint64_t rx_time) {
    write_le32(data + offset, SNIFFER_MAGIC);
    write_le16(data + offset + 4, source);
    write_le16(data + offset + 6, sequence);
    write_le16(data + offset + 8, payload_length);
    write_le64(data + offset + 10, rx_time);
    memcpy(data + offset + SNIFFER_META_SIZE, payload, payload_length);
    return offset + SNIFFER_META_SIZE + payload_length;
}

static SnifferIoStatus mock_read(void *context, uint8_t *buffer,
                                 size_t capacity, size_t *received) {
    MockTransport *mock = context;
    if (mock->timeouts_before_data > 0) {
        mock->timeouts_before_data--;
        *received = 0;
        return SNIFFER_IO_TIMEOUT;
    }
    if (mock->read_error_enabled && mock->input_offset >= mock->read_error_at) {
        *received = 0;
        return SNIFFER_IO_ERROR;
    }
    if (mock->input_offset == mock->input_length) {
        *received = 0;
        return SNIFFER_IO_EOF;
    }
    size_t available = mock->input_length - mock->input_offset;
    size_t chunk = capacity < available ? capacity : available;
    if (mock->max_read > 0 && chunk > mock->max_read) {
        chunk = mock->max_read;
    }
    memcpy(buffer, mock->input + mock->input_offset, chunk);
    mock->input_offset += chunk;
    *received = chunk;
    return SNIFFER_IO_DATA;
}

static SnifferIoStatus mock_write(void *context, const uint8_t *buffer,
                                  size_t length, size_t *written) {
    MockTransport *mock = context;
    if (mock->write_timeouts_before_data > 0) {
        mock->write_timeouts_before_data--;
        *written = 0;
        return SNIFFER_IO_TIMEOUT;
    }
    size_t chunk = length;
    if (mock->max_write > 0 && chunk > mock->max_write) {
        chunk = mock->max_write;
    }
    if (mock->output_length + chunk > sizeof(mock->output)) {
        *written = 0;
        return SNIFFER_IO_ERROR;
    }
    memcpy(mock->output + mock->output_length, buffer, chunk);
    mock->output_length += chunk;
    *written = chunk;
    return SNIFFER_IO_DATA;
}

static SnifferIoStatus packet_read(void *context, uint8_t *buffer,
                                   size_t capacity, size_t *received) {
    PacketTransport *packets = context;
    if (packets->index == packets->count) {
        *received = 0;
        return SNIFFER_IO_EOF;
    }
    size_t length = packets->lengths[packets->index];
    if (capacity < length) {
        *received = 0;
        return SNIFFER_IO_ERROR;
    }
    memcpy(buffer, packets->packets[packets->index], length);
    SnifferIoStatus status = packets->statuses[packets->index];
    packets->index++;
    *received = length;
    return status == SNIFFER_IO_EOF ? SNIFFER_IO_DATA : status;
}

static SnifferStats run_capture(MockTransport *mock, uint8_t *stored,
                                size_t stored_capacity, size_t *stored_length,
                                size_t max_payload) {
    FILE *file = tmpfile();
    CHECK_CAPTURE(file != NULL);
    SnifferTransport transport = {
        .context = mock,
        .read = mock_read,
        .write = mock_write,
    };
    SnifferCaptureOptions options = {
        .max_payload = max_payload,
        .max_records = 0,
        .flush_each_record = 0,
    };
    SnifferStats stats = {0};
    CHECK_CAPTURE(sniffer_capture(file, &transport, &options, NULL, &stats) == 0);
    CHECK_CAPTURE(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK_CAPTURE(length >= 0 && (size_t)length <= stored_capacity);
    CHECK_CAPTURE(fseek(file, 0, SEEK_SET) == 0);
    *stored_length = fread(stored, 1, (size_t)length, file);
    CHECK_CAPTURE(*stored_length == (size_t)length);
    CHECK_CAPTURE(fclose(file) == 0);
    return stats;
}

static int capture_mock_to_files(MockTransport *mock, FILE *output,
                                 FILE *print_stream, int print_records,
                                 int print_payload, int flush_each_record,
                                 SnifferStats *stats) {
    SnifferTransport transport = {
        .context = mock,
        .read = mock_read,
        .write = mock_write,
    };
    SnifferCaptureOptions options = {
        .max_payload = 64,
        .max_records = 0,
        .flush_each_record = flush_each_record,
        .print_records = print_records,
        .print_payload = print_payload,
        .print_stream = print_stream,
    };
    return sniffer_capture(output, &transport, &options, NULL, stats);
}

static size_t read_stream(FILE *stream, void *buffer, size_t capacity) {
    if (fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        return SIZE_MAX;
    }
    long length = ftell(stream);
    if (length < 0 || (size_t)length > capacity ||
        fseek(stream, 0, SEEK_SET) != 0) {
        return SIZE_MAX;
    }
    size_t read_length = fread(buffer, 1, (size_t)length, stream);
    return read_length == (size_t)length ? read_length : SIZE_MAX;
}

static void test_complete_record(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {1, 2, 3, 4};
    size_t length = append_frame(data, 0, 5, 9, payload, sizeof(payload), 123);
    MockTransport mock = {.input = data, .input_length = length};
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 1 && stored_length == length);
    CHECK(memcmp(data, stored, length) == 0);
    SnifferMeta meta;
    sniffer_decode_meta(data, &meta);
    CHECK(meta.magic == SNIFFER_MAGIC && meta.sender_address == 5);
    CHECK(meta.sequence == 9 && meta.message_length == sizeof(payload));
    CHECK(meta.rx_time == 123);
}

static void test_split_meta(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {0xAA};
    size_t length = append_frame(data, 0, 1, 2, payload, sizeof(payload), 3);
    MockTransport mock = {.input = data, .input_length = length, .max_read = 1};
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 1 && stats.received_bytes == length);
}

static void test_usb_packet_larger_than_parser_request(void) {
    tests++;
    uint8_t frame[64];
    const uint8_t payload[] = {9, 8, 7, 6};
    size_t length = append_frame(frame, 0, 3, 4, payload, sizeof(payload), 5);
    PacketTransport packets = {
        .packets = {frame, frame + SNIFFER_META_SIZE},
        .lengths = {SNIFFER_META_SIZE, sizeof(payload)},
        .statuses = {SNIFFER_IO_DATA, SNIFFER_IO_DATA},
        .count = 2,
    };
    SnifferTransport upstream = {
        .context = &packets, .read = packet_read, .write = NULL,
    };
    uint8_t staging[64];
    SnifferBufferedTransport buffered;
    CHECK(sniffer_buffered_transport_init(&buffered, upstream, staging,
                                          sizeof(staging)) == 0);
    SnifferTransport transport = sniffer_buffered_transport_adapter(&buffered);
    FILE *file = tmpfile();
    CHECK(file != NULL);
    SnifferCaptureOptions options = {
        .max_payload = 64, .max_records = 0, .flush_each_record = 0,
    };
    SnifferStats stats = {0};
    CHECK(sniffer_capture(file, &transport, &options, NULL, &stats) == 0);
    CHECK(stats.records == 1 && stats.received_bytes == length);
    CHECK(packets.index == packets.count);
    CHECK(fclose(file) == 0);
}

static void test_buffered_partial_timeout_delivery(void) {
    tests++;
    uint8_t frame[64];
    const uint8_t payload[] = {0x42};
    size_t length = append_frame(frame, 0, 4, 5, payload, sizeof(payload), 6);
    PacketTransport packets = {
        .packets = {frame, frame + SNIFFER_META_SIZE},
        .lengths = {SNIFFER_META_SIZE, sizeof(payload)},
        .statuses = {SNIFFER_IO_TIMEOUT, SNIFFER_IO_DATA},
        .count = 2,
    };
    SnifferTransport upstream = {
        .context = &packets, .read = packet_read, .write = NULL,
    };
    uint8_t staging[64];
    SnifferBufferedTransport buffered;
    CHECK(sniffer_buffered_transport_init(&buffered, upstream, staging,
                                          sizeof(staging)) == 0);
    SnifferTransport transport = sniffer_buffered_transport_adapter(&buffered);
    FILE *file = tmpfile();
    CHECK(file != NULL);
    SnifferCaptureOptions options = {
        .max_payload = 64, .max_records = 0, .flush_each_record = 0,
    };
    SnifferStats stats = {0};
    CHECK(sniffer_capture(file, &transport, &options, NULL, &stats) == 0);
    CHECK(stats.records == 1 && stats.received_bytes == length);
    CHECK(stats.read_timeouts == 1 && packets.index == packets.count);
    CHECK(fclose(file) == 0);
}

static void test_split_payload(void) {
    tests++;
    uint8_t data[96];
    const uint8_t payload[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    size_t length = append_frame(data, 0, 2, 3, payload, sizeof(payload), 4);
    MockTransport mock = {.input = data, .input_length = length, .max_read = 3};
    uint8_t stored[96];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 1 && stored_length == length);
}

static void test_multiple_records(void) {
    tests++;
    uint8_t data[128];
    const uint8_t first[] = {1, 2};
    const uint8_t second[] = {3, 4, 5};
    size_t length = append_frame(data, 0, 1, 10, first, sizeof(first), 100);
    length = append_frame(data, length, 2, 11, second, sizeof(second), 200);
    MockTransport mock = {.input = data, .input_length = length, .max_read = 2};
    uint8_t stored[128];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 2 && stored_length == length);
}

static void test_magic_resynchronization(void) {
    tests++;
    uint8_t data[64] = {0x10, 0x20, 0x30};
    const uint8_t payload[] = {7};
    size_t length = append_frame(data, 3, 1, 1, payload, sizeof(payload), 1);
    MockTransport mock = {.input = data, .input_length = length, .max_read = 1};
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 1 && stats.magic_errors == 1);
    CHECK(stats.resynchronizations == 1 && stats.discarded_bytes == 3);
}

static void test_unrecoverable_magic_tail(void) {
    tests++;
    const uint8_t data[] = {1, 2, 3, 4, 5};
    MockTransport mock = {.input = data, .input_length = sizeof(data), .max_read = 1};
    uint8_t stored[16];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.records == 0 && stats.magic_errors == 1);
    CHECK(stats.resynchronizations == 0 && stats.discarded_bytes == sizeof(data));
}

static void test_invalid_length(void) {
    tests++;
    uint8_t data[96];
    const uint8_t payload[] = {8};
    write_le32(data, SNIFFER_MAGIC);
    write_le16(data + 4, 1);
    write_le16(data + 6, 1);
    write_le16(data + 8, 100);
    write_le64(data + 10, 1);
    size_t length = append_frame(data, SNIFFER_META_SIZE, 2, 2,
                                 payload, sizeof(payload), 2);
    MockTransport mock = {.input = data, .input_length = length};
    uint8_t stored[96];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.invalid_lengths == 1 && stats.records == 1);
}

static void test_truncated_payload(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {1, 2, 3, 4};
    size_t length = append_frame(data, 0, 1, 1, payload, sizeof(payload), 1);
    MockTransport mock = {.input = data, .input_length = length - 1};
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.truncated_payload == 1 && stats.records == 0 && stored_length == 0);
}

static void test_truncated_meta(void) {
    tests++;
    const uint8_t data[] = {0xBB, 0xBB, 0x00, 0x00, 1, 2, 3};
    MockTransport mock = {.input = data, .input_length = sizeof(data)};
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.truncated_meta == 1 && stats.records == 0 && stored_length == 0);
}

static void test_disconnect_during_payload(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {1, 2, 3, 4};
    size_t length = append_frame(data, 0, 1, 1, payload, sizeof(payload), 1);
    MockTransport mock = {
        .input = data,
        .input_length = length,
        .max_read = 1,
        .read_error_at = length - 1,
        .read_error_enabled = 1,
    };
    SnifferTransport transport = {
        .context = &mock, .read = mock_read, .write = mock_write,
    };
    FILE *file = tmpfile();
    CHECK(file != NULL);
    SnifferCaptureOptions options = {
        .max_payload = 64, .max_records = 0, .flush_each_record = 0,
    };
    SnifferStats stats = {0};
    CHECK(sniffer_capture(file, &transport, &options, NULL, &stats) != 0);
    CHECK(stats.truncated_payload == 1 && stats.transport_errors == 1);
    CHECK(stats.records == 0 && stats.stored_bytes == 0);
    CHECK(fclose(file) == 0);
}

static void test_timeout_then_data(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {1};
    size_t length = append_frame(data, 0, 1, 1, payload, sizeof(payload), 1);
    MockTransport mock = {
        .input = data, .input_length = length, .timeouts_before_data = 2,
    };
    uint8_t stored[64];
    size_t stored_length = 0;
    SnifferStats stats = run_capture(&mock, stored, sizeof(stored), &stored_length, 64);
    CHECK(stats.read_timeouts == 2 && stats.records == 1);
}

static void test_partial_write(void) {
    tests++;
    const uint8_t expected[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    MockTransport mock = {.max_write = 2};
    SnifferTransport transport = {
        .context = &mock, .read = mock_read, .write = mock_write,
    };
    uint64_t timeouts = 0;
    CHECK(sniffer_transport_write_all(&transport, expected, sizeof(expected),
                                      3, NULL, &timeouts) == 0);
    CHECK(mock.output_length == sizeof(expected));
    CHECK(memcmp(mock.output, expected, sizeof(expected)) == 0);
}

static void test_write_timeout_is_bounded(void) {
    tests++;
    const uint8_t data[] = {1};
    MockTransport mock = {.write_timeouts_before_data = 3};
    SnifferTransport transport = {
        .context = &mock, .read = mock_read, .write = mock_write,
    };
    uint64_t timeouts = 0;
    CHECK(sniffer_transport_write_all(&transport, data, sizeof(data), 2,
                                      NULL, &timeouts) != 0);
    CHECK(timeouts == 3 && mock.output_length == 0);
}

static void test_send_hex_round_trip(void) {
    tests++;
    uint8_t encoded[16];
    size_t length = 0;
    char formatted[33];
    CHECK(sniffer_parse_hex("0xBB:01 02-a5", encoded, sizeof(encoded), &length) == 0);
    CHECK(length == 4 && encoded[0] == 0xBB && encoded[3] == 0xA5);
    CHECK(sniffer_format_hex(encoded, length, formatted, sizeof(formatted)) == 0);
    CHECK(strcmp(formatted, "BB0102A5") == 0);
    CHECK(sniffer_parse_hex("ABC", encoded, sizeof(encoded), &length) != 0);
}

static void test_record_printing_and_binary_identity(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB};
    const uint64_t rx_time = UINT64_C(0x123456123456789A);
    size_t length = append_frame(data, 0, 0, 244, payload,
                                 sizeof(payload), rx_time);
    MockTransport quiet_mock = {.input = data, .input_length = length};
    MockTransport print_mock = {.input = data, .input_length = length};
    FILE *quiet_output = tmpfile();
    FILE *print_output = tmpfile();
    FILE *quiet_stream = tmpfile();
    FILE *print_stream = tmpfile();
    CHECK(quiet_output != NULL && print_output != NULL);
    CHECK(quiet_stream != NULL && print_stream != NULL);

    SnifferStats quiet_stats = {0};
    SnifferStats print_stats = {0};
    CHECK(capture_mock_to_files(&quiet_mock, quiet_output, quiet_stream,
                                0, 0, 0, &quiet_stats) == 0);
    CHECK(capture_mock_to_files(&print_mock, print_output, print_stream,
                                1, 1, 0, &print_stats) == 0);

    uint8_t quiet_bytes[64];
    uint8_t print_bytes[64];
    char quiet_text[32];
    char print_text[512];
    size_t quiet_length = read_stream(quiet_output, quiet_bytes,
                                      sizeof(quiet_bytes));
    size_t print_length = read_stream(print_output, print_bytes,
                                      sizeof(print_bytes));
    size_t quiet_text_length = read_stream(quiet_stream, quiet_text,
                                           sizeof(quiet_text));
    size_t print_text_length = read_stream(print_stream, print_text,
                                           sizeof(print_text) - 1);
    CHECK(quiet_length == length && print_length == length);
    CHECK(memcmp(quiet_bytes, print_bytes, length) == 0);
    CHECK(memcmp(data, print_bytes, length) == 0);
    CHECK(quiet_text_length == 0 && print_text_length != SIZE_MAX);
    print_text[print_text_length] = '\0';
    CHECK(strstr(print_text, "Packet 1\n") != NULL);
    CHECK(strstr(print_text, "Sniffer_Meta_t:\n") != NULL);
    CHECK(strstr(print_text, "  Magic: 0x0000BBBB\n") != NULL);
    CHECK(strstr(print_text, "  Sender Address: 0\n") != NULL);
    CHECK(strstr(print_text, "  Sequence Number: 244\n") != NULL);
    CHECK(strstr(print_text, "  Message Length: 6\n") != NULL);
    CHECK(strstr(print_text, "  RX Time: 0x123456123456789A\n") != NULL);
    CHECK(strstr(print_text, "Payload: 01020304AABB\n") != NULL);
    char *first_record = strstr(print_text, "Packet ");
    CHECK(first_record != NULL && strstr(first_record + 1, "Packet ") == NULL);
    CHECK(quiet_stats.records == 1 && print_stats.records == 1);

    CHECK(fclose(quiet_output) == 0);
    CHECK(fclose(print_output) == 0);
    CHECK(fclose(quiet_stream) == 0);
    CHECK(fclose(print_stream) == 0);
}

static void test_original_print_timing_for_truncated_records(void) {
    tests++;
    const uint8_t short_meta[] = {0xBB, 0xBB, 0x00, 0x00, 1, 2, 3};
    MockTransport meta_mock = {
        .input = short_meta, .input_length = sizeof(short_meta),
    };
    FILE *meta_output = tmpfile();
    FILE *meta_stream = tmpfile();
    CHECK(meta_output != NULL && meta_stream != NULL);
    SnifferStats meta_stats = {0};
    CHECK(capture_mock_to_files(&meta_mock, meta_output, meta_stream,
                                1, 0, 0, &meta_stats) == 0);
    char text[512];
    CHECK(read_stream(meta_stream, text, sizeof(text)) == 0);
    CHECK(meta_stats.truncated_meta == 1 && meta_stats.records == 0);
    CHECK(fclose(meta_output) == 0 && fclose(meta_stream) == 0);

    uint8_t frame[64];
    const uint8_t payload[] = {1, 2, 3, 4};
    size_t length = append_frame(frame, 0, 1, 2, payload,
                                 sizeof(payload), 3);
    MockTransport payload_mock = {
        .input = frame, .input_length = length - 1,
    };
    FILE *payload_output = tmpfile();
    FILE *payload_stream = tmpfile();
    CHECK(payload_output != NULL && payload_stream != NULL);
    SnifferStats payload_stats = {0};
    CHECK(capture_mock_to_files(&payload_mock, payload_output, payload_stream,
                                1, 0, 0, &payload_stats) == 0);
    size_t text_length = read_stream(payload_stream, text, sizeof(text) - 1);
    CHECK(text_length != SIZE_MAX && text_length > 0);
    text[text_length] = '\0';
    CHECK(strstr(text, "Packet 1\n") != NULL);
    CHECK(strstr(text, "  Sequence Number: 2\n") != NULL);
    CHECK(payload_stats.truncated_payload == 1 && payload_stats.records == 0);
    CHECK(fclose(payload_output) == 0 && fclose(payload_stream) == 0);
}

static void test_original_print_occurs_before_file_write_failure(void) {
    tests++;
    uint8_t data[64];
    const uint8_t payload[] = {1, 2, 3};
    size_t length = append_frame(data, 0, 1, 2, payload,
                                 sizeof(payload), 3);
    MockTransport mock = {.input = data, .input_length = length};
    FILE *output = tmpfile();
    FILE *print_stream = tmpfile();
    CHECK(output != NULL && print_stream != NULL);
    CHECK(CLOSE_FD(STREAM_FD(output)) == 0);
    SnifferStats stats = {0};
    CHECK(capture_mock_to_files(&mock, output, print_stream,
                                1, 0, 1, &stats) != 0);
    char text[512];
    size_t text_length = read_stream(print_stream, text, sizeof(text) - 1);
    CHECK(text_length != SIZE_MAX && text_length > 0);
    text[text_length] = '\0';
    CHECK(strstr(text, "Packet 1\n") != NULL);
    CHECK(strstr(text, "  Sequence Number: 2\n") != NULL);
    CHECK(stats.file_write_failures == 1 && stats.records == 0);
    (void)fclose(output);
    CHECK(fclose(print_stream) == 0);
}

int main(void) {
    test_complete_record();
    test_split_meta();
    test_usb_packet_larger_than_parser_request();
    test_buffered_partial_timeout_delivery();
    test_split_payload();
    test_multiple_records();
    test_magic_resynchronization();
    test_unrecoverable_magic_tail();
    test_invalid_length();
    test_truncated_payload();
    test_truncated_meta();
    test_disconnect_during_payload();
    test_timeout_then_data();
    test_partial_write();
    test_write_timeout_is_bounded();
    test_send_hex_round_trip();
    test_record_printing_and_binary_identity();
    test_original_print_timing_for_truncated_records();
    test_original_print_occurs_before_file_write_failure();
    if (failures != 0) {
        fprintf(stderr, "%d/%d test(s) failed\n", failures, tests);
        return 1;
    }
    printf("All %d sniffer storage tests passed\n", tests);
    return 0;
}
