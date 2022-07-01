

all:

install:
	make -C software install
	install -T scripts/mockdoor.sh /opt/portal300/mockdoor.sh -m 555

.phony: install