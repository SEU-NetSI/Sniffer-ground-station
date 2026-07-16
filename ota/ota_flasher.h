#ifndef OTA_FLASHER_H
#define OTA_FLASHER_H

#include <stdint.h>

typedef enum {
  UWB_REVERSED_MESSAGE = 0,
  UWB_TRANSCEIVE_MESSAGE = 1,
  UWB_RANGING_MESSAGE = 2,
  UWB_FLOODING_MESSAGE = 3,
  UWB_DATA_MESSAGE = 4,
  UWB_AODV_MESSAGE = 5,
  UWB_OLSR_MESSAGE = 6,
  // PRINT = 7,
  SNIFFER = 8,
  UWB_USER_MESSAGE = 9,
  UWB_ADMIN_MESSAGE = 10,
  UWB_OTA_MESSAGE = 11,
  UWB_MESSAGE_TYPE_COUNT, /* only used for counting message types. */
} UWB_MESSAGE_TYPE;

typedef uint16_t UWB_Address_t;

#define UWB_FRAME_LEN_MAX       256
#define UWB_PACKET_SIZE_MAX     (UWB_FRAME_LEN_MAX - 2)

typedef struct {
  UWB_Address_t srcAddress; // mac address, currently using MY_UWB_ADDRESS
  UWB_Address_t destAddress; // mac address
  uint16_t seqNumber;
  struct {
	  UWB_MESSAGE_TYPE type: 6;
      uint16_t length: 10;
    } __attribute__((packed));
} __attribute__((packed)) UWB_Packet_Header_t;

#define UWB_PAYLOAD_SIZE_MAX    (UWB_PACKET_SIZE_MAX - sizeof(UWB_Packet_Header_t))
#define UWB_OTA_PAYLOAD_SIZE 224U

typedef struct {
  UWB_Packet_Header_t header; // Packet header
  uint8_t payload[UWB_PAYLOAD_SIZE_MAX];
} __attribute__((packed)) UWB_Packet_t;

//========

#define UWB_OTA_COMMAND_START 0U
#define UWB_OTA_COMMAND_DATA  1U
#define UWB_OTA_COMMAND_END   2U

#define UWB_OTA_IMAGE_BOOTLOADER 0U
#define UWB_OTA_IMAGE_FIRMWARE   1U

/* 仅供本地等待任意镜像类型使用，不能作为空口 image_type 发送。 */
#define UWB_OTA_IMAGE_NONE 0xFFU

#define UWB_OTA_PAYLOAD_SIZE 224U

typedef struct {
    uint8_t  command_type;
    uint8_t  image_type;
    uint16_t payload_length;
    uint16_t chunk_index; // meaning only for UWB_OTA_PACKET_DATA
} __attribute__((packed)) OTA_Message_Header_t;

typedef struct {
    OTA_Message_Header_t header;
    uint8_t body[UWB_OTA_PAYLOAD_SIZE];
} __attribute__((packed)) OTA_Message_t;

typedef struct {
    uint32_t image_size;
    uint32_t image_crc32;
} __attribute__((packed)) OTA_Start_Info_t;

void printMessage(UWB_Packet_t *uwbPacket);

#endif //OTA_FLASHER_H
