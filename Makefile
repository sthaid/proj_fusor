TARGETS = fusor

CC = gcc
CFLAGS = -c -g -O0 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

OBJS = main.o \
       util_sdl.o \
       util_jpeg_decode.o \
       util_dataq.o \
       util_cam.o \
       util_misc.o

XXX_LATER= \
       util_sdl_predefined_displays.o

#
# build rules
#

all: $(TARGETS)

fusor: $(OBJS) 
	$(CC) -pthread -lrt -ljpeg -lpng -lm -lSDL2 -lSDL2_ttf -lSDL2_mixer -o $@ $(OBJS)
	sudo chown root:root fusor
	sudo chmod 4777 fusor

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJS)

#
# compile rules
# XXX use makedepend
#

