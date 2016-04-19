#ifndef __UTIL_DATAQ_H__
#define __UTIL_DATAQ_H__

void dataq_init(float averaging_duration_sec, int32_t max_adc_chan, ...);

int32_t dataq_get_adc(int32_t adc_chan, float * rms, float * mean, float * sdev, float * min, float * max);

#endif
