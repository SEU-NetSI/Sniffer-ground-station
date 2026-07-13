#include "libusb_transport.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

static int device_matches(libusb_device *device,
                          const LibusbDeviceOptions *options,
                          struct libusb_device_descriptor *descriptor) {
    if (libusb_get_device_descriptor(device, descriptor) != LIBUSB_SUCCESS) {
        return 0;
    }
    if (descriptor->idVendor != options->vendor_id ||
        descriptor->idProduct != options->product_id) {
        return 0;
    }
    if (options->bus_number >= 0 &&
        libusb_get_bus_number(device) != (uint8_t)options->bus_number) {
        return 0;
    }
    if (options->device_address >= 0 &&
        libusb_get_device_address(device) != (uint8_t)options->device_address) {
        return 0;
    }
    return 1;
}

static void print_device(libusb_device *device,
                         const struct libusb_device_descriptor *descriptor,
                         FILE *stream) {
    fprintf(stream, "bus=%u address=%u vid=0x%04" PRIX16 " pid=0x%04" PRIX16 "\n",
            libusb_get_bus_number(device), libusb_get_device_address(device),
            descriptor->idVendor, descriptor->idProduct);
}

static void print_interface_endpoints(libusb_device *device, int interface_number,
                                      FILE *stream) {
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_config_descriptor(device, 0, &config) != LIBUSB_SUCCESS) {
        fprintf(stream, "  interface=%d endpoints=unavailable\n", interface_number);
        return;
    }
    for (uint8_t index = 0; index < config->bNumInterfaces; index++) {
        const struct libusb_interface *interface = &config->interface[index];
        for (int alt = 0; alt < interface->num_altsetting; alt++) {
            const struct libusb_interface_descriptor *setting = &interface->altsetting[alt];
            if (setting->bInterfaceNumber != interface_number ||
                setting->bAlternateSetting != 0) {
                continue;
            }
            fprintf(stream, "  interface=%u alt=%u endpoints=",
                    setting->bInterfaceNumber, setting->bAlternateSetting);
            for (uint8_t ep = 0; ep < setting->bNumEndpoints; ep++) {
                const struct libusb_endpoint_descriptor *candidate = &setting->endpoint[ep];
                const char *type =
                    (candidate->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                            LIBUSB_TRANSFER_TYPE_BULK
                        ? "bulk"
                        : "other";
                fprintf(stream, "%s0x%02X/%s/max%u",
                        ep == 0 ? "" : ",",
                        candidate->bEndpointAddress, type,
                        candidate->wMaxPacketSize);
            }
            fputc('\n', stream);
        }
    }
    libusb_free_config_descriptor(config);
}

int libusb_transport_list(const LibusbDeviceOptions *options, FILE *stream) {
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    if (libusb_init(&context) != LIBUSB_SUCCESS) {
        return -1;
    }
    ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        libusb_exit(context);
        return -1;
    }
    int matches = 0;
    for (ssize_t index = 0; index < count; index++) {
        struct libusb_device_descriptor descriptor;
        if (device_matches(devices[index], options, &descriptor)) {
            print_device(devices[index], &descriptor, stream);
            print_interface_endpoints(devices[index], options->interface_number,
                                      stream);
            matches++;
        }
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return matches;
}

static int endpoint_is_bulk(libusb_device_handle *handle, int interface_number,
                            uint8_t endpoint) {
    struct libusb_config_descriptor *config = NULL;
    libusb_device *device = libusb_get_device(handle);
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) {
        return 0;
    }
    int found = 0;
    for (uint8_t index = 0; index < config->bNumInterfaces && !found; index++) {
        const struct libusb_interface *interface = &config->interface[index];
        for (int alt = 0; alt < interface->num_altsetting && !found; alt++) {
            const struct libusb_interface_descriptor *setting = &interface->altsetting[alt];
            if (setting->bInterfaceNumber != interface_number ||
                setting->bAlternateSetting != 0) {
                continue;
            }
            for (uint8_t ep = 0; ep < setting->bNumEndpoints; ep++) {
                const struct libusb_endpoint_descriptor *candidate = &setting->endpoint[ep];
                if (candidate->bEndpointAddress == endpoint &&
                    (candidate->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                        LIBUSB_TRANSFER_TYPE_BULK) {
                    found = 1;
                    break;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

int libusb_transport_open(LibusbTransport *transport,
                          const LibusbDeviceOptions *options,
                          int require_in, int require_out) {
    memset(transport, 0, sizeof(*transport));
    transport->options = *options;
    int response = libusb_init(&transport->context);
    if (response != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_strerror(response));
        return -1;
    }

    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(transport->context, &devices);
    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list failed: %s\n",
                libusb_strerror((int)count));
        libusb_transport_close(transport);
        return -1;
    }
    libusb_device *selected = NULL;
    size_t match_count = 0;
    for (ssize_t index = 0; index < count; index++) {
        struct libusb_device_descriptor descriptor;
        if (device_matches(devices[index], options, &descriptor)) {
            selected = devices[index];
            match_count++;
        }
    }
    if (options->bus_number < 0 && options->device_address < 0 && match_count > 1) {
        fprintf(stderr,
                "Multiple matching USB devices found; use --list-devices and "
                "--device BUS:ADDRESS\n");
        libusb_free_device_list(devices, 1);
        libusb_transport_close(transport);
        return -1;
    }
    if (selected != NULL) {
        response = libusb_open(selected, &transport->handle);
        if (response == LIBUSB_SUCCESS) {
            transport->options.bus_number = libusb_get_bus_number(selected);
            transport->options.device_address = libusb_get_device_address(selected);
        } else {
            fprintf(stderr, "libusb_open failed: %s\n", libusb_strerror(response));
        }
    }
    libusb_free_device_list(devices, 1);
    if (transport->handle == NULL) {
        fprintf(stderr, "No matching USB device found\n");
        libusb_transport_close(transport);
        return -1;
    }

    response = libusb_set_auto_detach_kernel_driver(transport->handle, 1);
    if (response != LIBUSB_SUCCESS && response != LIBUSB_ERROR_NOT_SUPPORTED) {
        fprintf(stderr, "auto-detach warning: %s\n", libusb_strerror(response));
    }
    response = libusb_claim_interface(transport->handle, options->interface_number);
    if (response != LIBUSB_SUCCESS) {
        fprintf(stderr, "claim interface %d failed: %s\n",
                options->interface_number, libusb_strerror(response));
        libusb_transport_close(transport);
        return -1;
    }
    transport->interface_claimed = 1;

    if ((require_in && !endpoint_is_bulk(transport->handle,
                                         options->interface_number,
                                         options->endpoint_in)) ||
        (require_out && !endpoint_is_bulk(transport->handle,
                                          options->interface_number,
                                          options->endpoint_out))) {
        fprintf(stderr, "Required bulk endpoint is absent from interface %d\n",
                options->interface_number);
        libusb_transport_close(transport);
        return -1;
    }
    if (require_in) {
        response = libusb_clear_halt(transport->handle, options->endpoint_in);
        if (response != LIBUSB_SUCCESS) {
            fprintf(stderr, "clear halt 0x%02X warning: %s\n",
                    options->endpoint_in, libusb_strerror(response));
        }
    }
    return 0;
}

void libusb_transport_close(LibusbTransport *transport) {
    if (transport->interface_claimed && transport->handle != NULL) {
        (void)libusb_release_interface(transport->handle,
                                       transport->options.interface_number);
    }
    if (transport->handle != NULL) {
        libusb_close(transport->handle);
    }
    if (transport->context != NULL) {
        libusb_exit(transport->context);
    }
    memset(transport, 0, sizeof(*transport));
}

static SnifferIoStatus usb_read(void *context, uint8_t *buffer,
                                size_t capacity, size_t *received) {
    LibusbTransport *transport = context;
    int transferred = 0;
    int request = capacity > INT_MAX ? INT_MAX : (int)capacity;
    int response = libusb_bulk_transfer(transport->handle,
                                        transport->options.endpoint_in,
                                        buffer, request, &transferred,
                                        transport->options.timeout_ms);
    *received = transferred > 0 ? (size_t)transferred : 0;
    if (response == LIBUSB_ERROR_TIMEOUT) {
        return SNIFFER_IO_TIMEOUT;
    }
    if (response == LIBUSB_ERROR_INTERRUPTED) {
        return SNIFFER_IO_INTERRUPTED;
    }
    if (response == LIBUSB_ERROR_NO_DEVICE) {
        fprintf(stderr, "USB device disconnected while reading endpoint 0x%02X\n",
                transport->options.endpoint_in);
        return SNIFFER_IO_ERROR;
    }
    if (response != LIBUSB_SUCCESS) {
        fprintf(stderr, "Bulk IN 0x%02X failed: %s\n",
                transport->options.endpoint_in, libusb_strerror(response));
        return SNIFFER_IO_ERROR;
    }
    return *received > 0 ? SNIFFER_IO_DATA : SNIFFER_IO_TIMEOUT;
}

static SnifferIoStatus usb_write(void *context, const uint8_t *buffer,
                                 size_t length, size_t *written) {
    LibusbTransport *transport = context;
    int transferred = 0;
    int request = length > INT_MAX ? INT_MAX : (int)length;
    int response = libusb_bulk_transfer(transport->handle,
                                        transport->options.endpoint_out,
                                        (unsigned char *)(uintptr_t)buffer,
                                        request, &transferred,
                                        transport->options.timeout_ms);
    *written = transferred > 0 ? (size_t)transferred : 0;
    if (response == LIBUSB_ERROR_TIMEOUT) {
        return SNIFFER_IO_TIMEOUT;
    }
    if (response == LIBUSB_ERROR_INTERRUPTED) {
        return SNIFFER_IO_INTERRUPTED;
    }
    if (response == LIBUSB_ERROR_NO_DEVICE) {
        fprintf(stderr, "USB device disconnected while writing endpoint 0x%02X\n",
                transport->options.endpoint_out);
        return SNIFFER_IO_ERROR;
    }
    if (response != LIBUSB_SUCCESS) {
        fprintf(stderr, "Bulk OUT 0x%02X failed: %s\n",
                transport->options.endpoint_out, libusb_strerror(response));
        return SNIFFER_IO_ERROR;
    }
    return *written > 0 ? SNIFFER_IO_DATA : SNIFFER_IO_ERROR;
}

SnifferTransport libusb_transport_adapter(LibusbTransport *transport) {
    SnifferTransport adapter = {
        .context = transport,
        .read = usb_read,
        .write = usb_write,
    };
    return adapter;
}
