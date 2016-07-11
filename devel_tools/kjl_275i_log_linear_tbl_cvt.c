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

// This program converts the JKL 275i Log Linear Analog Output Pressure/Voltage 
// table (section 7.2 in the Instruction Manual) to C code.
//
// Refer to http://www.lesker.com/newweb/gauges/pdf/manuals/275iusermanual.pdf
// section 7.2.

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

char line[200][200];
int32_t max_line;
char * parse[200][200];
int32_t max_parse[200];

int main()
{
    int32_t i,g;
    char * s;
    bool last_line;

    // read the lines from stdin
    while (fgets(line[max_line],sizeof(line[0]),stdin) != NULL) {
        max_line++;
    }
    printf("max_line %d\n", max_line);

    // parse the lines
    for (i = 0; i < max_line; i++) {
        while (true) {
            s = strtok(max_parse[i] == 0 ? line[i] : NULL, " \n");
            if (!s) {
                break;
            }
            parse[i][max_parse[i]++] = s;
        }
    }

    // sanity check that every line has been parsed to the same number of entries
    for (i = 0; i < max_line; i++) {
        if (max_parse[i] != max_parse[0]) {
            printf("ERROR max_parse[%d]=%d is not equal to max_parse[0]=%d\n",
                   i, max_parse[i], max_parse[0]);
            return 1;
        }
    }

    // loop over the gases, output in the following format:
    //
    //    { "name",
    //      { { "pressure", "voltage" },
    //        { "pressure", "voltage" },
    //        { "pressure", "voltage" }, } }
    //
    // Add the 3 entries at the begining to interpolate pressure gauge
    // output voltages in the range 1 to 1 volt. According to the documentation
    // voltages below 0.01 volts means faulty sensor, which implies voltages
    // above 0.01 volts are valid.
    //
    // If a voltage in the table is 8.041 volts, this means the pressure is
    // too high to be measured. So stop generating the output when 8.041 volts
    // is encountered.
    for (g = 1; g < max_parse[0]; g++) {
        printf("    { \"%s\",\n", parse[0][g]);
        printf("      { {     0.00001,     0.000 },\n");
        printf("        {     0.00002,     0.301 },\n");
        printf("        {     0.00005,     0.699 },\n");
        for (i = 1; i < max_line; i++) {
            last_line = (i == max_line-1) ||
                        (strcmp(parse[i+1][g], "8.041") == 0);
            printf("        { %10s, %10s },%s\n", 
                   parse[i][0], parse[i][g],
                   last_line ? " } }" : "");
            if (last_line) {
                break;
            }
        }
        printf("\n");
    }
    return 0;
}
