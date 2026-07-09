#include <libusb-1.0/libusb.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define FILENAME_SIZE     64
#define MAX_TRANSFER_SIZE 256
#define META_SIZE         ((int)sizeof(Sniffer_Meta_t))
#define MAGIC_MATCH       0xBBBBu
#define VENDOR_ID         0x0483
#define PRODUCT_ID        0x5740
#define USB_ENDPOINT_IN   0x81
#define USB_TIMEOUT_MS    5000


char filename[FILENAME_SIZE];
volatile sig_atomic_t keep_running = 1;


typedef union {
    uint8_t raw[18];
    struct {
        uint32_t magic;
        uint16_t senderAddress;
        uint16_t seqNumber;
        uint16_t msgLength;
        uint64_t rxTime;
    } __attribute__((packed));
} __attribute__((packed)) Sniffer_Meta_t;


void handle_sigint(int sigint) {
    (void)sigint;
    keep_running = 0;
}

void generate_filename(char *buffer, size_t buffer_size) {
    time_t rawtime;
    struct tm *curtime;
    time(&rawtime);
    curtime = localtime(&rawtime);
    strftime(buffer, buffer_size, "raw_sensor_data_%Y%m%d_%H%M%S.bin", curtime);
}

void printMetaInfo(const Sniffer_Meta_t *meta) {
    printf("Sniffer_Meta_t:\n");
    printf("  Magic: 0x%08" PRIX32 "\n", meta->magic);
    printf("  Sender Address: %" PRIu16 "\n", meta->senderAddress);
    printf("  Sequence Number: %" PRIu16 "\n", meta->seqNumber);
    printf("  Message Length: %" PRIu16 "\n", meta->msgLength);
    printf("  RX Time: 0x%016" PRIX64 "\n", meta->rxTime);
}

int read_exact_usb(libusb_device_handle *device_handle, uint8_t *buffer, int length, const char *label) {
    int total = 0;

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
        }

        if (response != 0) {
            if (!keep_running) {
                break;
            }

            if (transferred > 0 && response == LIBUSB_ERROR_TIMEOUT) {
                continue;
            }

            fprintf(stderr, "Bulk transfer failed while reading %s: %s\n",
                    label, libusb_strerror(response));
            return -1;
        }

        if (transferred == 0) {
            fprintf(stderr, "Bulk transfer returned no data while reading %s\n", label);
            return -1;
        }
    }

    return total;
}

int write_binary(FILE *file, const uint8_t *buffer, size_t length, const char *label) {
    if (fwrite(buffer, 1, length, file) != length) {
        fprintf(stderr, "Write %s failed\n", label);
        return -1;
    }

    return 0;
}

int storeSnifferBinaryData(FILE *file, libusb_device_handle *device_handle) {
    uint8_t payload[MAX_TRANSFER_SIZE];
    unsigned long packet_count = 0;

    while (keep_running) {
        Sniffer_Meta_t meta;
        int received = read_exact_usb(device_handle, meta.raw, META_SIZE, "Sniffer_Meta_t");
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
        printMetaInfo(&meta);

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

int main() {
    libusb_device_handle *device_handle = NULL;
    libusb_context *context = NULL;
    FILE *log_file = NULL;
    int interface_claimed = 0;
    int exit_code = 1;

    signal(SIGINT, handle_sigint);

    int response = libusb_init(&context);
    if (response < 0) {
        fprintf(stderr, "libusb init error\n");
        return exit_code;
    }

    device_handle = libusb_open_device_with_vid_pid(context, VENDOR_ID, PRODUCT_ID);
    if (!device_handle) {
        fprintf(stderr, "cannot find USB device\n");
        goto cleanup;
    }

    libusb_set_auto_detach_kernel_driver(device_handle, 1);
    response = libusb_claim_interface(device_handle, 0);
    if (response != 0) {
        fprintf(stderr, "libusb claim interface error: %s\n", libusb_strerror(response));
        goto cleanup;
    }
    interface_claimed = 1;

    generate_filename(filename, sizeof(filename));
    log_file = fopen(filename, "wb");
    if (!log_file) {
        perror("open log file");
        goto cleanup;
    }

    printf("Saving raw sniffer data to %s\n", filename);
    if (storeSnifferBinaryData(log_file, device_handle) != 0) {
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    if (log_file) {
        fclose(log_file);
    }
    if (interface_claimed) {
        libusb_release_interface(device_handle, 0);
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
