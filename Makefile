
CC=gcc
LD=gcc
AR=ar

CFLAGS=-static 
CFLAGS_LIB=$(CFLAGS)
CFLAGS_APP=$(CFLAGS) -Wall -Wextra -Werror=return-type -Werror=unused-parameter -I vendor/mqtt-c/include -I vendor/c-periphery/src
LFLAGS=-static

DAEMON_LIBS=
TRIGGER_LIBS=

all: bin/portal-daemon bin/portal-trigger

bin/portal-daemon: obj/portal-daemon.o obj/periphery.a obj/mqtt.a
	$(LD) $(LFLAGS) $(addprefix -l ,$(DAEMON_LIBS)) -o "$@" $^

bin/portal-trigger: obj/portal-trigger.o
	$(LD) $(LFLAGS) $(addprefix -l ,$(TRIGGER_LIBS)) -o "$@" $^

# application object files
obj/%.o: src/%.c
	$(CC) $(CFLAGS_APP) -c -o "$@" $<

# c-periphery library
obj/periphery.a:  obj/periphery-gpio.o obj/periphery-led.o obj/periphery-pwm.o obj/periphery-spi.o obj/periphery-i2c.o obj/periphery-mmio.o obj/periphery-serial.o obj/periphery-version.o
	$(AR) rcs "$@" $^

# objects for c-periphery
obj/periphery-%.o: vendor/c-periphery/src/%.c
	$(CC) $(CFLAGS_LIB) -c -o "$@" $<

# mqtt-c library
obj/mqtt.a: obj/mqtt-mqtt.o obj/mqtt-mqtt_pal.o
	$(AR) rcs "$@" $^

# objects for mqtt-c
obj/mqtt-%.o: vendor/mqtt-c/src/%.c
	$(CC) $(CFLAGS_LIB) -c -I vendor/mqtt-c/include -DMQTT_USE_BIO -o "$@" $<

clean:
	rm -f obj/*.o obj/*.a

.PHONY: clean
.SUFFIXES: 