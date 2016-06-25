// XXX common.h  header

#define CAM_WIDTH       640
#define CAM_HEIGHT      480
#define FRAMES_PER_SEC  5

#define ADC_CHAN_VOLTAGE           1  // XXX names
#define ADC_CHAN_CURRENT           2
#define ADC_CHAN_CHAMBER_PRESSURE  3

#define PORT 9001

#define MAX_DETECTOR_CHAN    4
#define MAX_ADC_DIAG_CHAN    4
#define MAX_ADC_DIAG_VALUE   1000

#define MAGIC_DATA  0xaabbccdd55aa55aa

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
        float    chamber_pressure_d2_mtorr;
        float    chamber_pressure_n2_mtorr;
        float    average_cpm[MAX_DETECTOR_CHAN];
        float    moving_average_cpm[MAX_DETECTOR_CHAN];

        off_t    data_part2_offset;
        uint32_t data_part2_length;
        bool     data_part2_jpeg_buff_valid;
        uint8_t  pad[3];
    } part1;
    struct data_part2_s {
        int16_t  adc_diag[MAX_ADC_DIAG_CHAN][MAX_ADC_DIAG_VALUE];
        uint32_t jpeg_buff_len;
        uint8_t  pad[4];
        uint8_t  jpeg_buff[0];
    } part2;
} data_t;

