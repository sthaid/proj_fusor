void dataq_init(double averaging_duration_sec, int32_t num_adc_chan, ...);

int32_t dataq_get_adc(int32_t adc_chan, double * v_mean, double * v_min, double * v_max, double * v_rms);


