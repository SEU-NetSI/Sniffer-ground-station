#ifndef SNIFFER_HEX_H
#define SNIFFER_HEX_H

#include <stddef.h>
#include <stdint.h>

int sniffer_parse_hex(const char *text, uint8_t *output, size_t capacity,
                      size_t *output_length);
int sniffer_format_hex(const uint8_t *data, size_t length, char *output,
                       size_t capacity);

#endif
