#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../AdHocUWB/Inc/adhocuwb_sniffer_protocol.h"

#define VENDOR_ID               0x0483
#define PRODUCT_ID              0x5740
#define USB_INTERFACE           0
#define CF_IN_ENDPOINT          0x81
#define CF_OUT_ENDPOINT         0x01
#define USB_RX_TX_PACKET_SIZE   64
#define USB_TIMEOUT_MS          500
#define ACK_TIMEOUT_MS          3000
#define FILENAME_SIZE           64
#define MAX_MESSAGE_SIZE        4096
#define STREAM_BUFFER_SIZE      8192
#define MAX_PAYLOAD_SIZE        SNIFFER_DOWNLINK_PAYLOAD_MAX

static volatile sig_atomic_t keep_running = 1;

static pthread_mutex_t ackMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ackCond = PTHREAD_COND_INITIALIZER;
static int ackReceived = 0;

static pthread_mutex_t printMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    libusb_device_handle *deviceHandle;
    FILE *logFile;
    char filename[FILENAME_SIZE];
    uint8_t stream[STREAM_BUFFER_SIZE];
    size_t streamLen;
    unsigned long packetCount;
} RxContext;

static void handle_sigint(int sigint) {
    (void)sigint;
    keep_running = 0;
}

static void safe_printf(const char *format, ...) {
    va_list args;

    pthread_mutex_lock(&printMutex);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&printMutex);
}

static void safe_fprintf(FILE *stream, const char *format, ...) {
    va_list args;

    pthread_mutex_lock(&printMutex);
    va_start(args, format);
    vfprintf(stream, format, args);
    va_end(args);
    fflush(stream);
    pthread_mutex_unlock(&printMutex);
}

static void generate_filename(char *buffer, size_t bufferSize) {
    time_t rawtime;
    struct tm *curtime;

    time(&rawtime);
    curtime = localtime(&rawtime);
    strftime(buffer, bufferSize, "raw_sensor_data_%Y%m%d_%H%M%S.bin", curtime);
}

static uint64_t get_system_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static uint64_t get_realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static int is_downlink_ack(const uint8_t *data, int length) {
    return length == SNIFFER_DOWNLINK_ACK_SIZE &&
           data[0] == SNIFFER_DOWNLINK_ACK0 &&
           data[1] == SNIFFER_DOWNLINK_ACK1 &&
           data[2] == SNIFFER_DOWNLINK_ACK2 &&
           data[3] == SNIFFER_DOWNLINK_ACK3;
}

static int find_downlink_ack(const uint8_t *data, int length) {
    for (int i = 0; i <= length - SNIFFER_DOWNLINK_ACK_SIZE; i++) {
        if (data[i] == SNIFFER_DOWNLINK_ACK0 &&
            data[i + 1] == SNIFFER_DOWNLINK_ACK1 &&
            data[i + 2] == SNIFFER_DOWNLINK_ACK2 &&
            data[i + 3] == SNIFFER_DOWNLINK_ACK3) {
            return i;
        }
    }

    return -1;
}

static void notify_ack(void) {
    pthread_mutex_lock(&ackMutex);
    ackReceived = 1;
    pthread_cond_signal(&ackCond);
    pthread_mutex_unlock(&ackMutex);
}

static void reset_ack_wait(void) {
    pthread_mutex_lock(&ackMutex);
    ackReceived = 0;
    pthread_mutex_unlock(&ackMutex);
}

static int wait_ack_ms(int timeoutMs) {
    struct timespec deadline;
    uint64_t deadlineMs = get_realtime_ms() + (uint64_t)timeoutMs;

    deadline.tv_sec = (time_t)(deadlineMs / 1000ULL);
    deadline.tv_nsec = (long)((deadlineMs % 1000ULL) * 1000000ULL);

    pthread_mutex_lock(&ackMutex);
    while (keep_running && !ackReceived) {
        int response = pthread_cond_timedwait(&ackCond, &ackMutex, &deadline);
        if (response == ETIMEDOUT) {
            pthread_mutex_unlock(&ackMutex);
            return -1;
        }
    }

    if (!keep_running) {
        pthread_mutex_unlock(&ackMutex);
        return -1;
    }

    ackReceived = 0;
    pthread_mutex_unlock(&ackMutex);
    return 0;
}

static libusb_device_handle *open_matching_device(libusb_context *context) {
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    ssize_t count = libusb_get_device_list(context, &devices);

    if (count < 0) {
        safe_fprintf(stderr, "libusb_get_device_list error: %s\n", libusb_strerror((int)count));
        return NULL;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor descriptor;
        int response = libusb_get_device_descriptor(devices[i], &descriptor);
        if (response != 0) {
            continue;
        }

        if (descriptor.idVendor == VENDOR_ID && descriptor.idProduct == PRODUCT_ID) {
            safe_printf("Found USB device %04x:%04x on bus %u address %u\n",
                        descriptor.idVendor,
                        descriptor.idProduct,
                        libusb_get_bus_number(devices[i]),
                        libusb_get_device_address(devices[i]));

            response = libusb_open(devices[i], &handle);
            if (response != 0) {
                safe_fprintf(stderr, "libusb_open error: %s\n", libusb_strerror(response));
                handle = NULL;
            }
            break;
        }
    }

    libusb_free_device_list(devices, 1);
    return handle;
}

static int parse_u16(const char *text, uint16_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);

    if (errno != 0 || end == text || *end != '\0' || parsed > UINT16_MAX) {
        return -1;
    }

    *value = (uint16_t)parsed;
    return 0;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

static int parse_hex_payload(const char *text, uint8_t *payload, uint16_t *payloadLength) {
    int highNibble = -1;
    uint16_t length = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == ':' || *p == '-' || *p == ',') {
            continue;
        }

        int value = hex_value(*p);
        if (value < 0) {
            return -1;
        }

        if (highNibble < 0) {
            highNibble = value;
        } else {
            if (length >= MAX_PAYLOAD_SIZE) {
                return -1;
            }
            payload[length++] = (uint8_t)((highNibble << 4) | value);
            highNibble = -1;
        }
    }

    if (highNibble >= 0) {
        return -1;
    }

    *payloadLength = length;
    return 0;
}

static void consume_stream(RxContext *rx, size_t count) {
    if (count >= rx->streamLen) {
        rx->streamLen = 0;
        return;
    }

    memmove(rx->stream, rx->stream + count, rx->streamLen - count);
    rx->streamLen -= count;
}

static void parse_sniffer_stream(RxContext *rx) {
    const uint8_t magicRaw[4] = {0xBB, 0xBB, 0x00, 0x00};

    while (keep_running) {
        if (rx->streamLen < sizeof(magicRaw)) {
            return;
        }

        if (memcmp(rx->stream, magicRaw, sizeof(magicRaw)) != 0) {
            consume_stream(rx, 1);
            continue;
        }

        if (rx->streamLen < sizeof(Sniffer_Meta_t)) {
            return;
        }

        Sniffer_Meta_t meta;
        memcpy(meta.raw, rx->stream, sizeof(Sniffer_Meta_t));

        if (meta.msgLength > MAX_MESSAGE_SIZE) {
            safe_fprintf(stderr,
                         "Discarding invalid meta with unreasonable message length: %" PRIu16 "\n",
                         meta.msgLength);
            consume_stream(rx, 1);
            continue;
        }

        size_t frameLength = sizeof(Sniffer_Meta_t) + meta.msgLength;
        if (rx->streamLen < frameLength) {
            return;
        }

        rx->packetCount++;

        if (fwrite(rx->stream, 1, frameLength, rx->logFile) != frameLength) {
            safe_fprintf(stderr, "Write sniffer frame failed\n");
            keep_running = 0;
            return;
        }
        fflush(rx->logFile);

        consume_stream(rx, frameLength);
    }
}

static void append_sniffer_bytes(RxContext *rx, const uint8_t *data, int length) {
    if (length <= 0) {
        return;
    }

    if (rx->streamLen + (size_t)length > STREAM_BUFFER_SIZE) {
        safe_fprintf(stderr, "Sniffer stream buffer overflow, resynchronizing\n");
        rx->streamLen = 0;
    }

    if ((size_t)length > STREAM_BUFFER_SIZE) {
        data += length - STREAM_BUFFER_SIZE;
        length = STREAM_BUFFER_SIZE;
    }

    memcpy(rx->stream + rx->streamLen, data, (size_t)length);
    rx->streamLen += (size_t)length;
    parse_sniffer_stream(rx);
}

static void dispatch_usb_in_packet(RxContext *rx, const uint8_t *data, int length) {
    int offset = 0;

    while (offset < length) {
        int ackOffset = find_downlink_ack(data + offset, length - offset);
        if (ackOffset < 0) {
            append_sniffer_bytes(rx, data + offset, length - offset);
            return;
        }

        if (ackOffset > 0) {
            append_sniffer_bytes(rx, data + offset, ackOffset);
        }

        notify_ack();
        offset += ackOffset + SNIFFER_DOWNLINK_ACK_SIZE;
    }
}

static void *rx_thread_main(void *parameters) {
    RxContext *rx = (RxContext *)parameters;
    uint8_t packet[USB_RX_TX_PACKET_SIZE];

    while (keep_running) {
        int transferred = 0;
        int response = libusb_bulk_transfer(rx->deviceHandle,
                                            CF_IN_ENDPOINT,
                                            packet,
                                            sizeof(packet),
                                            &transferred,
                                            USB_TIMEOUT_MS);

        if (response == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }

        if (response != 0) {
            if (keep_running) {
                safe_fprintf(stderr, "USB IN transfer failed: %s\n", libusb_strerror(response));
            }
            keep_running = 0;
            break;
        }

        if (transferred <= 0) {
            continue;
        }

        dispatch_usb_in_packet(rx, packet, transferred);
    }

    return NULL;
}

static int send_pc_to_uwb(libusb_device_handle *deviceHandle,
                          uint16_t destAddress,
                          uint16_t sequence,
                          const uint8_t *payload,
                          uint16_t payloadLength) {
    Downlink_Meta_t meta;
    int transferred = 0;

    meta.magic = SNIFFER_DOWNLINK_MAGIC;
    meta.destAddress = destAddress;
    meta.seqNumber = sequence;
    meta.msgLength = payloadLength;
    meta.txTime = get_system_time_ms();

    reset_ack_wait();

    int response = libusb_bulk_transfer(deviceHandle,
                                        CF_OUT_ENDPOINT,
                                        meta.raw,
                                        sizeof(Downlink_Meta_t),
                                        &transferred,
                                        5000);

    if (response != 0 || transferred != (int)sizeof(Downlink_Meta_t)) {
        safe_fprintf(stderr, "Downlink meta transfer failed: %s, transferred=%d\n",
                     libusb_strerror(response), transferred);
        return -1;
    }

    const uint8_t *pointer = payload;
    int remain = payloadLength;

    while (remain > 0) {
        int sizeToSend = remain > USB_RX_TX_PACKET_SIZE ? USB_RX_TX_PACKET_SIZE : remain;

        response = libusb_bulk_transfer(deviceHandle,
                                        CF_OUT_ENDPOINT,
                                        (unsigned char *)pointer,
                                        sizeToSend,
                                        &transferred,
                                        5000);

        if (response != 0 || transferred != sizeToSend) {
            safe_fprintf(stderr, "Downlink payload transfer failed: %s, transferred=%d\n",
                         libusb_strerror(response), transferred);
            return -1;
        }

        pointer += sizeToSend;
        remain -= sizeToSend;
    }

    if (wait_ack_ms(ACK_TIMEOUT_MS) != 0) {
        safe_fprintf(stderr, "Downlink ACK timed out\n");
        return -1;
    }

    safe_printf("Sent %u bytes to UWB dest 0x%04x, seq=%u\n",
                payloadLength, destAddress, sequence);
    safe_printf("Received downlink ACK from CF firmware\n");
    return 0;
}

static int send_from_tokens(libusb_device_handle *deviceHandle,
                            const char *destText,
                            const char *payloadText,
                            const char *seqText,
                            uint16_t *nextSeq) {
    uint16_t destAddress;
    uint16_t sequence;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t payloadLength;

    if (parse_u16(destText, &destAddress) != 0) {
        safe_fprintf(stderr, "Invalid dest_addr: %s\n", destText);
        return -1;
    }

    if (parse_hex_payload(payloadText, payload, &payloadLength) != 0) {
        safe_fprintf(stderr, "Invalid hex_payload: %s\n", payloadText);
        return -1;
    }

    if (seqText != NULL) {
        if (parse_u16(seqText, &sequence) != 0) {
            safe_fprintf(stderr, "Invalid seq: %s\n", seqText);
            return -1;
        }
        *nextSeq = (uint16_t)(sequence + 1);
    } else {
        sequence = (*nextSeq)++;
    }

    return send_pc_to_uwb(deviceHandle, destAddress, sequence, payload, payloadLength);
}

static void print_usage(const char *programName) {
    safe_fprintf(stderr, "Usage: %s [dest_addr hex_payload [seq]]\n", programName);
    safe_fprintf(stderr, "Example: %s 0xffff \"01 02 03 04\" 1\n", programName);
    safe_fprintf(stderr, "Interactive command: send 0xffff 01:02:03:04 [seq]\n");
}

static void interactive_loop(libusb_device_handle *deviceHandle, uint16_t *nextSeq) {
    char line[512];

    safe_printf("Commands: send <dest_addr> <hex_payload> [seq], quit\n");
    while (keep_running) {
        safe_printf("> ");

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        char *newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        char *command = strtok(line, " \t");
        if (command == NULL) {
            continue;
        }

        if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) {
            keep_running = 0;
            break;
        }

        if (strcmp(command, "send") != 0) {
            safe_fprintf(stderr, "Unknown command: %s\n", command);
            continue;
        }

        char *destText = strtok(NULL, " \t");
        char *payloadText = strtok(NULL, " \t");
        char *seqText = strtok(NULL, " \t");

        if (destText == NULL || payloadText == NULL) {
            safe_fprintf(stderr, "Usage: send <dest_addr> <hex_payload> [seq]\n");
            continue;
        }

        send_from_tokens(deviceHandle, destText, payloadText, seqText, nextSeq);
    }
}

int main(int argc, char **argv) {
    libusb_context *context = NULL;
    libusb_device_handle *deviceHandle = NULL;
    FILE *logFile = NULL;
    pthread_t rxThread;
    int rxThreadStarted = 0;
    int interfaceClaimed = 0;
    int exitCode = 1;
    uint16_t nextSeq = 1;
    RxContext rx;

    if (argc != 1 && argc != 3 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    memset(&rx, 0, sizeof(rx));
    signal(SIGINT, handle_sigint);

    int response = libusb_init(&context);
    if (response < 0) {
        safe_fprintf(stderr, "libusb init error: %s\n", libusb_strerror(response));
        return 1;
    }

    deviceHandle = open_matching_device(context);
    if (!deviceHandle) {
        safe_fprintf(stderr, "cannot open USB device\n");
        goto cleanup;
    }

    libusb_set_auto_detach_kernel_driver(deviceHandle, 1);
    response = libusb_claim_interface(deviceHandle, USB_INTERFACE);
    if (response != 0) {
        safe_fprintf(stderr, "libusb claim interface %d error: %s\n",
                     USB_INTERFACE, libusb_strerror(response));
        goto cleanup;
    }
    interfaceClaimed = 1;

    libusb_clear_halt(deviceHandle, CF_IN_ENDPOINT);
    libusb_clear_halt(deviceHandle, CF_OUT_ENDPOINT);

    generate_filename(rx.filename, sizeof(rx.filename));
    logFile = fopen(rx.filename, "wb");
    if (!logFile) {
        perror("open log file");
        goto cleanup;
    }

    rx.deviceHandle = deviceHandle;
    rx.logFile = logFile;

    safe_printf("Saving raw sniffer data to %s\n", rx.filename);

    if (pthread_create(&rxThread, NULL, rx_thread_main, &rx) != 0) {
        safe_fprintf(stderr, "Failed to create RX thread\n");
        goto cleanup;
    }
    rxThreadStarted = 1;

    if (argc == 3 || argc == 4) {
        const char *seqText = argc == 4 ? argv[3] : NULL;
        send_from_tokens(deviceHandle, argv[1], argv[2], seqText, &nextSeq);
    }

    interactive_loop(deviceHandle, &nextSeq);
    exitCode = 0;

cleanup:
    keep_running = 0;

    if (rxThreadStarted) {
        pthread_join(rxThread, NULL);
    }

    if (logFile) {
        fclose(logFile);
    }

    if (interfaceClaimed) {
        libusb_release_interface(deviceHandle, USB_INTERFACE);
    }

    if (deviceHandle) {
        libusb_close(deviceHandle);
    }

    if (context) {
        libusb_exit(context);
    }

    if (rx.filename[0] != '\0') {
        safe_printf("Logging finished. Data saved to %s\n", rx.filename);
    }

    return exitCode;
}
