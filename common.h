// XXX common.h  header

#define CAM_WIDTH       640
#define CAM_HEIGHT      480
#define FRAMES_PER_SEC  5

#define ADC_CHAN_VOLTAGE   1  // XXX names
#define ADC_CHAN_CURRENT   2
#define ADC_CHAN_PRESSURE  3

#define PORT 9001

#define MAX_DETECTOR_CHAN    4
#define MAX_ADC_SAMPLES      1200

#define MAGIC_DATA_PART1  0xaabbccdd55aa55aa
#define MAGIC_DATA_PART2  0x77777777aaaaaaaa

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

// YYY check size on linux and rpi
// YYY need valid for diag too
// data_part1_s and data_part2_s are each padded to 8 byte boundary
typedef struct {
    struct data_part1_s {
        uint64_t magic;
        uint64_t time; 

        float    voltage_mean_kv;
        float    voltage_min_kv;
        float    voltage_max_kv;
        float    current_ma;
        float    pressure_d2_mtorr;
        float    pressure_n2_mtorr;
        float    average_cpm[MAX_DETECTOR_CHAN];
        float    moving_average_cpm[MAX_DETECTOR_CHAN];

        off_t    data_part2_offset;   // for display pgm
        uint32_t data_part2_length;
        bool     data_part2_jpeg_buff_valid;
        bool     data_part2_voltage_adc_samples_mv_valid;
        bool     data_part2_current_adc_samples_mv_valid;
        bool     data_part2_pressure_adc_samples_mv_valid;
    } part1;
    struct data_part2_s {
        uint64_t magic;
        int16_t  voltage_adc_samples_mv[MAX_ADC_SAMPLES];
        int16_t  current_adc_samples_mv[MAX_ADC_SAMPLES];
        int16_t  pressure_adc_samples_mv[MAX_ADC_SAMPLES];
        uint32_t jpeg_buff_len;
        uint8_t  pad[4];
        uint8_t  jpeg_buff[0];
    } part2;
} data_t;

