

all:

install:
	make -C software install
	install -T scripts/mockdoor.sh /opt/portal300/mockdoor.sh -m 555
	install -T scripts/import-keys.sh /opt/portal300/import-keys.sh -m 555
	install -T scripts/generate_authorized_keys.jq /opt/portal300/generate_authorized_keys.jq -m 555

.phony: install