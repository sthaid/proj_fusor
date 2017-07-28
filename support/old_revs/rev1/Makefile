TARGETS = fusor

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

SRC = main.c \
      util_sdl.c \
      util_sdl_predefined_displays.c \
      util_jpeg_decode.c \
      util_dataq.c \
      util_cam.c \
      util_misc.c
OBJ=$(SRC:.c=.o)
DEP=$(SRC:.c=.d)

#
# build rules
#

fusor: $(OBJ) 
	$(CC) -pthread -lrt -ljpeg -lpng -lm -lSDL2 -lSDL2_ttf -lSDL2_mixer -o $@ $(OBJ)
	sudo chown root:root fusor
	sudo chmod 4777 fusor

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ) $(DEP)

