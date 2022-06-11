CC=gcc
LD=gcc
AR=ar

CFLAGS=-DMQTTC_PAL_FILE=portal_mqtt_pal.h -Isrc -g
CFLAGS_LIB=$(CFLAGS)
CFLAGS_APP=$(CFLAGS) -pedantic -Wall -Wextra -Werror=missing-prototypes -Werror=strict-prototypes -Werror=format -Werror=shadow -Werror=return-type -Werror=unused-parameter -I vendor/mqtt-c/include -I vendor/c-periphery/src 
LFLAGS=

DAEMON_LIBS=ssl crypto pthread
TRIGGER_LIBS=

all: bin/portal-daemon bin/portal-trigger

bin/portal-daemon: obj/portal-daemon.o obj/mqtt-client.o obj/ipc.o obj/periphery.a obj/mqtt.a obj/state-machine.o
	$(LD) $(LFLAGS) -o "$@" $^ $(addprefix -l ,$(DAEMON_LIBS))

bin/portal-trigger: obj/portal-trigger.o obj/ipc.o
	$(LD) $(LFLAGS) -o "$@" $^ $(addprefix -l ,$(TRIGGER_LIBS))

# application object files
obj/%.o: src/%.c
	$(CC) $(CFLAGS_APP) -c -o "$@" $<

# c-periphery library
obj/periphery.a:  obj/periphery-gpio.o obj/periphery-led.o obj/periphery-pwm.o obj/periphery-spi.o obj/periphery-i2c.o obj/periphery-mmio.o obj/periphery-serial.o obj/periphery-version.o
	$(AR) rcs "$@" $^

# objects for c-periphery
obj/periphery-%.o: vendor/c-periphery/src/%.c
	$(CC) $(CFLAGS_LIB) -c -o "$@" $< -DPERIPHERY_GPIO_CDEV_SUPPORT

# mqtt-c library
obj/mqtt.a: obj/mqtt-mqtt.o obj/mqtt-mqtt_pal.o
	$(AR) rcs "$@" $^

# objects for mqtt-c
obj/mqtt-%.o: vendor/mqtt-c/src/%.c
	$(CC) $(CFLAGS_LIB) -c -I vendor/mqtt-c/include -o "$@" $<

clean:
	rm -f obj/*.o obj/*.a

.PHONY: clean
.SUFFIXES: 