void dataq_init(double averaging_duration_sec);  // XXX args, such as channel list

int32_t dataq_get_adc(int32_t adc_idx, double * v_mean, double * v_min, double * v_max, double * v_rms);


