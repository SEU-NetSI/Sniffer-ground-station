#include <libusb-1.0/libusb.h>

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FILENAME_SIZE     64
#define MAX_TRANSFER_SIZE 256
#define META_SIZE         ((int)sizeof(Sniffer_Meta_t))
#define MAGIC_MATCH       0xBBBBu
#define VENDOR_ID         0x0483
#define PRODUCT_ID        0x5740
#define USB_INTERFACE     0
#define USB_ENDPOINT_IN   0x81
#define USB_TIMEOUT_MS    5000
#define IDLE_NOTICE_EVERY 6
#define MAX_MESSAGE_SIZE  4096

static char filename[FILENAME_SIZE];
static volatile sig_atomic_t keep_running = 1;

#pragma pack(push, 1)
typedef union {
    uint8_t raw[18];
    struct {
        uint32_t magic;
        uint16_t senderAddress;
        uint16_t seqNumber;
        uint16_t msgLength;
        uint64_t rxTime;
    };
} Sniffer_Meta_t;
#pragma pack(pop)

static void handle_sigint(int sigint) {
    (void)sigint;
    keep_running = 0;
}

static void generate_filename(char *buffer, size_t buffer_size) {
    time_t rawtime;
    struct tm *curtime;

    time(&rawtime);
    curtime = localtime(&rawtime);
    strftime(buffer, buffer_size, "raw_sensor_data_%Y%m%d_%H%M%S.bin", curtime);
}

static void print_meta_info(const Sniffer_Meta_t *meta) {
    printf("Sniffer_Meta_t:\n");
    printf("  Magic: 0x%08" PRIX32 "\n", meta->magic);
    printf("  Sender Address: %" PRIu16 "\n", meta->senderAddress);
    printf("  Sequence Number: %" PRIu16 "\n", meta->seqNumber);
    printf("  Message Length: %" PRIu16 "\n", meta->msgLength);
    printf("  RX Time: 0x%016" PRIX64 "\n", meta->rxTime);
}

static libusb_device_handle *open_matching_device(libusb_context *context) {
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    ssize_t count = libusb_get_device_list(context, &devices);

    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list error: %s\n", libusb_strerror((int)count));
        return NULL;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor descriptor;
        int response = libusb_get_device_descriptor(devices[i], &descriptor);
        if (response != 0) {
            continue;
        }

        if (descriptor.idVendor == VENDOR_ID && descriptor.idProduct == PRODUCT_ID) {
            printf("Found USB device %04x:%04x on bus %u address %u\n",
                   descriptor.idVendor,
                   descriptor.idProduct,
                   libusb_get_bus_number(devices[i]),
                   libusb_get_device_address(devices[i]));

            response = libusb_open(devices[i], &handle);
            if (response != 0) {
                fprintf(stderr, "libusb_open error: %s\n", libusb_strerror(response));
                fprintf(stderr, "On Windows, install WinUSB for this Crazyflie interface with Zadig.\n");
                handle = NULL;
            }
            break;
        }
    }

    libusb_free_device_list(devices, 1);
    return handle;
}

static int read_exact_usb(libusb_device_handle *device_handle, uint8_t *buffer, int length, const char *label) {
    int total = 0;
    int idle_timeouts = 0;

    while (keep_running && total < length) {
        int transferred = 0;
        int response = libusb_bulk_transfer(
            device_handle,
            USB_ENDPOINT_IN,
            buffer + total,
            length - total,
            &transferred,
            USB_TIMEOUT_MS
        );

        if (transferred > 0) {
            total += transferred;
            idle_timeouts = 0;
        }

        if (response != 0) {
            if (!keep_running) {
                break;
            }

            if (response == LIBUSB_ERROR_TIMEOUT) {
                idle_timeouts++;
                if (idle_timeouts % IDLE_NOTICE_EVERY == 0) {
                    fprintf(stderr, "Waiting for USB data while reading %s...\n", label);
                }
                continue;
            }

            fprintf(stderr, "Bulk transfer failed while reading %s: %s\n",
                    label, libusb_strerror(response));
            return -1;
        }

        if (transferred == 0) {
            idle_timeouts++;
            if (idle_timeouts % IDLE_NOTICE_EVERY == 0) {
                fprintf(stderr, "Waiting for USB data while reading %s...\n", label);
            }
            continue;
        }
    }

    return total;
}

static int write_binary(FILE *file, const uint8_t *buffer, size_t length, const char *label) {
    if (fwrite(buffer, 1, length, file) != length) {
        fprintf(stderr, "Write %s failed\n", label);
        return -1;
    }

    return 0;
}

static int read_next_meta(libusb_device_handle *device_handle, Sniffer_Meta_t *meta) {
    const uint8_t magic_raw[4] = {0xBB, 0xBB, 0x00, 0x00};
    uint8_t window[4] = {0};
    unsigned long discarded = 0;
    int received = 0;

    received = read_exact_usb(device_handle, window, sizeof(window), "Sniffer_Meta_t magic");
    if (received < 0) {
        return -1;
    }
    if (received != (int)sizeof(window)) {
        return received;
    }

    while (keep_running) {
        if (window[0] == magic_raw[0] &&
            window[1] == magic_raw[1] &&
            window[2] == magic_raw[2] &&
            window[3] == magic_raw[3]) {
            memcpy(meta->raw, window, sizeof(window));
            received = read_exact_usb(
                device_handle,
                meta->raw + sizeof(window),
                META_SIZE - (int)sizeof(window),
                "Sniffer_Meta_t"
            );
            if (received < 0) {
                return -1;
            }
            if (received != META_SIZE - (int)sizeof(window)) {
                return (int)sizeof(window) + received;
            }

            if (meta->msgLength <= MAX_MESSAGE_SIZE) {
                if (discarded > 0) {
                    fprintf(stderr, "Resynchronized after discarding %lu byte(s)\n", discarded);
                }
                return META_SIZE;
            }

            fprintf(stderr,
                    "Discarding invalid meta with unreasonable message length: %" PRIu16 "\n",
                    meta->msgLength);
        }

        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];

        received = read_exact_usb(device_handle, &window[3], 1, "Sniffer_Meta_t magic");
        if (received < 0) {
            return -1;
        }
        if (received != 1) {
            return received;
        }
        discarded++;
    }

    return 0;
}

static int store_sniffer_binary_data(FILE *file, libusb_device_handle *device_handle) {
    uint8_t payload[MAX_TRANSFER_SIZE];
    unsigned long packet_count = 0;

    while (keep_running) {
        Sniffer_Meta_t meta;
        int received = read_next_meta(device_handle, &meta);
        if (received < 0) {
            return -1;
        }
        if (received != META_SIZE) {
            if (!keep_running) {
                break;
            }
            fprintf(stderr, "Received incomplete Sniffer_Meta_t: %d/%d bytes\n", received, META_SIZE);
            return -1;
        }

        packet_count++;
        printf("Packet %lu\n", packet_count);
        print_meta_info(&meta);

        if (write_binary(file, meta.raw, sizeof(meta.raw), "Sniffer_Meta_t") != 0) {
            return -1;
        }

        if (meta.magic != MAGIC_MATCH) {
            fprintf(stderr, "Magic number mismatch: 0x%08" PRIX32 "\n", meta.magic);
            fflush(file);
            continue;
        }

        uint32_t remaining = meta.msgLength;
        while (keep_running && remaining > 0) {
            int chunk_size = remaining > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : (int)remaining;
            received = read_exact_usb(device_handle, payload, chunk_size, "payload");
            if (received < 0) {
                return -1;
            }

            if (received > 0 && write_binary(file, payload, (size_t)received, "payload") != 0) {
                return -1;
            }

            if (received != chunk_size) {
                if (!keep_running) {
                    break;
                }
                fprintf(stderr, "Received incomplete payload: %d/%d bytes\n", received, chunk_size);
                return -1;
            }

            remaining -= (uint32_t)received;
        }

        fflush(file);
    }

    return 0;
}

int main(void) {
    libusb_device_handle *device_handle = NULL;
    libusb_context *context = NULL;
    FILE *log_file = NULL;
    int interface_claimed = 0;
    int exit_code = 1;

    signal(SIGINT, handle_sigint);

    int response = libusb_init(&context);
    if (response < 0) {
        fprintf(stderr, "libusb init error: %s\n", libusb_strerror(response));
        return exit_code;
    }

    device_handle = open_matching_device(context);
    if (!device_handle) {
        fprintf(stderr, "cannot open USB device\n");
        goto cleanup;
    }

    response = libusb_claim_interface(device_handle, USB_INTERFACE);
    if (response != 0) {
        fprintf(stderr, "libusb claim interface %d error: %s\n",
                USB_INTERFACE, libusb_strerror(response));
        fprintf(stderr, "If this is LIBUSB_ERROR_NOT_SUPPORTED or ACCESS, install WinUSB with Zadig.\n");
        goto cleanup;
    }
    interface_claimed = 1;

    response = libusb_clear_halt(device_handle, USB_ENDPOINT_IN);
    if (response != 0) {
        fprintf(stderr, "libusb clear halt warning on endpoint 0x%02x: %s\n",
                USB_ENDPOINT_IN, libusb_strerror(response));
    }

    generate_filename(filename, sizeof(filename));
    log_file = fopen(filename, "wb");
    if (!log_file) {
        perror("open log file");
        goto cleanup;
    }

    printf("Saving raw sniffer data to %s\n", filename);
    if (store_sniffer_binary_data(log_file, device_handle) != 0) {
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    if (log_file) {
        fclose(log_file);
    }
    if (interface_claimed) {
        libusb_release_interface(device_handle, USB_INTERFACE);
    }
    if (device_handle) {
        libusb_close(device_handle);
    }
    if (context) {
        libusb_exit(context);
    }

    if (exit_code == 0) {
        printf("Logging finished. Data saved to %s\n", filename);
    }

    return exit_code;
}
