# ubnt-discover CLI

A small standalone helper that broadcasts a UBNT v1 discovery probe and prints parsed device fields.

- Binary: `rev/discover/ubnt_discover_cli`
- Purpose: quick, local discovery probe useful for debugging networks and validating device replies (TLV parsing + ARP fallback).

## Options

- `-b bind_ip`  Bind the local socket to this address (uses `ubnt_open_broadcast_socket_bound`).
- `-p port`     Destination and bind port (default: `10001`).
- `-t timeout`  Listening timeout in seconds (default: `3`).
- `-d`          Enable runtime debug output (sets `OLSRD_STATUS_UBNT_DEBUG=1`).
- `-h`          Show help / usage.

## Examples

```
# default probe (may require sudo depending on network privileges):
./rev/discover/ubnt_discover_cli

# bind to a specific local address and listen 5s:
./rev/discover/ubnt_discover_cli -b 192.168.1.5 -t 5

# send to a non-default port and enable debug output:
./rev/discover/ubnt_discover_cli -p 20001 -d
```

The CLI is intentionally standalone (it provides a minimal logging stub) and does not depend on loading the plugin; it's useful for developers and ops when investigating why the plugin's UBNT discovery yields limited metadata.
