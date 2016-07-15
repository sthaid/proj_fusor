TARGETS = test

CC = gcc
OUTPUT_OPTION=-MMD -MP -o $@
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall \
         $(shell sdl2-config --cflags) 

SRC_TEST = test.c \
           util_mccdaq.c \
           util_misc.c 
OBJ_TEST=$(SRC_TEST:.c=.o)

DEP=$(SRC_TEST:.c=.d)

#
# build rules
#

test: $(OBJ_TEST) 
	$(CC) -pthread -lrt -lm -lreadline -o $@ $(OBJ_TEST) \
            -L/usr/local/lib -lmccusb -lhidapi-libusb -lusb-1.0
	sudo chown root:root $@
	sudo chmod 4777 $@

-include $(DEP)

#
# clean rule
#

clean:
	rm -f $(TARGETS) $(OBJ_TEST) $(DEP)

