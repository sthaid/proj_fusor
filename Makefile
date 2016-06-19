TARGETS = get_data

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

SRC_GET_DATA = get_data.c \
               util_dataq.c \
               util_cam.c \
               util_misc.c
OBJ_GET_DATA=$(SRC_GET_DATA:.c=.o)

DEP=$(SRC_GET_DATA:.c=.d)

#
# build rules
#

get_data: $(OBJ_GET_DATA) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_GET_DATA)

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ_GET_DATA) $(DEP)

