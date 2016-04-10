#define JPEG_DECODE_MODE_GS    1
#define JPEG_DECODE_MODE_YUY2  2

int jpeg_decode(uint32_t cxid, uint32_t jpeg_decode_mode, uint8_t * jpeg, uint32_t jpeg_size,
                uint8_t ** out_buf, uint32_t * width, uint32_t * height);

