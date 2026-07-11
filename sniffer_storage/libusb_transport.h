#ifndef LIBUSB_TRANSPORT_H
#define LIBUSB_TRANSPORT_H

#include "sniffer_capture.h"

#include <libusb.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    int interface_number;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    unsigned int timeout_ms;
    int bus_number;
    int device_address;
} LibusbDeviceOptions;

typedef struct {
    libusb_context *context;
    libusb_device_handle *handle;
    LibusbDeviceOptions options;
    int interface_claimed;
} LibusbTransport;

int libusb_transport_list(const LibusbDeviceOptions *options, FILE *stream);
int libusb_transport_open(LibusbTransport *transport,
                          const LibusbDeviceOptions *options,
                          int require_in, int require_out);
void libusb_transport_close(LibusbTransport *transport);
SnifferTransport libusb_transport_adapter(LibusbTransport *transport);

#endif
