.[] | "command=\"echo /opt/portal300/portal-trigger " + $action + " -i " + .number + " -f '" + .name + "' -n '" + .nick + "'\",no-port-forwarding,no-X11-forwarding,no-agent-forwarding " + .ssh_public_key