TARGETS = get_data display

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

SRC_GET_DATA = get_data.c \
               util_dataq.c \
               util_cam.c \
               util_misc.c
OBJ_GET_DATA=$(SRC_GET_DATA:.c=.o)

SRC_DISPLAY = display.c \
              util_sdl.c \
              util_sdl_predefined_displays.c \
              util_jpeg_decode.c \
              util_misc.c
OBJ_DISPLAY=$(SRC_DISPLAY:.c=.o)

SRC_TEST = test.c \
           util_mccdaq.c \
           util_misc.c 
OBJ_TEST=$(SRC_TEST:.c=.o)

DEP=$(SRC_GET_DATA:.c=.d) $(SRC_DISPLAY:.c=.d) $(SRC_TEST:.c=.d)

#
# build rules
#

test: $(OBJ_TEST) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_TEST) \
            -L/usr/local/lib -lmccusb -lhidapi-libusb -lusb-1.0
	sudo chown root:root $@
	sudo chmod 4777 $@

all: $(TARGETS)

get_data: $(OBJ_GET_DATA) 
	$(CC) -pthread -lrt -lm -o $@ $(OBJ_GET_DATA) \
            -L/usr/local/lib -lmccusb -lhidapi-libusb -lusb-1.0
	sudo chown root:root $@
	sudo chmod 4777 $@

display: $(OBJ_DISPLAY) 
	$(CC) -pthread -lrt -lm -lpng -ljpeg -lSDL2 -lSDL2_ttf -lSDL2_mixer -o $@ $(OBJ_DISPLAY)
	sudo chown root:root $@
	sudo chmod 4777 $@

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ_GET_DATA) $(OBJ_DISPLAY) $(OBJ_TEST) $(DEP)

