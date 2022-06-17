all:
	@echo "This makefile provides the following targets:"
	@echo ""
	@echo "- update:                Updates all key header files"
	@echo "- regenerate:            Generates new client certificates for all projects."
	@echo "                         Pass path to CA via Make variable 'CA'"
	@echo "                         WARNING: THIS CREATES NEW IDENTITIES!"
	@echo "- firmware:              Builds both firmwares."
	@echo "- flash-busch-interface: Flashes the busch-interface firmware to the Make variable PORT"
	@echo "- flash-door-control:    Flashes the door-control firmware to the Make variable PORT"

update:
	./make-cheader.py \
		busch-interface/main/certs/client.crt \
		door-control/main/certs/client.crt \
		busch-interface/main/certs/client.key \
		door-control/main/certs/client.key \
		busch-interface/main/certs/ca.crt \
		door-control/main/certs/ca.crt

regenerate: $(CA).crt
	cp "$(CA).crt" door-control/main/certs/ca.crt
	cp "$(CA).crt" busch-interface/main/certs/ca.crt
	../debug/gen-client-cert.sh "$(CA)" door-control/main/certs/client
	../debug/gen-client-cert.sh "$(CA)" busch-interface/main/certs/client

firmware:
	idf.py -C busch-interface/ build
	idf.py -C door-control/ build

flash-busch-interface:
	idf.py -p "$(PORT)" -C busch-interface/ flash

flash-door-control:
	idf.py -p "$(PORT)" -C door-control/ flash

monitor:
	echo "Press C-a C-g to start ESP."
	picocom --baud 115200 --raise-rts --lower-dtr "$(PORT)"