#ifndef PORTAL300_ETHERNET_H
#define PORTAL300_ETHERNET_H

//! Sets up and starts the WT32-ETH01 ethernet port with the given `host_name`.
//! If `host_name` is NULL, the default hostname is used.
void ethernet_init(char const * host_name);

#endif