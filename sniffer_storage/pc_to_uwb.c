#include <ctype.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../AdHocUWB/Inc/adhocuwb_sniffer_protocol.h"

#define VENDOR_ID               0x0483
#define PRODUCT_ID              0x5740
#define CF_IN_ENDPOINT          0x81
#define CF_OUT_ENDPOINT         0x01
#define USB_RX_TX_PACKET_SIZE   64
#define MAX_PAYLOAD_SIZE        SNIFFER_DOWNLINK_PAYLOAD_MAX

static uint16_t seqNumber = 0;

static uint64_t get_system_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
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

    int response = libusb_bulk_transfer(deviceHandle,
                                        CF_OUT_ENDPOINT,
                                        meta.raw,
                                        sizeof(Downlink_Meta_t),
                                        &transferred,
                                        5000);

    if (response != 0 || transferred != sizeof(Downlink_Meta_t)) {
        fprintf(stderr, "Downlink meta transfer failed: %s, transferred=%d\n",
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
            fprintf(stderr, "Downlink payload transfer failed: %s, transferred=%d\n",
                    libusb_strerror(response), transferred);
            return -1;
        }

        pointer += sizeToSend;
        remain -= sizeToSend;
    }

    return 0;
}

static int wait_downlink_ack(libusb_device_handle *deviceHandle) {
    uint8_t ack[USB_RX_TX_PACKET_SIZE];
    const int maxAttempts = 20;

    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        int transferred = 0;
        int response = libusb_bulk_transfer(deviceHandle,
                                            CF_IN_ENDPOINT,
                                            ack,
                                            sizeof(ack),
                                            &transferred,
                                            500);

        if (response != 0) {
            fprintf(stderr, "Downlink ACK read failed: %s\n", libusb_strerror(response));
            return -1;
        }

        if (transferred == SNIFFER_DOWNLINK_ACK_SIZE &&
            ack[0] == SNIFFER_DOWNLINK_ACK0 &&
            ack[1] == SNIFFER_DOWNLINK_ACK1 &&
            ack[2] == SNIFFER_DOWNLINK_ACK2 &&
            ack[3] == SNIFFER_DOWNLINK_ACK3) {
            return 0;
        }

        fprintf(stderr, "Skip non-ACK USB IN packet:");
        for (int i = 0; i < transferred; i++) {
            fprintf(stderr, " %02x", ack[i]);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "Downlink ACK not found\n");
    return -1;
}

static void print_usage(const char *programName) {
    fprintf(stderr, "Usage: %s <dest_addr> <hex_payload> [seq]\n", programName);
    fprintf(stderr, "Example: %s 0xffff \"01 02 03 04\" 1\n", programName);
}

int main(int argc, char **argv) {
    libusb_context *context = NULL;
    libusb_device_handle *deviceHandle = NULL;
    uint16_t destAddress;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t payloadLength;

    if (argc < 3 || argc > 4) {
        print_usage(argv[0]);
        return 1;
    }

    if (parse_u16(argv[1], &destAddress) != 0) {
        fprintf(stderr, "Invalid dest_addr: %s\n", argv[1]);
        return 1;
    }

    if (parse_hex_payload(argv[2], payload, &payloadLength) != 0) {
        fprintf(stderr, "Invalid hex_payload: %s\n", argv[2]);
        return 1;
    }

    if (argc == 4 && parse_u16(argv[3], &seqNumber) != 0) {
        fprintf(stderr, "Invalid seq: %s\n", argv[3]);
        return 1;
    }

    int response = libusb_init(&context);
    if (response < 0) {
        fprintf(stderr, "libusb init error: %s\n", libusb_strerror(response));
        return 1;
    }

    deviceHandle = libusb_open_device_with_vid_pid(context, VENDOR_ID, PRODUCT_ID);
    if (!deviceHandle) {
        fprintf(stderr, "cannot find USB device\n");
        libusb_exit(context);
        return 1;
    }

    libusb_set_auto_detach_kernel_driver(deviceHandle, 1);
    response = libusb_claim_interface(deviceHandle, 0);
    if (response != 0) {
        fprintf(stderr, "claim interface failed: %s\n", libusb_strerror(response));
        libusb_close(deviceHandle);
        libusb_exit(context);
        return 1;
    }

    response = send_pc_to_uwb(deviceHandle, destAddress, seqNumber, payload, payloadLength);
    if (response == 0) {
        response = wait_downlink_ack(deviceHandle);
    }

    libusb_release_interface(deviceHandle, 0);
    libusb_close(deviceHandle);
    libusb_exit(context);

    if (response != 0) {
        return 1;
    }

    printf("Sent %u bytes to UWB dest 0x%04x, seq=%u\n",
           payloadLength, destAddress, seqNumber);
    printf("Received downlink ACK from CF firmware\n");
    return 0;
}
