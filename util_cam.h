#ifndef __UTIL_CAM_H__
#define __UTIL_CAM_H__

void cam_init(void);

void cam_get_buff(uint8_t **buff, uint32_t *len, uint32_t * buff_id);

void cam_put_buff(uint32_t buff_id);

#endif
