Here’s what you want from me, based on your conversation:

The plugin and web interface must match the Python reference (bmk-webstatus.py) in features, endpoints, and UI layout (Bootstrap, pixel-perfect, all strings/icons).
All hardcoded HTML must be moved out of the C plugin and into external assets (index.html, app.js).
The plugin must serve all required endpoints (/status, /capabilities, /nodedb.json, /connections.json, /versions.json, etc.) with correct, normalized, and enriched data.
The web UI must show all values (devices, links, neighbors, traceroute, uptime, etc.) exactly as in the Python reference, with correct formatting and no placeholders.
The build must be completely free of warnings and errors.
All backend data sources (ubnt-discover, OLSRd API, ARP, etc.) must be detected and used; fallback logic must work.
If anything is missing, broken, or not shown in the UI, I must diagnose and fix it, including guiding you through system setup if needed.
I must keep going until everything works and matches the reference, without asking what to do next.

important:
the bmk-webstatus.py script is running usually on ubiquiti edgerouters, therefore it needs to be converted to a linux (busybox/alpine) container which is not related to ubiquiti. there need to be a autodetect funtionality which recognize if its ubiquiti edgerouter (for parameters), or if its a linux container. 

this is a plugin for the routing protocol olsrd!