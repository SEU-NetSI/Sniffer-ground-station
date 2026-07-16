/*
 * ground_ota.c
 *
 * PC-side OTA image broadcaster for the Crazyflie USB -> AdHocUWB bridge.
 * It sends typed USB downlink metadata followed by an OTA payload. The relay
 * Crazyflie constructs UWB_Packet_t with type UWB_OTA_MESSAGE (11).
 *
 * Build in an MSYS2 UCRT64 shell:
 *   gcc -std=c11 -Wall -Wextra -O2 ground_ota.c -o ground_ota.exe -lusb-1.0
 *
 * Interactive use:
 *   ./ground_ota.exe
 *
 * Command-line use:
 *   ./ground_ota.exe --file "C:\\path\\firmware.bin" --image-type firmware
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

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

#define CF_USB_VID_DEFAULT             0x0483u
#define CF_USB_PID_DEFAULT             0x5740u
#define CF_USB_INTERFACE               0
#define CF_USB_EP_OUT                  0x01u
#define CF_USB_EP_IN                   0x81u
#define CF_USB_PACKET_SIZE             64u
#define USB_TRANSFER_TIMEOUT_MS        1000u
#define LOCAL_TX_ACK_TIMEOUT_MS        1500u

/* Backward-compatible typed USB -> UWB metadata. */
#define SNIFFER_DOWNLINK_TYPED_MAGIC   0x0000CCCDu
#define DOWNLINK_TYPED_META_SIZE       20u
#define UWB_DEST_ANY                   0xFFFFu
#define UWB_OTA_MESSAGE                11u

#define SNIFFER_ACK_SIZE               4u

/* OTA wire protocol. */
#define OTA_PACKET_START               0u
#define OTA_PACKET_DATA                1u
#define OTA_PACKET_END                 2u
#define OTA_IMAGE_BOOTLOADER           0u
#define OTA_IMAGE_FIRMWARE             1u
#define OTA_HEADER_SIZE                6u
#define OTA_DATA_PAYLOAD_MAX           224u
#define OTA_START_INFO_SIZE            8u
#define OTA_START_MESSAGE_SIZE         (OTA_HEADER_SIZE + OTA_START_INFO_SIZE)
#define OTA_END_MESSAGE_SIZE           OTA_HEADER_SIZE
#define OTA_MESSAGE_SIZE_MAX           (OTA_HEADER_SIZE + OTA_DATA_PAYLOAD_MAX)
#define UWB_PACKET_HEADER_SIZE         8u
#define FIRMWARE_IMAGE_SIZE_MAX        262112u

/* Conservative first-integration timing. */
#define START_ANNOUNCE_DURATION_MS      10000u
#define START_ROUND_INTERVAL_MS        250u
#define PACKET_REPEAT_COUNT             3u
#define PACKET_REPEAT_INTERVAL_MS       3u
#define DATA_CHUNK_INTERVAL_MS          3u
#define DATA_PROGRESS_INTERVAL          3u

#define CRC32_INIT                      0xFFFFFFFFu
#define CRC32_POLY_REFLECTED            0xEDB88320u
#define CRC32_XOR_OUT                   0xFFFFFFFFu

#define INPUT_PATH_SIZE                 4096u

static const uint8_t localTxAck[SNIFFER_ACK_SIZE] = {
    0xACu, 0xC0u, 0x00u, 0x01u
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
    uint8_t imageType;
    const char *filePath;
    bool listDevices;
    bool assumeYes;
    bool dryRun;
    bool verbose;
    bool selfTestOnly;
    bool waitForLocalAck;
} Options;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t crc32;
} Image;

_Static_assert(OTA_START_MESSAGE_SIZE == 14u,
               "START OTA message must be 14 bytes");
_Static_assert(OTA_MESSAGE_SIZE_MAX == 230u,
               "Full DATA OTA message must be 230 bytes");
_Static_assert(UWB_PACKET_HEADER_SIZE + OTA_START_MESSAGE_SIZE == 22u,
               "START UWB length must be 22 bytes");
_Static_assert(UWB_PACKET_HEADER_SIZE + OTA_MESSAGE_SIZE_MAX == 238u,
               "Full DATA UWB length must be 238 bytes");
_Static_assert(UWB_PACKET_HEADER_SIZE + OTA_END_MESSAGE_SIZE == 14u,
               "END UWB length must be 14 bytes");

static void printUsage(const char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("\n");
    printf("Options:\n");
    printf("  --file <path>              BIN image path; prompted if omitted\n");
    printf("  --image-type <value>       firmware/1 (default) or bootloader/0\n");
    printf("  --device <value>           USB BUS:ADDRESS, or legacy match index (default 0)\n");
    printf("  --vid <value>              USB VID (default 0x%04X)\n",
           CF_USB_VID_DEFAULT);
    printf("  --pid <value>              USB PID (default 0x%04X)\n",
           CF_USB_PID_DEFAULT);
    printf("  --interface <value>        USB interface number (default %d)\n",
           CF_USB_INTERFACE);
    printf("  --endpoint-in <value>      Bulk IN endpoint (default 0x%02X)\n",
           CF_USB_EP_IN);
    printf("  --endpoint-out <value>     Bulk OUT endpoint (default 0x%02X)\n",
           CF_USB_EP_OUT);
    printf("  --timeout-ms <value>       Bulk transfer timeout (default %u ms)\n",
           USB_TRANSFER_TIMEOUT_MS);
    printf("  --list-devices             List matching USB devices and exit\n");
    printf("  --yes                      Skip receiver-ready confirmation\n");
    printf("  --no-local-ack             Do not wait for relay UWB TX completion\n");
    printf("  --verbose                  Print USB metadata and local ACK bytes\n");
    printf("  --dry-run                  Build all packets without opening USB\n");
    printf("  --self-test                Run CRC/serialization tests and exit\n");
    printf("  --help                     Show this help\n");
    printf("\n");
    printf("All OTA packets use UWB broadcast destination 0xFFFF.\n");
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

static bool parseImageType(const char *text, uint8_t *imageType)
{
    if (strcmp(text, "firmware") == 0 || strcmp(text, "fw") == 0 ||
        strcmp(text, "1") == 0) {
        *imageType = OTA_IMAGE_FIRMWARE;
        return true;
    }

    if (strcmp(text, "bootloader") == 0 || strcmp(text, "bl") == 0 ||
        strcmp(text, "0") == 0) {
        *imageType = OTA_IMAGE_BOOTLOADER;
        return true;
    }

    return false;
}

static bool parseOptions(int argc, char **argv, Options *options)
{
    int i;

    options->vid = CF_USB_VID_DEFAULT;
    options->pid = CF_USB_PID_DEFAULT;
    options->deviceIndex = 0u;
    options->busNumber = -1;
    options->deviceAddress = -1;
    options->interfaceNumber = CF_USB_INTERFACE;
    options->endpointIn = CF_USB_EP_IN;
    options->endpointOut = CF_USB_EP_OUT;
    options->timeoutMs = USB_TRANSFER_TIMEOUT_MS;
    options->imageType = OTA_IMAGE_FIRMWARE;
    options->filePath = NULL;
    options->listDevices = false;
    options->assumeYes = false;
    options->dryRun = false;
    options->verbose = false;
    options->selfTestOnly = false;
    options->waitForLocalAck = true;

    for (i = 1; i < argc; ++i) {
        const char *value = NULL;
        unsigned long parsed = 0;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--file") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value)) {
                return false;
            }
            options->filePath = value;
        } else if (strcmp(argv[i], "--image-type") == 0) {
            if (!takeOptionValue(argc, argv, &i, &value) ||
                !parseImageType(value, &options->imageType)) {
                fprintf(stderr,
                        "Invalid image type; use firmware/1 or bootloader/0\n");
                return false;
            }
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
                !parseUnsigned(value, UINT_MAX, &parsed) || parsed == 0u) {
                fprintf(stderr, "Invalid USB timeout (must be greater than zero)\n");
                return false;
            }
            options->timeoutMs = (unsigned int)parsed;
        } else if (strcmp(argv[i], "--list-devices") == 0) {
            options->listDevices = true;
        } else if (strcmp(argv[i], "--yes") == 0) {
            options->assumeYes = true;
        } else if (strcmp(argv[i], "--no-local-ack") == 0) {
            options->waitForLocalAck = false;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options->verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            options->dryRun = true;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            options->selfTestOnly = true;
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

    for (i = 0; i < 8u; ++i) {
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

static uint32_t crc32IsoHdlc(const uint8_t *data, size_t length)
{
    uint32_t crc = CRC32_INIT;
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned int bit;

        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ CRC32_POLY_REFLECTED;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc ^ CRC32_XOR_OUT;
}

static size_t serializeOtaMessage(uint8_t *output,
                                  uint8_t commandType,
                                  uint8_t imageType,
                                  uint16_t payloadLength,
                                  uint16_t chunkIndex,
                                  const uint8_t *body)
{
    output[0] = commandType;
    output[1] = imageType;
    putU16Le(&output[2], payloadLength);
    putU16Le(&output[4], chunkIndex);

    if (payloadLength > 0u && body != NULL) {
        memcpy(&output[OTA_HEADER_SIZE], body, payloadLength);
    }

    return OTA_HEADER_SIZE + payloadLength;
}

static size_t buildStartMessage(uint8_t *output, uint8_t imageType,
                                uint32_t imageSize, uint32_t imageCrc32)
{
    uint8_t body[OTA_START_INFO_SIZE];

    putU32Le(&body[0], imageSize);
    putU32Le(&body[4], imageCrc32);
    return serializeOtaMessage(output, OTA_PACKET_START, imageType,
                               OTA_START_INFO_SIZE, 0u, body);
}

static size_t buildDataMessage(uint8_t *output, uint8_t imageType,
                               uint16_t chunkIndex, const uint8_t *data,
                               uint16_t dataLength)
{
    return serializeOtaMessage(output, OTA_PACKET_DATA, imageType,
                               dataLength, chunkIndex, data);
}

static size_t buildEndMessage(uint8_t *output, uint8_t imageType)
{
    return serializeOtaMessage(output, OTA_PACKET_END, imageType, 0u, 0u,
                               NULL);
}

static bool runSelfTests(void)
{
    static const uint8_t crcInput[] = "123456789";
    static const uint8_t expectedStart[OTA_START_MESSAGE_SIZE] = {
        0x00, 0x01, 0x08, 0x00, 0x00, 0x00,
        0x45, 0x23, 0x01, 0x00, 0xEF, 0xCD, 0xAB, 0x89
    };
    static const uint8_t expectedEnd[OTA_END_MESSAGE_SIZE] = {
        0x02, 0x01, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t actual[OTA_START_MESSAGE_SIZE];
    size_t length;

    if (crc32IsoHdlc(crcInput, sizeof(crcInput) - 1u) != 0xCBF43926u) {
        fprintf(stderr, "SELF-TEST FAILED: CRC32 vector\n");
        return false;
    }

    length = buildStartMessage(actual, OTA_IMAGE_FIRMWARE,
                               0x00012345u, 0x89ABCDEFu);
    if (length != sizeof(expectedStart) ||
        memcmp(actual, expectedStart, sizeof(expectedStart)) != 0) {
        fprintf(stderr, "SELF-TEST FAILED: START serialization\n");
        return false;
    }

    length = buildEndMessage(actual, OTA_IMAGE_FIRMWARE);
    if (length != sizeof(expectedEnd) ||
        memcmp(actual, expectedEnd, sizeof(expectedEnd)) != 0) {
        fprintf(stderr, "SELF-TEST FAILED: END serialization\n");
        return false;
    }

    return true;
}

static char *trimInput(char *text)
{
    char *start = text;
    char *end;

    while (isspace((unsigned char)*start)) {
        ++start;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';

    if (end - start >= 2 &&
        ((start[0] == '"' && end[-1] == '"') ||
         (start[0] == '\'' && end[-1] == '\''))) {
        ++start;
        --end;
        *end = '\0';
    }

    return start;
}

static void normalizeInteractiveMsysPath(char *path)
{
#ifdef _WIN32
    /* Native UCRT fopen() does not translate text read from stdin. */
    if (path[0] == '/' && isalpha((unsigned char)path[1]) &&
        path[2] == '/') {
        path[0] = (char)toupper((unsigned char)path[1]);
        path[1] = ':';
    }
#else
    (void)path;
#endif
}

static bool promptForPath(char *storage, size_t storageSize,
                          const char **path)
{
    char *trimmed;

    printf("Enter BIN file path: ");
    fflush(stdout);

    if (fgets(storage, (int)storageSize, stdin) == NULL) {
        fprintf(stderr, "Unable to read BIN path\n");
        return false;
    }

    trimmed = trimInput(storage);
    if (*trimmed == '\0') {
        fprintf(stderr, "BIN path is empty\n");
        return false;
    }

    if (trimmed != storage) {
        memmove(storage, trimmed, strlen(trimmed) + 1u);
    }
    normalizeInteractiveMsysPath(storage);
    *path = storage;
    return true;
}

static bool readImage(const char *path, Image *image)
{
    FILE *file = NULL;
    long fileSize;
    size_t bytesRead;

    memset(image, 0, sizeof(*image));

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Unable to open BIN '%s': %s\n", path,
                strerror(errno));
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "Unable to seek BIN '%s'\n", path);
        fclose(file);
        return false;
    }

    fileSize = ftell(file);
    if (fileSize <= 0) {
        fprintf(stderr, "BIN must contain at least one byte\n");
        fclose(file);
        return false;
    }

    if ((unsigned long)fileSize > FIRMWARE_IMAGE_SIZE_MAX) {
        fprintf(stderr,
                "BIN is too large: %ld bytes (maximum %u bytes)\n",
                fileSize, FIRMWARE_IMAGE_SIZE_MAX);
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Unable to rewind BIN '%s'\n", path);
        fclose(file);
        return false;
    }

    image->data = (uint8_t *)malloc((size_t)fileSize);
    if (image->data == NULL) {
        fprintf(stderr, "Unable to allocate %ld bytes for BIN\n", fileSize);
        fclose(file);
        return false;
    }

    bytesRead = fread(image->data, 1u, (size_t)fileSize, file);
    if (bytesRead != (size_t)fileSize) {
        fprintf(stderr, "Short BIN read: %zu/%ld bytes\n", bytesRead,
                fileSize);
        free(image->data);
        image->data = NULL;
        fclose(file);
        return false;
    }

    if (fclose(file) != 0) {
        fprintf(stderr, "Warning: failed to close BIN cleanly\n");
    }

    image->size = (uint32_t)fileSize;
    image->crc32 = crc32IsoHdlc(image->data, image->size);
    return true;
}

static void sleepMs(uint32_t milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000u;
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
#endif
}

static void delayUnlessDryRun(const Options *options, uint32_t milliseconds)
{
    if (!options->dryRun) {
        sleepMs(milliseconds);
    }
}

static libusb_device_handle *openCrazyflie(libusb_context *context,
                                            const Options *options)
{
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    ssize_t count;
    ssize_t i;
    unsigned int matchIndex = 0u;
    unsigned int matchCount = 0u;

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
        if (matchCount == 0u) {
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
    ssize_t i;

    if (count < 0) {
        fprintf(stderr, "Unable to enumerate USB devices: %s (%d)\n",
                libusb_error_name((int)count), (int)count);
        return -1;
    }

    for (i = 0; i < count; ++i) {
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

    (void)libusb_set_auto_detach_kernel_driver(handle, 1);

    result = libusb_claim_interface(handle, options->interfaceNumber);
    if (result != LIBUSB_SUCCESS) {
        fprintf(stderr, "Could not claim Crazyflie USB interface %d: %s (%d)\n",
                options->interfaceNumber, libusb_error_name(result), result);
        fprintf(stderr,
                "Bind interface 0 to WinUSB/libusbK and close cfclient on this USB device.\n");
        return false;
    }

#if !defined(__APPLE__)
    result = libusb_control_transfer(handle,
                                     LIBUSB_ENDPOINT_OUT |
                                         LIBUSB_REQUEST_TYPE_VENDOR |
                                         LIBUSB_RECIPIENT_DEVICE,
                                     0x01, 0, 1, NULL, 0,
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
    (void)libusb_control_transfer(handle,
                                  LIBUSB_ENDPOINT_OUT |
                                      LIBUSB_REQUEST_TYPE_VENDOR |
                                      LIBUSB_RECIPIENT_DEVICE,
                                  0x01, 0, 0, NULL, 0,
                                  options->timeoutMs);
#endif
    (void)libusb_release_interface(handle, options->interfaceNumber);
}

static bool bulkWriteExact(libusb_device_handle *handle, uint8_t *data,
                           int length, const char *description,
                           const Options *options)
{
    int transferred = 0;
    int result = libusb_bulk_transfer(handle, options->endpointOut, data,
                                      length, &transferred,
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

static bool writePayloadPackets(libusb_device_handle *handle,
                                const uint8_t *payload, size_t length,
                                const Options *options)
{
    size_t offset = 0u;

    while (offset < length) {
        size_t remaining = length - offset;
        int packetLength = (int)(remaining > CF_USB_PACKET_SIZE ?
                                 CF_USB_PACKET_SIZE : remaining);

        if (!bulkWriteExact(handle, (uint8_t *)&payload[offset],
                            packetLength, "OTA payload", options)) {
            return false;
        }
        offset += (size_t)packetLength;
    }

    return true;
}

static bool waitForLocalTxAck(libusb_device_handle *handle,
                              const Options *options)
{
    uint8_t received[CF_USB_PACKET_SIZE];
    unsigned int elapsed = 0u;
    const unsigned int pollTimeout = 100u;

    while (elapsed < LOCAL_TX_ACK_TIMEOUT_MS) {
        int transferred = 0;
        unsigned int remaining = LOCAL_TX_ACK_TIMEOUT_MS - elapsed;
        unsigned int timeout = options->timeoutMs < pollTimeout
                                   ? options->timeoutMs : pollTimeout;
        int result;

        if (timeout > remaining) {
            timeout = remaining;
        }
        result = libusb_bulk_transfer(handle, options->endpointIn, received,
                                      sizeof(received), &transferred,
                                      timeout);

        elapsed += timeout;

        if (result == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (result != LIBUSB_SUCCESS) {
            fprintf(stderr,
                    "Local TX ACK Bulk IN endpoint 0x%02X failed: %s (%d)\n",
                    options->endpointIn, libusb_error_name(result), result);
            return false;
        }

        if (options->verbose) {
            printBytes("USB RX", received, (size_t)transferred);
        }

        if (transferred == (int)SNIFFER_ACK_SIZE &&
            memcmp(received, localTxAck, SNIFFER_ACK_SIZE) == 0) {
            return true;
        }
    }

    fprintf(stderr,
            "Timed out waiting for relay UWB TX completion ACK\n");
    return false;
}

static bool sendOtaPayload(libusb_device_handle *handle,
                           const Options *options,
                           uint16_t sequence,
                           const uint8_t *payload,
                           uint16_t payloadLength)
{
    uint8_t metadata[DOWNLINK_TYPED_META_SIZE] = {0};

    if (payloadLength > OTA_MESSAGE_SIZE_MAX) {
        fprintf(stderr, "Internal error: OTA payload length %u exceeds %u\n",
                payloadLength, OTA_MESSAGE_SIZE_MAX);
        return false;
    }

    putU32Le(&metadata[0], SNIFFER_DOWNLINK_TYPED_MAGIC);
    putU16Le(&metadata[4], UWB_DEST_ANY);
    putU16Le(&metadata[6], sequence);
    putU16Le(&metadata[8], payloadLength);
    putU64Le(&metadata[10], 0u);
    metadata[18] = UWB_OTA_MESSAGE;
    metadata[19] = 0u;

    if (options->verbose) {
        printBytes("Typed downlink meta", metadata, sizeof(metadata));
    }

    if (options->dryRun) {
        return true;
    }

    if (!bulkWriteExact(handle, metadata, sizeof(metadata),
                        "Typed metadata", options)) {
        return false;
    }
    if (!writePayloadPackets(handle, payload, payloadLength, options)) {
        return false;
    }

    if (options->waitForLocalAck &&
        !waitForLocalTxAck(handle, options)) {
        return false;
    }

    return true;
}

static bool sendRepeated(libusb_device_handle *handle,
                         const Options *options,
                         uint16_t *sequence,
                         const uint8_t *message,
                         uint16_t messageLength,
                         const char *phase,
                         uint32_t chunkIndex)
{
    unsigned int repeat;

    for (repeat = 0u; repeat < PACKET_REPEAT_COUNT; ++repeat) {
        if (!sendOtaPayload(handle, options, *sequence, message,
                            messageLength)) {
            fprintf(stderr,
                    "OTA send failed: phase=%s chunk=%" PRIu32
                    " repeat=%u sequence=%u\n",
                    phase, chunkIndex, repeat + 1u, *sequence);
            return false;
        }

        ++(*sequence);
        if (repeat + 1u < PACKET_REPEAT_COUNT) {
            delayUnlessDryRun(options, PACKET_REPEAT_INTERVAL_MS);
        }
    }

    return true;
}

static bool broadcastImage(libusb_device_handle *handle,
                           const Options *options,
                           const Image *image)
{
    uint8_t message[OTA_MESSAGE_SIZE_MAX];
    uint16_t sequence = 1u;
    uint32_t totalChunks =
        (image->size + OTA_DATA_PAYLOAD_MAX - 1u) / OTA_DATA_PAYLOAD_MAX;
    const uint32_t startRoundDurationMs =
        ((PACKET_REPEAT_COUNT - 1u) * PACKET_REPEAT_INTERVAL_MS) +
        START_ROUND_INTERVAL_MS;
    const uint32_t startRounds =
        (START_ANNOUNCE_DURATION_MS + startRoundDurationMs - 1u) /
        startRoundDurationMs;
    size_t messageLength;
    uint32_t round;
    uint32_t chunkIndex;

    messageLength = buildStartMessage(message, options->imageType,
                                      image->size, image->crc32);
    printBytes("START OTA bytes", message, messageLength);
    printf("START announcement begin: %" PRIu32
           " rounds, %u repeats/round, about %u ms\n",
           startRounds, PACKET_REPEAT_COUNT,
           startRounds * startRoundDurationMs);

    for (round = 0u; round < startRounds; ++round) {
        if (!sendRepeated(handle, options, &sequence, message,
                          (uint16_t)messageLength, "START", round)) {
            return false;
        }
        delayUnlessDryRun(options, START_ROUND_INTERVAL_MS);
    }

    printf("START announcement complete\n");

    for (chunkIndex = 0u; chunkIndex < totalChunks; ++chunkIndex) {
        uint32_t offset = chunkIndex * OTA_DATA_PAYLOAD_MAX;
        uint32_t remaining = image->size - offset;
        uint16_t chunkLength =
            (uint16_t)(remaining > OTA_DATA_PAYLOAD_MAX ?
                       OTA_DATA_PAYLOAD_MAX : remaining);

        messageLength = buildDataMessage(message, options->imageType,
                                         (uint16_t)chunkIndex,
                                         &image->data[offset], chunkLength);

        if (!sendRepeated(handle, options, &sequence, message,
                          (uint16_t)messageLength, "DATA", chunkIndex)) {
            return false;
        }

        if (chunkIndex == 0u ||
            (chunkIndex + 1u) % DATA_PROGRESS_INTERVAL == 0u ||
            chunkIndex + 1u == totalChunks) {
            printf("DATA progress: %" PRIu32 "/%" PRIu32 "\n",
                   chunkIndex + 1u, totalChunks);
        }

        if (chunkIndex + 1u < totalChunks) {
            delayUnlessDryRun(options, DATA_CHUNK_INTERVAL_MS);
        }
    }

    if (totalChunks > 0u) {
        uint32_t lastIndex = totalChunks - 1u;
        uint32_t lastOffset = lastIndex * OTA_DATA_PAYLOAD_MAX;
        uint16_t lastLength = (uint16_t)(image->size - lastOffset);

        printf("Last DATA: chunk_index=%" PRIu32
               " payload_length=%u\n", lastIndex, lastLength);
    }

    messageLength = buildEndMessage(message, options->imageType);
    if (!sendRepeated(handle, options, &sequence, message,
                      (uint16_t)messageLength, "END", 0u)) {
        return false;
    }

    printf("END sent successfully (%u repeats)\n", PACKET_REPEAT_COUNT);
    return true;
}

static bool confirmReceiverReady(void)
{
    char input[32];
    char *answer;

    printf("Receivers must already be in Bootloader OTA receive mode.\n");
    printf("Type YES to start broadcasting: ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        return false;
    }

    answer = trimInput(input);
    return strcmp(answer, "YES") == 0 || strcmp(answer, "yes") == 0;
}

int main(int argc, char **argv)
{
    Options options;
    Image image = {0};
    char interactivePath[INPUT_PATH_SIZE];
    const char *path;
    libusb_context *usbContext = NULL;
    libusb_device_handle *usbHandle = NULL;
    bool interfaceClaimed = false;
    uint32_t totalChunks;
    int result = EXIT_FAILURE;
    int usbResult;

    if (!parseOptions(argc, argv, &options)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!runSelfTests()) {
        return EXIT_FAILURE;
    }

    if (options.selfTestOnly) {
        printf("SELF-TEST PASSED: CRC32=0xCBF43926 and OTA serialization vectors match\n");
        return EXIT_SUCCESS;
    }

    if (options.listDevices) {
        usbResult = libusb_init(&usbContext);
        if (usbResult != LIBUSB_SUCCESS) {
            fprintf(stderr, "libusb initialization failed: %s\n",
                    libusb_error_name(usbResult));
            goto cleanup;
        }
        result = listCrazyflies(usbContext, &options) < 0
                     ? EXIT_FAILURE : EXIT_SUCCESS;
        goto cleanup;
    }

    path = options.filePath;
    if (path == NULL &&
        !promptForPath(interactivePath, sizeof(interactivePath), &path)) {
        return EXIT_FAILURE;
    }

    if (!readImage(path, &image)) {
        goto cleanup;
    }

    totalChunks =
        (image.size + OTA_DATA_PAYLOAD_MAX - 1u) / OTA_DATA_PAYLOAD_MAX;

    printf("BIN path      : %s\n", path);
    printf("image_type    : %u (%s)\n", options.imageType,
           options.imageType == OTA_IMAGE_FIRMWARE ? "firmware" :
                                                     "bootloader");
    printf("image_size    : %" PRIu32 " bytes\n", image.size);
    printf("image_crc32   : 0x%08" PRIX32 "\n", image.crc32);
    printf("total_chunks  : %" PRIu32 "\n", totalChunks);
    printf("cfclient      : watch the target Crazyflie Console for OTA_RX logs\n");

    if (!options.assumeYes && !options.dryRun && !confirmReceiverReady()) {
        fprintf(stderr, "OTA broadcast cancelled\n");
        goto cleanup;
    }

    if (!options.dryRun) {
        usbResult = libusb_init(&usbContext);
        if (usbResult != LIBUSB_SUCCESS) {
            fprintf(stderr, "libusb initialization failed: %s\n",
                    libusb_error_name(usbResult));
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
        printf("Dry-run mode: USB is not opened and delays are skipped\n");
    }

    if (!broadcastImage(usbHandle, &options, &image)) {
        goto cleanup;
    }

    printf("OTA broadcast completed\n");
    result = EXIT_SUCCESS;

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
    free(image.data);
    return result;
}
