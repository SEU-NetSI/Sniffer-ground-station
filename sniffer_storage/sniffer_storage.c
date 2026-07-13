#include "libusb_transport.h"
#include "sniffer_buffered_transport.h"
#include "sniffer_capture.h"
#include "sniffer_hex.h"
#ifdef SNIFFER_MACOS_PLOT
#include "sniffer_plot_macos.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FILENAME_SIZE 128u
#define SEND_BUFFER_SIZE 4096u
#define RECEIVE_STAGING_SIZE 16384u
#define WRITE_TIMEOUT_LIMIT 3u

typedef struct {
    LibusbDeviceOptions usb;
    const char *output_path;
    const char *send_hex;
    const char *plot_output;
    size_t max_payload;
    uint64_t max_records;
    double plot_period_ms;
    int list_devices;
    int send_only;
    int print_records;
    int print_payload;
    int print_stats;
    int plot_after_capture;
} ProgramOptions;

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [options]\n"
            "\n"
            "Capture raw Sniffer records over libusb bulk transfers. This is not\n"
            "a serial/termios program; /dev/cu.* paths and baud rates do not apply.\n"
            "\n"
            "Options:\n"
            "  -o, --output PATH       Binary capture path (default: timestamped .bin)\n"
            "      --vid VALUE         USB vendor ID (default: 0x0483)\n"
            "      --pid VALUE         USB product ID (default: 0x5740)\n"
            "      --device BUS:ADDR   Select USB device (auto if exactly one match)\n"
            "      --interface N       USB interface number (default: 0)\n"
            "      --endpoint-in N     Bulk IN endpoint (default: 0x81)\n"
            "      --endpoint-out N    Bulk OUT endpoint for raw send (default: 0x01)\n"
            "      --timeout-ms N      Per-transfer timeout (default: 5000)\n"
            "      --max-payload N     Accepted Sniffer payload limit (default: 4096)\n"
            "      --max-records N     Stop after N valid records (0: unlimited)\n"
            "      --print-records     Print original multi-line meta (default: on)\n"
            "      --no-print-records  Disable per-meta terminal output\n"
            "      --print-payload     Also print payload hex\n"
            "      --print-stats       Print final detailed capture statistics\n"
            "      --plot-after-capture  Generate phase.png after capture (macOS)\n"
            "      --plot-output DIR   Auto-plot output directory\n"
            "      --plot-period-ms N  Auto-plot period (default: 60)\n"
            "      --list-devices      List matching VID/PID devices without opening\n"
            "      --send-hex HEX      Send exact raw bytes on Bulk OUT before capture\n"
            "      --send HEX          Alias for --send-hex\n"
            "      --send-only         Send and exit without opening an output file\n"
            "  -h, --help              Show this help\n"
            "\n"
            "Warning: --send-hex confirms USB transfer only. This repository contains\n"
            "no verified device-side Sniffer command handler; supply only a frame that\n"
            "the installed firmware explicitly supports.\n",
            program);
}

static int parse_unsigned(const char *text, uint64_t maximum, uint64_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_positive_double(const char *text, double *value) {
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !(parsed > 0.0) || parsed > 1000000000.0) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_device(const char *text, int *bus, int *address) {
    char *end = NULL;
    errno = 0;
    long parsed_bus = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != ':' || parsed_bus < 0 || parsed_bus > 255) {
        return -1;
    }
    const char *address_text = end + 1;
    errno = 0;
    long parsed_address = strtol(address_text, &end, 10);
    if (errno != 0 || end == address_text || *end != '\0' ||
        parsed_address < 0 || parsed_address > 255) {
        return -1;
    }
    *bus = (int)parsed_bus;
    *address = (int)parsed_address;
    return 0;
}

static int require_value(int argc, char **argv, int *index, const char **value) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", argv[*index]);
        return -1;
    }
    *value = argv[++(*index)];
    return 0;
}

static int parse_options(int argc, char **argv, ProgramOptions *options) {
    const char *value = NULL;
    uint64_t number = 0;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0) {
            usage(stdout, argv[0]);
            return 1;
        }
        if (strcmp(argv[index], "--list-devices") == 0) {
            options->list_devices = 1;
            continue;
        }
        if (strcmp(argv[index], "--send-only") == 0) {
            options->send_only = 1;
            continue;
        }
        if (strcmp(argv[index], "--print-records") == 0) {
            options->print_records = 1;
            continue;
        }
        if (strcmp(argv[index], "--no-print-records") == 0) {
            options->print_records = 0;
            continue;
        }
        if (strcmp(argv[index], "--print-payload") == 0) {
            options->print_payload = 1;
            continue;
        }
        if (strcmp(argv[index], "--print-stats") == 0) {
            options->print_stats = 1;
            continue;
        }
        if (strcmp(argv[index], "--plot-after-capture") == 0) {
            options->plot_after_capture = 1;
            continue;
        }
        if (strcmp(argv[index], "--plot-output") == 0) {
            if (require_value(argc, argv, &index, &options->plot_output) != 0) {
                return -1;
            }
            options->plot_after_capture = 1;
            continue;
        }
        if (strcmp(argv[index], "--plot-period-ms") == 0) {
            if (require_value(argc, argv, &index, &value) != 0 ||
                parse_positive_double(value, &options->plot_period_ms) != 0) {
                fprintf(stderr, "Invalid value for --plot-period-ms\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "-o") == 0 || strcmp(argv[index], "--output") == 0) {
            if (require_value(argc, argv, &index, &options->output_path) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--send") == 0 || strcmp(argv[index], "--send-hex") == 0) {
            if (require_value(argc, argv, &index, &options->send_hex) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--device") == 0) {
            if (require_value(argc, argv, &index, &value) != 0 ||
                parse_device(value, &options->usb.bus_number,
                             &options->usb.device_address) != 0) {
                fprintf(stderr, "Invalid --device; expected BUS:ADDRESS\n");
                return -1;
            }
            continue;
        }

#define PARSE_NUMBER_OPTION(name, maximum, destination)                         \
        if (strcmp(argv[index], name) == 0) {                                   \
            if (require_value(argc, argv, &index, &value) != 0 ||               \
                parse_unsigned(value, maximum, &number) != 0) {                  \
                fprintf(stderr, "Invalid value for %s\n", name);               \
                return -1;                                                       \
            }                                                                    \
            destination = number;                                                \
            continue;                                                            \
        }

        PARSE_NUMBER_OPTION("--vid", UINT16_MAX, options->usb.vendor_id)
        PARSE_NUMBER_OPTION("--pid", UINT16_MAX, options->usb.product_id)
        PARSE_NUMBER_OPTION("--interface", INT32_MAX, options->usb.interface_number)
        PARSE_NUMBER_OPTION("--endpoint-in", UINT8_MAX, options->usb.endpoint_in)
        PARSE_NUMBER_OPTION("--endpoint-out", UINT8_MAX, options->usb.endpoint_out)
        PARSE_NUMBER_OPTION("--timeout-ms", UINT32_MAX, options->usb.timeout_ms)
        PARSE_NUMBER_OPTION("--max-payload", UINT16_MAX, options->max_payload)
        PARSE_NUMBER_OPTION("--max-records", UINT64_MAX, options->max_records)
#undef PARSE_NUMBER_OPTION

        if (strcmp(argv[index], "--baud") == 0) {
            fprintf(stderr, "--baud is not supported: this device uses raw libusb, not a serial port\n");
            return -1;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[index]);
        return -1;
    }
    if (options->send_only && options->send_hex == NULL) {
        fprintf(stderr, "--send-only requires --send-hex\n");
        return -1;
    }
    if (options->print_payload && !options->print_records) {
        fprintf(stderr, "--print-payload requires --print-records\n");
        return -1;
    }
    if (options->max_payload == 0) {
        fprintf(stderr, "--max-payload must be greater than zero\n");
        return -1;
    }
    if (options->usb.timeout_ms == 0) {
        fprintf(stderr, "--timeout-ms must be greater than zero\n");
        return -1;
    }
    if (options->send_hex != NULL &&
        (options->usb.bus_number < 0 || options->usb.device_address < 0)) {
        fprintf(stderr, "--send-hex requires explicit --device BUS:ADDRESS\n");
        return -1;
    }
    if ((options->usb.endpoint_in & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN ||
        (options->usb.endpoint_out & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_OUT) {
        fprintf(stderr, "Endpoint direction mismatch: IN needs bit 0x80, OUT must not have it\n");
        return -1;
    }
    return 0;
}

static int generate_filename(char *buffer, size_t capacity) {
    time_t now = time(NULL);
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL) {
        return -1;
    }
    return strftime(buffer, capacity, "raw_sensor_data_%Y%m%d_%H%M%S.bin", &local) > 0
               ? 0
               : -1;
}

int main(int argc, char **argv) {
    ProgramOptions options = {
        .usb = {
            .vendor_id = 0x0483,
            .product_id = 0x5740,
            .interface_number = 0,
            .endpoint_in = 0x81,
            .endpoint_out = 0x01,
            .timeout_ms = 5000,
            .bus_number = -1,
            .device_address = -1,
        },
        .max_payload = SNIFFER_DEFAULT_MAX_PAYLOAD,
        .plot_period_ms = 60.0,
        .print_records = 1,
    };
    int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }

    if (options.list_devices) {
        int matches = libusb_transport_list(&options.usb, stdout);
        if (matches < 0) {
            fprintf(stderr, "USB enumeration failed\n");
            return 1;
        }
        if (matches == 0) {
            printf("No matching devices found\n");
        }
        return 0;
    }

    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    uint8_t send_buffer[SEND_BUFFER_SIZE];
    size_t send_length = 0;
    char send_text[SEND_BUFFER_SIZE * 2 + 1];
    if (options.send_hex != NULL) {
        if (sniffer_parse_hex(options.send_hex, send_buffer,
                              sizeof(send_buffer), &send_length) != 0 ||
            sniffer_format_hex(send_buffer, send_length, send_text,
                               sizeof(send_text)) != 0) {
            fprintf(stderr, "Invalid --send-hex value or frame exceeds %u bytes\n",
                    SEND_BUFFER_SIZE);
            return 2;
        }
    }

    LibusbTransport usb;
    int capture_enabled = !options.send_only;
    if (libusb_transport_open(&usb, &options.usb, capture_enabled,
                              options.send_hex != NULL) != 0) {
        return 1;
    }
    SnifferTransport usb_adapter = libusb_transport_adapter(&usb);
    uint8_t receive_staging[RECEIVE_STAGING_SIZE];
    SnifferBufferedTransport buffered;
    if (sniffer_buffered_transport_init(&buffered, usb_adapter,
                                        receive_staging,
                                        sizeof(receive_staging)) != 0) {
        fprintf(stderr, "Could not initialize USB receive staging buffer\n");
        libusb_transport_close(&usb);
        return 1;
    }
    SnifferTransport transport = sniffer_buffered_transport_adapter(&buffered);

    int exit_code = 0;
    if (options.send_hex != NULL) {
        printf("Sending %zu raw byte(s) to bus=%d address=%d endpoint=0x%02X: %s\n",
               send_length, usb.options.bus_number, usb.options.device_address,
               usb.options.endpoint_out, send_text);
        printf("USB transfer success does not prove application-level command handling.\n");
        uint64_t write_timeouts = 0;
        if (sniffer_transport_write_all(&transport, send_buffer, send_length,
                                        WRITE_TIMEOUT_LIMIT, &keep_running,
                                        &write_timeouts) != 0) {
            fprintf(stderr, "Bulk OUT transfer failed after %" PRIu64 " timeout(s)\n",
                    write_timeouts);
            exit_code = 1;
            goto cleanup;
        }
        printf("Bulk OUT transfer completed (%" PRIu64 " timeout(s))\n",
               write_timeouts);
    }

    if (capture_enabled) {
        char generated[FILENAME_SIZE];
        if (options.output_path == NULL) {
            if (generate_filename(generated, sizeof(generated)) != 0) {
                fprintf(stderr, "Could not generate output filename\n");
                exit_code = 1;
                goto cleanup;
            }
            options.output_path = generated;
        }
        FILE *output = fopen(options.output_path, "wb");
        if (output == NULL) {
            perror("open output");
            exit_code = 1;
            goto cleanup;
        }

        printf("Saving raw sniffer data to %s\n", options.output_path);
        SnifferCaptureOptions capture_options = {
            .max_payload = options.max_payload,
            .max_records = options.max_records,
            .flush_each_record = 1,
            .print_records = options.print_records,
            .print_payload = options.print_payload,
            .print_stream = options.print_records ? stdout : NULL,
        };
        SnifferStats stats = {0};
        if (sniffer_capture(output, &transport, &capture_options,
                            &keep_running, &stats) != 0) {
            exit_code = 1;
        }
        int output_closed = fclose(output) == 0;
        if (!output_closed) {
            perror("close output");
            stats.file_write_failures++;
            exit_code = 1;
        }
        if (options.print_stats) {
            sniffer_print_stats(stdout, &stats);
        }
        if (options.plot_after_capture && output_closed && stats.records > 0) {
#ifdef SNIFFER_MACOS_PLOT
            if (sniffer_plot_macos_run(options.output_path, options.plot_output,
                                       options.plot_period_ms) != 0) {
                exit_code = 1;
            }
#else
            fprintf(stderr, "--plot-after-capture is available only in the macOS build\n");
            exit_code = 1;
#endif
        }
        if (exit_code == 0) {
            printf("Logging finished. Data saved to %s\n", options.output_path);
        }
    }

cleanup:
    libusb_transport_close(&usb);
    return exit_code;
}
