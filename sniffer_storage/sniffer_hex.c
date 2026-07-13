#include "sniffer_hex.h"

#include <ctype.h>
#include <stdio.h>

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (unsigned char)tolower(value);
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

int sniffer_parse_hex(const char *text, uint8_t *output, size_t capacity,
                      size_t *output_length) {
    if (text == NULL || output == NULL || output_length == NULL) {
        return -1;
    }
    size_t count = 0;
    int high = -1;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; cursor++) {
        if (isspace(*cursor) || *cursor == ':' || *cursor == '-' || *cursor == ',') {
            continue;
        }
        if (*cursor == 'x' || *cursor == 'X') {
            if (high == 0) {
                high = -1;
                continue;
            }
            return -1;
        }
        int value = hex_value(*cursor);
        if (value < 0) {
            return -1;
        }
        if (high < 0) {
            high = value;
        } else {
            if (count >= capacity) {
                return -1;
            }
            output[count++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    if (high >= 0 || count == 0) {
        return -1;
    }
    *output_length = count;
    return 0;
}

int sniffer_format_hex(const uint8_t *data, size_t length, char *output,
                       size_t capacity) {
    if ((length > 0 && data == NULL) || output == NULL ||
        capacity < length * 2 + 1) {
        return -1;
    }
    for (size_t index = 0; index < length; index++) {
        (void)snprintf(output + index * 2, 3, "%02X", data[index]);
    }
    output[length * 2] = '\0';
    return 0;
}
