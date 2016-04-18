#ifndef __UTIL_DATAQ_H__
#define __UTIL_DATAQ_H__

#define VOLTS(x) ((float)(x) / 2048)

typedef struct {
    int16_t rms;
    int16_t mean;
    int16_t min;
    int16_t max;
} adc_value_t;

void dataq_init(float averaging_duration_sec, int32_t * adc_chan_list, int32_t max_adc_chan_list);

int32_t dataq_get_adc(int32_t adc_chan, adc_value_t * adc_value);

#endif
