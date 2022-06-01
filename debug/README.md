# Debug Utilities

This folder contains utilities to spawn a local MQTT server "mosquitto" with a exemplary configuration.

The following scripts are provided:

- `gen-ca.sh`: Generates a new certificate authority `ca.portal`
- `gen-cert.sh`: Generates a new server certificate `mqtt.portal`
- `gen-client-cert.sh`: Generates a new client certificate `portal.portal`
- `mosquitto.sh`: Runs a local mosquitto instance with `server.key` and `ca.crt`
- `mosquitto-test.sh`: Connects to the local mosquitto instance and tests the connection

Run these scripts in the following order before starting to debug:

```sh-session
[user@host debug]$ ./gen-ca.sh ca
Generating RSA private key, 2048 bit long modulus (2 primes)
.........................................................+++++
...............................................+++++
e is 65537 (0x010001)
[user@host debug]$ ./gen-server-cert.sh ca server
Generating RSA private key, 2048 bit long modulus (2 primes)
...................................................+++++
.....+++++
e is 65537 (0x010001)
Signature ok
subject=C = DE, O = shackspace, OU = portal, CN = localhost
Getting CA Private Key
[user@host debug]$ ./gen-client-cert.sh ca client
Generating RSA private key, 2048 bit long modulus (2 primes)
........+++++
........................+++++
e is 65537 (0x010001)
Signature ok
subject=C = DE, O = shackspace, OU = portal, CN = portal.portal
Getting CA Private Key
[user@host debug]$
```

Then to start the mosquitto server, run this:

```sh-session
[user@host debug]$ ./mosquitto.sh
1654075678: mosquitto version 2.0.9 starting
1654075678: Config loaded from /home/felix/projects/shackspace/portal300/code/debug/mosquitto.conf.
1654075678: Opening ipv4 listen socket on port 8883.
1654075678: Opening ipv6 listen socket on port 8883.
1654075678: mosquitto version 2.0.9 running
```

To test the setup, you can run the test script while the mosquitto server is running:

```sh-session
[user@host debug]$ ./mosquitto-test.sh
Client (null) sending CONNECT
Client (null) received CONNACK (0)
Client (null) sending PUBLISH (d0, q0, r0, m1, 'system/test', ... (21 bytes))
Client (null) sending DISCONNECT
[user@host debug]$
```
