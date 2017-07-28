/*
Copyright (c) 2016 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef __COMMON_H__
#define __COMMON_H__

#define CAM_WIDTH       640
#define CAM_HEIGHT      480
#define FRAMES_PER_SEC  5

#define DATAQ_ADC_CHAN_VOLTAGE   1
#define DATAQ_ADC_CHAN_CURRENT   2
#define DATAQ_ADC_CHAN_PRESSURE  3

#define MAX_ADC_DATA    1200

#define IS_ERROR(x) ((int32_t)(x) >= ERROR_FIRST && (int32_t)(x) <= ERROR_LAST)
#define ERROR_FIRST                   1000000 
#define ERROR_PRESSURE_SENSOR_FAULTY  1000000
#define ERROR_OVER_PRESSURE           1000001
#define ERROR_NO_VALUE                1000002
#define ERROR_LAST                    1000002
#define ERROR_TEXT(x) \
    ((int32_t)(x) == ERROR_PRESSURE_SENSOR_FAULTY ? "FAULTY" : \
     (int32_t)(x) == ERROR_OVER_PRESSURE          ? "OVERPR" : \
     (int32_t)(x) == ERROR_NO_VALUE               ? "NOVAL "   \
                                                  : "??????")

#define PORT 9001

#define MAGIC_DATA_PART1  0xaabbccdd55aa55aa
#define MAGIC_DATA_PART2  0x77777777aaaaaaaa

// data_part1_s and data_part2_s are each padded to 8 byte boundary
typedef struct {
    struct data_part1_s {
        uint64_t magic;
        uint64_t time; 

        float    voltage_kv;      // mean voltage
        float    current_ma;
        float    d2_pressure_mtorr;
        float    n2_pressure_mtorr;
        float    neutron_cps;
        float    reserved[5];

        off_t    data_part2_offset;      // for use by display pgm
        uint32_t data_part2_length;
        bool     data_part2_jpeg_buff_valid;
        bool     data_part2_voltage_adc_data_valid;
        bool     data_part2_current_adc_data_valid;
        bool     data_part2_pressure_adc_data_valid;
        bool     data_part2_neutron_adc_data_valid;
        uint8_t  pad[7];
    } part1;
    struct data_part2_s {
        uint64_t magic;
        int16_t  voltage_adc_data[MAX_ADC_DATA];    // mv, 1200/sec
        int16_t  current_adc_data[MAX_ADC_DATA];    // mv, 1200/sec
        int16_t  pressure_adc_data[MAX_ADC_DATA];   // mv, 1200/sec
        int16_t  neutron_adc_data[MAX_ADC_DATA];    // mv, stores multiple pulses, 50 samples each
        uint32_t max_neutron_adc_data;
        uint32_t jpeg_buff_len;
        uint8_t  jpeg_buff[0];
    } part2;
} data_t;

#endif
