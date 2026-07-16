#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ota_flasher.h"

static uint32_t crc32_file(FILE *file) {
    static uint32_t table[256];
    static int initialized;
    uint8_t buffer[64U * 1024U];
    uint32_t crc = UINT32_MAX;
    size_t bytes_read;

    if (!initialized) {
        for (uint32_t i = 0; i < 256U; ++i) {
            uint32_t value = i;
            for (unsigned int bit = 0; bit < 8U; ++bit) {
                value = (value >> 1U) ^
                        (0xEDB88320U & (uint32_t)-(int32_t)(value & 1U));
            }
            table[i] = value;
        }
        initialized = 1;
    }

    while ((bytes_read = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
        for (size_t i = 0; i < bytes_read; ++i) {
            crc = table[(crc ^ buffer[i]) & 0xFFU] ^ (crc >> 8U);
        }
    }
    return crc ^ UINT32_MAX;
}

int main(int argc, char **argv) {
    FILE *file;
    long file_length;
    OTA_Start_Info_t start_info;
    OTA_Message_t ota_message = {0};
    UWB_Packet_t uwb_packet = {0};
	uwb_packet.header.type = UWB_OTA_MESSAGE;
	uwb_packet.header.destAddress = 0xFFFFU;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image-file>\n", argv[0]);
        return 1;
    }

    file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (file_length = ftell(file)) < 0L ||
        (uintmax_t)file_length > UINT32_MAX ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "Unable to determine a valid image size for %s\n", argv[1]);
        fclose(file);
        return 1;
    }

    start_info.image_size = (uint32_t)file_length;
    start_info.image_crc32 = crc32_file(file);
    if (ferror(file)) {
        perror(argv[1]);
        fclose(file);
        return 1;
    }
    fclose(file);

    ota_message.header.command_type = UWB_OTA_COMMAND_START;
    ota_message.header.image_type = UWB_OTA_IMAGE_FIRMWARE;
    ota_message.header.payload_length = sizeof(start_info);
    memcpy(ota_message.body, &start_info, sizeof(start_info));

    uwb_packet.header.length = sizeof(uwb_packet.header) +
                               sizeof(ota_message.header) + sizeof(start_info);
    memcpy(uwb_packet.payload, &ota_message,
           sizeof(ota_message.header) + sizeof(start_info));
    printMessage(&uwb_packet);

    file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror(argv[1]);
        return 1;
    }

    ota_message.header.command_type = UWB_OTA_COMMAND_DATA;
    ota_message.header.image_type = UWB_OTA_IMAGE_FIRMWARE;
    for (uint32_t chunk_index = 0U;; ++chunk_index) {
        size_t bytes_read = fread(ota_message.body, 1U,
                                  sizeof(ota_message.body), file);
        if (bytes_read == 0U) {
            break;
        }

        ota_message.header.payload_length = (uint16_t)bytes_read;
        ota_message.header.chunk_index = (uint16_t)chunk_index;
        uwb_packet.header.length = sizeof(uwb_packet.header) +
                                   sizeof(ota_message.header) + bytes_read;
        memcpy(uwb_packet.payload, &ota_message,
               sizeof(ota_message.header) + bytes_read);
        printMessage(&uwb_packet);
    }

    if (ferror(file)) {
        perror(argv[1]);
        fclose(file);
        return 1;
    }
    fclose(file);

    ota_message.header.command_type = UWB_OTA_COMMAND_END;
    ota_message.header.image_type = UWB_OTA_IMAGE_FIRMWARE;
    ota_message.header.payload_length = 0U;
    ota_message.header.chunk_index = 0U;
    uwb_packet.header.length = sizeof(uwb_packet.header) +
                               sizeof(ota_message.header);
    memcpy(uwb_packet.payload, &ota_message, sizeof(ota_message.header));
    printMessage(&uwb_packet);
    return 0;
}

void printMessage(UWB_Packet_t *uwbPacket) {
    size_t uwbPayloadLength;
    size_t availableBodyLength;
    size_t bodyLength;

    if (uwbPacket == NULL) {
        fprintf(stderr, "UWB packet: (null)\n");
        return;
    }

    printf("UWB packet header:\n");
    printf("  srcAddress:  %u\n", (unsigned int)uwbPacket->header.srcAddress);
    printf("  destAddress: %u\n", (unsigned int)uwbPacket->header.destAddress);
    printf("  seqNumber:   %u\n", (unsigned int)uwbPacket->header.seqNumber);
    printf("  type:        %u\n", (unsigned int)uwbPacket->header.type);
    printf("  length:      %u\n", (unsigned int)uwbPacket->header.length);

    if (uwbPacket->header.length < sizeof(uwbPacket->header)) {
        printf("Invalid packet: length is smaller than the UWB header (%zu bytes).\n",
               sizeof(uwbPacket->header));
        return;
    }

    uwbPayloadLength =
        (size_t)uwbPacket->header.length - sizeof(uwbPacket->header);
    if (uwbPayloadLength > sizeof(uwbPacket->payload)) {
        printf("Warning: UWB payload length %zu exceeds capacity %zu; truncating.\n",
               uwbPayloadLength, sizeof(uwbPacket->payload));
        uwbPayloadLength = sizeof(uwbPacket->payload);
    }

    if (uwbPayloadLength < sizeof(OTA_Message_Header_t)) {
        printf("Invalid OTA message: only %zu payload bytes, header needs %zu.\n",
               uwbPayloadLength, sizeof(OTA_Message_Header_t));
        return;
    }

    const OTA_Message_t *otaMessage =
        (const OTA_Message_t *)(const void *)uwbPacket->payload;

    printf("OTA message header:\n");
    printf("  command_type:  %u\n",
           (unsigned int)otaMessage->header.command_type);
    printf("  image_type:    %u\n",
           (unsigned int)otaMessage->header.image_type);
    printf("  payload_length: %u\n",
           (unsigned int)otaMessage->header.payload_length);
    printf("  chunk_index:   %u\n",
           (unsigned int)otaMessage->header.chunk_index);

    availableBodyLength = uwbPayloadLength - sizeof(OTA_Message_Header_t);
    bodyLength = otaMessage->header.payload_length;

    if (bodyLength > sizeof(otaMessage->body)) {
        printf("Warning: OTA payload length %zu exceeds capacity %zu; truncating.\n",
               bodyLength, sizeof(otaMessage->body));
        bodyLength = sizeof(otaMessage->body);
    }
    if (bodyLength > availableBodyLength) {
        printf("Warning: OTA payload declares %u bytes but only %zu are available; "
               "truncating.\n",
               (unsigned int)otaMessage->header.payload_length,
               availableBodyLength);
        bodyLength = availableBodyLength;
    }

    printf("OTA message body (%zu bytes):", bodyLength);
    if (bodyLength == 0U) {
        printf(" <empty>\n");
        return;
    }

    for (size_t i = 0; i < bodyLength; ++i) {
        if (i % 16U == 0U) {
            printf("\n  %04zx:", i);
        }
        printf(" %02x", (unsigned int)otaMessage->body[i]);
    }
    putchar('\n');
	if (otaMessage->header.command_type == UWB_OTA_COMMAND_START) {
		OTA_Start_Info_t *startInfo = (OTA_Start_Info_t *)otaMessage->body;
		printf("OTA start info:\n");
		printf("  image_size: %u\n", (unsigned int)startInfo->image_size);
		printf("  image_crc32: %u\n", (unsigned int)startInfo->image_crc32);
	}
}
