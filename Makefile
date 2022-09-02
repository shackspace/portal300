

all:

install:
	make -C software install
	install -T scripts/mockdoor.sh /opt/portal300/mockdoor.sh -m 555
	install -T scripts/import-keys.sh /opt/portal300/import-keys.sh -m 555
	install -T scripts/trigger-key-import.sh /opt/portal300/trigger-key-import.sh -m 555
	install -T scripts/generate_authorized_keys.jq /opt/portal300/generate_authorized_keys.jq -m 555
	install -T scripts/99-usb-autoimport.rules /etc/udev/rules.d/99-usb-autoimport.rules -m 555
	install -T scripts/99-serial-naming.rules /etc/udev/rules.d/99-serial-naming.rules -m 555
	install -T scripts/portal-daemon.service /etc/systemd/system/portal-daemon.service -m 555
	install -T scripts/portal-mockdoor@.service /etc/systemd/system/portal-mockdoor@.service -m 555

	udevadm control --reload

deploy-admin:
	scp -r scripts/admin/* byro:/home/shackbureau/portal300/

.phony: install
