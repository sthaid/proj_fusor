#ifndef __UTIL_CAM_H__
#define __UTIL_CAM_H__

void cam_init(int32_t width, int32_t height);

void cam_get_buff(uint8_t **buff, uint32_t *len, uint32_t * buff_id);

void cam_put_buff(uint32_t buff_id);

#endif
