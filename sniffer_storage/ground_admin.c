/*
 * ground_admin.c
 *
 * Interactive PC-side ADMIN command sender for the Crazyflie USB -> UWB
 * downlink implemented by adhocuwb_sniffer.c.
 *
 * Build in an MSYS2 UCRT64 shell:
 *   gcc -std=c11 -Wall -Wextra -O2 ground_admin.c -o ground_admin.exe -lusb-1.0
 *
 * Example:
 *   ./ground_admin.exe
 *
 * The Crazyflie USB vendor interface must use a libusb-compatible Windows
 * driver (WinUSB/libusbK). Do not let cfclient claim this same USB device
 * while this program is running.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  elif __has_include(<libusb.h>)
#    include <libusb.h>
#  else
#    error "libusb header not found; install the MSYS2 libusb package"
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif

#define CF_USB_VID_DEFAULT       0x0483u
#define CF_USB_PID_DEFAULT       0x5740u
#define CF_USB_INTERFACE         0
#define CF_USB_EP_OUT            0x01u
#define CF_USB_EP_IN             0x81u
#define CF_USB_PACKET_SIZE       64

#define USB_TRANSFER_TIMEOUT_MS  1000u
#define ACK_WAIT_TIMEOUT_MS      1500u

#define SNIFFER_DOWNLINK_MAGIC   0x0000CCCCu
#define DOWNLINK_META_SIZE       18u
#define ADMIN_HEADER_SIZE        4u
#define UWB_DEST_ANY             0xFFFFu

#define SNIFFER_ACK_SIZE         4u

static const uint8_t snifferAck[SNIFFER_ACK_SIZE] = {
    0xACu, 0xC0u, 0x00u, 0x01u
};

typedef enum {
    BEE_EVENT_ARM = 0,
    BEE_EVENT_DISARM = 1,
    /* Event value 2 is reserved. */
    BEE_EVENT_TAKEOFF = 3,
    BEE_EVENT_START = 4,
    BEE_EVENT_HOVER = 5,
    BEE_EVENT_LAND = 6,
    BEE_EVENT_REBOOT = 7,
    BEE_EVENT_OTA = 8,
    BEE_EVENT_TIMEOUT = 9
} BeeEvent;

typedef struct {
    const char *name;
    uint16_t event;
} CommandEntry;

static const CommandEntry commandTable[] = {
    {"arm",     BEE_EVENT_ARM},
    {"disarm",  BEE_EVENT_DISARM},
    {"takeoff", BEE_EVENT_TAKEOFF},
    {"start",   BEE_EVENT_START},
    {"hover",   BEE_EVENT_HOVER},
    {"land",    BEE_EVENT_LAND},
    {"reboot",  BEE_EVENT_REBOOT},
    {"ota",     BEE_EVENT_OTA},
    {"timeout", BEE_EVENT_TIMEOUT},
};

typedef struct {
    uint16_t vid;
    uint16_t pid;
    unsigned int deviceIndex;
    int busNumber;
    int deviceAddress;
    int interfaceNumber;
    uint8_t endpointIn;
    uint8_t endpointOut;
    unsigned int timeoutMs;
    bool listDevices;
    bool waitForAck;
    bool verbose;
    bool dryRun;
    const char *oneShotCommand;
} Options;

static void printUsage(const char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("\n");
    printf("Options:\n");
    printf("  --device <value>    USB BUS:ADDRESS, or legacy match index (default 0)\n");
    printf("  --vid <value>       USB VID (default 0x%04X)\n", CF_USB_VID_DEFAULT);
    printf("  --pid <value>       USB PID (default 0x%04X)\n", CF_USB_PID_DEFAULT);
    printf("  --interface <value> USB interface number (default %d)\n", CF_USB_INTERFACE);
    printf("  --endpoint-in <n>   Bulk IN endpoint (default 0x%02X)\n", CF_USB_EP_IN);
    printf("  --endpoint-out <n>  Bulk OUT endpoint (default 0x%02X)\n", CF_USB_EP_OUT);
    printf("  --timeout-ms <n>    Bulk transfer timeout (default %u ms)\n",
           USB_TRANSFER_TIMEOUT_MS);
    printf("  --list-devices      List matching USB devices and exit\n");
    printf("  --command <name>    Send one command and exit\n");
    printf("  --no-ack            Do not wait for AC C0 00 01 after UWB TX\n");
    printf("  --verbose           Print every transmitted/received byte\n");
    printf("  --dry-run           Do not open USB; only build and print packets\n");
    printf("  --help              Show this help\n");
    printf("\n");
    printf("Interactive commands:\n");
    printf("  arm disarm takeoff start hover land reboot ota timeout\n");
    printf("  help                Show interactive help\n");
    printf("  quit / exit         Exit the program\n");
    printf("\n");
    printf("All ADMIN commands use broadcast UWB destination 0xFFFF.\n");
    printf("Numbers accept decimal or the 0x hexadecimal prefix.\n");
}

static bool parseUnsigned(const char *text, unsigned long maximum,
                          unsigned long *value);

static bool parseBusAddress(const char *text, int *bus, int *address)
{
    char *separator;
    char buffer[64];
    unsigned long parsedBus;
    unsigned long parsedAddress;

    if (text == NULL || strlen(text) >= sizeof(buffer)) {
        return false;
    }
    strcpy(buffer, text);
    separator = strchr(buffer, ':');
    if (separator == NULL || strchr(separator + 1, ':') != NULL) {
        return false;
    }
    *separator = '\0';
    if (!parseUnsigned(buffer, UINT8_MAX, &parsedBus) ||
        !parseUnsigned(separator + 1, UINT8_MAX, &parsedAddress)) {
        return false;
    }
    *bus = (int)parsedBus;
    *address = (int)parsedAddress;
    return true;
}

static bool parseUnsigned(const char *text, unsigned long maximum,
                          unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || *text == '\0' || *text == '-') {
        return false;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
        return false;
    }

    *value = parsed;
    return true;
}

static bool takeOptionValue(int argc, char **argv, int *index,
                            const char **value)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "Missing value after %s\n", argv[*index]);
        return false;
    }

    ++(*index);
    *value = argv[*index];
    return true;
}

static bool parseOptions(int argc, char **argv, Options *options)
{
    int i;

    options->vid = CF_USB_VID_DEFAULT;
    options->pid = CF_USB_PID_DEFAULT;
    options->deviceIndex = 0;
    options->busNumber = -1;
    options->deviceAddress = -1;
    options->interfaceNumber = CF_USB_INTERFACE;
    options->endpointIn = CF_USB_EP_IN;
    options->endpointOut = CF_USB_EP_OUT;
    options->timeoutMs = USB_TRANSFER_TIMEOUT_MS;
    options->listDevices = false;
    options->waitForAck = true;
    options->verbose = false;
    options->dryRun = false;
    options->oneShotCommand = NULL;

    for (i = 1; i < argc; ++i) {
        const char *value = NULL;
        unsigned long parsed = 0;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--device") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value)) {
                return false;
            }
            if (strchr(value, ':') != NULL) {
                if (!parseBusAddress(value, &options->busNumber,
                                     &options->deviceAddress)) {
                    fprintf(stderr, "Invalid USB device; expected BUS:ADDRESS\n");
                    return false;
                }
            } else if (!parseUnsigned(value, UINT_MAX, &parsed)) {
                fprintf(stderr, "Invalid USB device index or BUS:ADDRESS\n");
                return false;
            } else {
                options->deviceIndex = (unsigned int)parsed;
            }
        } else if (strcmp(argv[i], "--vid") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseUnsigned(value, UINT16_MAX, &parsed)) {
                fprintf(stderr, "Invalid USB VID\n");
                return false;
            }
            options->vid = (uint16_t)parsed;
        } else if (strcmp(argv[i], "--pid") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseUnsigned(value, UINT16_MAX, &parsed)) {
                fprintf(stderr, "Invalid USB PID\n");
                return false;
            }
            options->pid = (uint16_t)parsed;
        } else if (strcmp(argv[i], "--interface") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseUnsigned(value, INT_MAX, &parsed)) {
                fprintf(stderr, "Invalid USB interface number\n");
                return false;
            }
            options->interfaceNumber = (int)parsed;
        } else if (strcmp(argv[i], "--endpoint-in") == 0 ||
                   strcmp(argv[i], "--endpoint-out") == 0) {
            bool isInput = strcmp(argv[i], "--endpoint-in") == 0;
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseUnsigned(value, UINT8_MAX, &parsed)) {
                fprintf(stderr, "Invalid USB endpoint\n");
                return false;
            }
            if (isInput) {
                options->endpointIn = (uint8_t)parsed;
            } else {
                options->endpointOut = (uint8_t)parsed;
            }
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseUnsigned(value, UINT_MAX, &parsed) || parsed == 0) {
                fprintf(stderr, "Invalid USB timeout (must be greater than zero)\n");
                return false;
            }
            options->timeoutMs = (unsigned int)parsed;
        } else if (strcmp(argv[i], "--list-devices") == 0) {
            options->listDevices = true;
        } else if (strcmp(argv[i], "--command") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value)) {
                return false;
            }
            options->oneShotCommand = value;
        } else if (strcmp(argv[i], "--no-ack") == 0) {
            options->waitForAck = false;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options->verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            options->dryRun = true;
            options->verbose = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    if ((options->endpointIn & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN ||
        (options->endpointOut & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_OUT) {
        fprintf(stderr,
                "Endpoint direction mismatch: IN must have bit 0x80 set and "
                "OUT must have it clear\n");
        return false;
    }

    return true;
}

static void putU16Le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void putU32Le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
    destination[2] = (uint8_t)((value >> 16) & 0xFFu);
    destination[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void putU64Le(uint8_t *destination, uint64_t value)
{
    unsigned int i;

    for (i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (8u * i));
    }
}

static void printBytes(const char *label, const uint8_t *data, size_t length)
{
    size_t i;

    printf("%s (%zu bytes):", label, length);
    for (i = 0; i < length; ++i) {
        printf(" %02X", data[i]);
    }
    printf("\n");
}

static const CommandEntry *findCommand(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(commandTable) / sizeof(commandTable[0]); ++i) {
        if (strcmp(name, commandTable[i].name) == 0) {
            return &commandTable[i];
        }
    }

    return NULL;
}

static char *trimAndLower(char *text)
{
    char *start = text;
    char *end;
    char *cursor;

    while (isspace((unsigned char)*start)) {
        ++start;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';

    for (cursor = start; *cursor != '\0'; ++cursor) {
        *cursor = (char)tolower((unsigned char)*cursor);
    }

    return start;
}

static libusb_device_handle *openCrazyflie(libusb_context *context,
                                            const Options *options)
{
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    ssize_t count;
    ssize_t i;
    unsigned int matchIndex = 0;
    unsigned int matchCount = 0;

    count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        fprintf(stderr, "Unable to enumerate USB devices: %s\n",
                libusb_error_name((int)count));
        return NULL;
    }

    for (i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        int result = libusb_get_device_descriptor(devices[i], &descriptor);

        if (result != LIBUSB_SUCCESS || descriptor.idVendor != options->vid ||
            descriptor.idProduct != options->pid) {
            continue;
        }

        if ((options->busNumber >= 0 &&
             libusb_get_bus_number(devices[i]) != (uint8_t)options->busNumber) ||
            (options->deviceAddress >= 0 &&
             libusb_get_device_address(devices[i]) !=
                 (uint8_t)options->deviceAddress)) {
            continue;
        }

        if (matchIndex == options->deviceIndex) {
            result = libusb_open(devices[i], &handle);
            if (result != LIBUSB_SUCCESS) {
                fprintf(stderr,
                        "Found USB device %u at %u:%u but could not open it: "
                        "%s (%d)\n",
                        matchIndex, libusb_get_bus_number(devices[i]),
                        libusb_get_device_address(devices[i]),
                        libusb_error_name(result), result);
                handle = NULL;
            }
        }

        ++matchIndex;
        ++matchCount;
    }

    libusb_free_device_list(devices, 1);

    if (handle == NULL) {
        if (matchCount == 0) {
            fprintf(stderr,
                    "No Crazyflie USB device %04X:%04X matching the requested "
                    "BUS:ADDRESS was found.\n", options->vid, options->pid);
        } else if (options->deviceIndex >= matchCount) {
            fprintf(stderr,
                    "USB device index %u is out of range; found %u matching device(s).\n",
                    options->deviceIndex, matchCount);
        }
    }

    return handle;
}

static int listCrazyflies(libusb_context *context, const Options *options)
{
    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(context, &devices);
    int matches = 0;

    if (count < 0) {
        fprintf(stderr, "Unable to enumerate USB devices: %s (%d)\n",
                libusb_error_name((int)count), (int)count);
        return -1;
    }
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[i], &descriptor) ==
                LIBUSB_SUCCESS &&
            descriptor.idVendor == options->vid &&
            descriptor.idProduct == options->pid) {
            printf("device=%u:%u vid=0x%04X pid=0x%04X\n",
                   libusb_get_bus_number(devices[i]),
                   libusb_get_device_address(devices[i]),
                   descriptor.idVendor, descriptor.idProduct);
            ++matches;
        }
    }
    libusb_free_device_list(devices, 1);
    if (matches == 0) {
        printf("No matching USB devices found for %04X:%04X.\n",
               options->vid, options->pid);
    }
    return matches;
}

static bool prepareUsbInterface(libusb_device_handle *handle,
                                const Options *options)
{
    int configuration = 0;
    int result;

    result = libusb_get_configuration(handle, &configuration);
    if (result == LIBUSB_SUCCESS && configuration != 1) {
        result = libusb_set_configuration(handle, 1);
        if (result != LIBUSB_SUCCESS) {
            fprintf(stderr, "Could not select USB configuration 1: %s\n",
                    libusb_error_name(result));
            return false;
        }
    }

    /* This is useful on Linux and harmless/unsupported on Windows. */
    (void)libusb_set_auto_detach_kernel_driver(handle, 1);

    result = libusb_claim_interface(handle, options->interfaceNumber);
    if (result != LIBUSB_SUCCESS) {
        fprintf(stderr, "Could not claim Crazyflie USB interface %d: %s (%d)\n",
                options->interfaceNumber, libusb_error_name(result), result);
        fprintf(stderr,
                "On Windows, bind interface 0 to WinUSB/libusbK and close any "
                "program using this Crazyflie.\n");
        return false;
    }

#if !defined(__APPLE__)
    /* Crazyflie vendor request: route the USB link to the application. */
    result = libusb_control_transfer(handle,
                                     LIBUSB_ENDPOINT_OUT |
                                         LIBUSB_REQUEST_TYPE_VENDOR |
                                         LIBUSB_RECIPIENT_DEVICE,
                                     0x01,
                                     0,
                                     1,
                                     NULL,
                                     0,
                                     options->timeoutMs);
    if (result < 0) {
        fprintf(stderr,
                "Warning: USB link-select request failed: %s; continuing.\n",
                libusb_error_name(result));
    }
#endif

    return true;
}

static void restoreUsbInterface(libusb_device_handle *handle,
                                const Options *options)
{
    if (handle == NULL) {
        return;
    }

#if !defined(__APPLE__)
    /* Switch the Crazyflie back to its radio CRTP link when possible. */
    (void)libusb_control_transfer(handle,
                                  LIBUSB_ENDPOINT_OUT |
                                      LIBUSB_REQUEST_TYPE_VENDOR |
                                      LIBUSB_RECIPIENT_DEVICE,
                                  0x01,
                                  0,
                                  0,
                                  NULL,
                                  0,
                                  options->timeoutMs);
#endif
    (void)libusb_release_interface(handle, options->interfaceNumber);
}

static bool bulkWriteExact(libusb_device_handle *handle, uint8_t *data,
                           int length, const char *description,
                           const Options *options)
{
    int transferred = 0;
    int result = libusb_bulk_transfer(handle,
                                      options->endpointOut,
                                      data,
                                      length,
                                      &transferred,
                                      options->timeoutMs);

    if (result != LIBUSB_SUCCESS) {
        fprintf(stderr, "%s Bulk OUT endpoint 0x%02X failed: %s (%d)\n",
                description, options->endpointOut,
                libusb_error_name(result), result);
        return false;
    }

    if (transferred != length) {
        fprintf(stderr, "%s short USB transfer: %d/%d bytes\n",
                description, transferred, length);
        return false;
    }

    return true;
}

static bool waitForSnifferAck(libusb_device_handle *handle,
                              const Options *options)
{
    uint8_t received[CF_USB_PACKET_SIZE];
    unsigned int elapsed = 0;
    const unsigned int pollTimeout = 100;

    while (elapsed < ACK_WAIT_TIMEOUT_MS) {
        int transferred = 0;
        unsigned int remaining = ACK_WAIT_TIMEOUT_MS - elapsed;
        unsigned int timeout = options->timeoutMs < pollTimeout
                                   ? options->timeoutMs : pollTimeout;
        if (timeout > remaining) {
            timeout = remaining;
        }
        int result = libusb_bulk_transfer(handle,
                                          options->endpointIn,
                                          received,
                                          sizeof(received),
                                          &transferred,
                                          timeout);

        elapsed += timeout;

        if (result == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (result != LIBUSB_SUCCESS) {
            fprintf(stderr, "ACK Bulk IN endpoint 0x%02X failed: %s (%d)\n",
                    options->endpointIn, libusb_error_name(result), result);
            return false;
        }

        if (options->verbose) {
            printBytes("USB RX", received, (size_t)transferred);
        }

        if (transferred == (int)SNIFFER_ACK_SIZE &&
            memcmp(received, snifferAck, SNIFFER_ACK_SIZE) == 0) {
            return true;
        }

        if (!options->verbose) {
            printf("Received a non-ACK USB packet (%d bytes); still waiting.\n",
                   transferred);
        }
    }

    fprintf(stderr,
            "Timed out waiting for UWB TX ACK (expected AC C0 00 01).\n");
    return false;
}

static bool sendAdminCommand(libusb_device_handle *handle,
                             const Options *options,
                             uint16_t sequence,
                             const CommandEntry *command)
{
    uint8_t metadata[DOWNLINK_META_SIZE] = {0};
    uint8_t adminPayload[ADMIN_HEADER_SIZE] = {0};

    putU32Le(&metadata[0], SNIFFER_DOWNLINK_MAGIC);
    putU16Le(&metadata[4], UWB_DEST_ANY);
    putU16Le(&metadata[6], sequence);
    putU16Le(&metadata[8], ADMIN_HEADER_SIZE);
    putU64Le(&metadata[10], 0); /* txTime is currently unused by the firmware. */

    putU16Le(&adminPayload[0], command->event);
    putU16Le(&adminPayload[2], 0); /* All current terminal commands have no body. */

    printf("TX command=%s event=%u dest=0x%04X seq=%u\n",
           command->name, command->event, UWB_DEST_ANY, sequence);

    if (options->verbose) {
        printBytes("Downlink meta", metadata, sizeof(metadata));
        printBytes("ADMIN payload", adminPayload, sizeof(adminPayload));
    }

    if (options->dryRun) {
        printf("DRY RUN: USB transfer skipped\n");
        return true;
    }

    /*
     * Keep these as two separate bulk transfers. The firmware recognizes the
     * 18-byte metadata USB packet first, then consumes subsequent USB packets
     * as its payload.
     */
    if (!bulkWriteExact(handle, metadata, sizeof(metadata), "Metadata", options)) {
        return false;
    }
    if (!bulkWriteExact(handle, adminPayload, sizeof(adminPayload),
                        "ADMIN payload", options)) {
        return false;
    }

    if (options->waitForAck) {
        if (!waitForSnifferAck(handle, options)) {
            return false;
        }
        printf("UWB TX ACK received\n");
    }

    return true;
}

static void printInteractiveHelp(void)
{
    printf("Commands: arm, disarm, takeoff, start, hover, land, reboot, ota, timeout\n");
    printf("          help, quit, exit\n");
    printf("UWB destination is fixed to broadcast address 0xFFFF.\n");
    printf("Expected main path: arm -> takeoff -> start -> hover -> land -> reboot\n");
}

static int runOneCommand(libusb_device_handle *handle, Options *options,
                         uint16_t *sequence, const char *name)
{
    char commandBuffer[64];
    char *commandName;
    const CommandEntry *command;

    if (strlen(name) >= sizeof(commandBuffer)) {
        fprintf(stderr, "Command is too long\n");
        return EXIT_FAILURE;
    }

    strcpy(commandBuffer, name);
    commandName = trimAndLower(commandBuffer);
    command = findCommand(commandName);
    if (command == NULL) {
        fprintf(stderr, "Unknown command: %s\n", commandName);
        return EXIT_FAILURE;
    }

    if (!sendAdminCommand(handle, options, *sequence, command)) {
        return EXIT_FAILURE;
    }

    ++(*sequence);
    return EXIT_SUCCESS;
}

static int runInteractive(libusb_device_handle *handle, Options *options)
{
    char input[128];
    uint16_t sequence = 1;

    printInteractiveHelp();

    while (true) {
        char *commandLine;
        const CommandEntry *command;

        printf("admin> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            break;
        }

        commandLine = trimAndLower(input);
        if (*commandLine == '\0') {
            continue;
        }

        if (strcmp(commandLine, "quit") == 0 ||
            strcmp(commandLine, "exit") == 0) {
            break;
        }
        if (strcmp(commandLine, "help") == 0) {
            printInteractiveHelp();
            continue;
        }
        command = findCommand(commandLine);
        if (command == NULL) {
            fprintf(stderr, "Unknown command: %s (enter help for a list)\n",
                    commandLine);
            continue;
        }
        if (sendAdminCommand(handle, options, sequence, command)) {
            ++sequence;
        } else {
            fprintf(stderr,
                    "Command failed. Check the USB link and cfclient console.\n");
        }
    }

    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    Options options;
    libusb_context *usbContext = NULL;
    libusb_device_handle *usbHandle = NULL;
    bool interfaceClaimed = false;
    int result = EXIT_FAILURE;
    int usbResult;

    if (!parseOptions(argc, argv, &options)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!options.dryRun || options.listDevices) {
        usbResult = libusb_init(&usbContext);
        if (usbResult != LIBUSB_SUCCESS) {
            fprintf(stderr, "libusb initialization failed: %s\n",
                    libusb_error_name(usbResult));
            goto cleanup;
        }

        if (options.listDevices) {
            result = listCrazyflies(usbContext, &options) < 0
                         ? EXIT_FAILURE : EXIT_SUCCESS;
            goto cleanup;
        }

        usbHandle = openCrazyflie(usbContext, &options);
        if (usbHandle == NULL) {
            goto cleanup;
        }

        if (!prepareUsbInterface(usbHandle, &options)) {
            goto cleanup;
        }
        interfaceClaimed = true;

        printf("Opened Crazyflie USB %04X:%04X at %u:%u, interface %d, "
               "Bulk OUT 0x%02X, Bulk IN 0x%02X\n",
               options.vid, options.pid,
               libusb_get_bus_number(libusb_get_device(usbHandle)),
               libusb_get_device_address(libusb_get_device(usbHandle)),
               options.interfaceNumber, options.endpointOut,
               options.endpointIn);
    } else {
        printf("Dry-run mode: no USB device will be opened.\n");
    }

    if (options.oneShotCommand != NULL) {
        uint16_t sequence = 1;
        result = runOneCommand(usbHandle, &options, &sequence,
                               options.oneShotCommand);
    } else {
        result = runInteractive(usbHandle, &options);
    }

cleanup:
    if (interfaceClaimed) {
        restoreUsbInterface(usbHandle, &options);
    }
    if (usbHandle != NULL) {
        libusb_close(usbHandle);
    }
    if (usbContext != NULL) {
        libusb_exit(usbContext);
    }

    return result;
}
