CC=gcc
LD=gcc
AR=ar

CFLAGS=-DMQTTC_PAL_FILE=portal_mqtt_pal.h -Isrc -g -I../firmware/common/portal300/include
CFLAGS_LIB=$(CFLAGS)
CFLAGS_APP=$(CFLAGS) -pedantic -Wall -Wextra -Werror=missing-prototypes -Werror=strict-prototypes -Werror=format -Werror=shadow -Werror=return-type -Werror=unused-parameter -I vendor/mqtt-c/include -I vendor/c-periphery/src 
LFLAGS=

DAEMON_LIBS=ssl crypto pthread
TRIGGER_LIBS=

all: bin/portal-daemon bin/portal-trigger

install: bin/portal-daemon bin/portal-trigger
	mkdir -p /opt/portal300/
	install -T bin/portal-daemon /opt/portal300/portal-daemon -m 555
	install -T bin/portal-trigger /opt/portal300/portal-trigger -m 555

bin/portal-daemon: obj/portal-daemon.o obj/mqtt-client.o obj/ipc.o obj/mqtt-mqtt.o obj/log.o obj/state-machine.o
	$(LD) $(LFLAGS) -o "$@" $^ $(addprefix -l ,$(DAEMON_LIBS))

bin/portal-trigger: obj/portal-trigger.o obj/ipc.o obj/log.o
	$(LD) $(LFLAGS) -o "$@" $^ $(addprefix -l ,$(TRIGGER_LIBS))

# application object files
obj/%.o: src/%.c
	$(CC) $(CFLAGS_APP) -c -o "$@" $<

# objects for mqtt-c
obj/mqtt-%.o: vendor/mqtt-c/src/%.c
	$(CC) $(CFLAGS_LIB) -c -I vendor/mqtt-c/include -o "$@" $<

clean:
	rm -f obj/*.o obj/*.a

.PHONY: clean
.SUFFIXES: 