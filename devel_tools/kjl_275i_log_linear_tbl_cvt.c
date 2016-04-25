// XXX add the extras to the begining and take off the end

// this program ...

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
    for (g = 1; g < max_parse[0]; g++) {
        printf("    { \"%s\",\n", parse[0][g]);
        for (i = 1; i < max_line; i++) {
            printf("      %s { %10s, %10s },%s\n", 
                   i == 1 ? "{" : " ",
                   parse[i][0], parse[i][g],
                   i == max_line-1 ? " } }" : "");
        }
        printf("\n");
    }
    return 0;
}

/*
gas_t gas_Ne       = { "Ne",
                       { {     0.0001,         Ne },
    {     0.0002,         Ne },

typedef struct {
    char * name;
    struct {
        float pressure;
        float voltage;
    } interp_tbl[50];
} gas_t;


*/
