// XXX common.h  header

#define CAM_WIDTH   960
#define CAM_HEIGHT  720

#define ADC_CHAN_VOLTAGE           1  // XXX names
#define ADC_CHAN_CURRENT           2
#define ADC_CHAN_CHAMBER_PRESSURE  3

#define PORT 9001

#define MAX_DETECTOR_CHAN    4
#define MAX_ADC_DIAG_CHAN    4
#define MAX_ADC_DIAG_VALUE   1000

#define DATA_MAGIC  0xaabbccdd

#define IS_ERROR(x) ((int32_t)(x) >= ERROR_FIRST && (int32_t)(x) <= ERROR_LAST)
#define ERROR_FIRST                   1000000
#define ERROR_PRESSURE_SENSOR_FAULTY  1000000
#define ERROR_OVER_PRESSURE           1000001
#define ERROR_NO_VALUE                1000002
#define ERROR_LAST                    1000002
#define ERROR_TEXT(x) \
    ((int32_t)(x) == ERROR_PRESSURE_SENSOR_FAULTY ? "FAULTY" : \
     (int32_t)(x) == ERROR_OVER_PRESSURE          ? "OVPRES" : \
     (int32_t)(x) == ERROR_NO_VALUE               ? "NOVAL"    \
                                                  : "????")

typedef struct {
    uint32_t magic;
    uint32_t length;
    time_t   time;
    struct data_part1_s {
        float voltage_mean_kv;
        float voltage_min_kv;
        float voltage_max_kv;
        float current_ma;
        float chamber_pressure_d2_mtorr;
        float chamber_pressure_n2_mtorr;
        float average_cpm[MAX_DETECTOR_CHAN];
        float moving_average_cpm[MAX_DETECTOR_CHAN];
    } part1;
    struct data_part2_s {
        // XXX needs a vaid flag for each 
        int16_t  adc_diag[MAX_ADC_DIAG_CHAN][MAX_ADC_DIAG_VALUE];
        bool     jpeg_buff_valid;
        uint32_t jpeg_buff_len;
        uint8_t  jpeg_buff[0];
    } part2;
} data_t;

