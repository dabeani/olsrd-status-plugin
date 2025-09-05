# olsrd-status-plugin

Light‑weight HTTP status / diagnostics plugin for legacy **olsrd (v1)** and **olsr2** daemons. It exposes a small embedded web UI plus JSON APIs that aggregate and normalize link, neighbor, route, topology, device discovery and system information into a single, cache‑friendly interface for monitoring tools or browsers.

## Key Capabilities
* Unified detection of running OLSR daemons (olsrd v1 and olsr2) inside classic systems, EdgeRouters and Linux containers.
* Normalized per‑neighbor link table with derived fields: `routes`, `nodes`, `is_default`, reverse DNS of peers.
* Route & topology correlation (counts per neighbor) with multi‑stage fallback (routes -> topology -> neighbors two‑hop -> pattern scan) so counts rarely stay zero.
* Neighbor normalization (olsr2 JSON or legacy link output) to a consistent shape for the UI.
* Device discovery aggregation:
    * Primary: `ubnt-discover` binary (JSON mode)
    * Fallbacks: cached dump, active broadcast probe, ARP table synthesis
    * Reverse DNS enrichment.
* Dynamic `nodedb.json` style output (object map form) synthesized from real sources with validation & ARP fallback – keeps external scripts compatible.
* Connection / MAC / IP correlation via simple `/connections.json` (bridges, interfaces, learned MACs & IPs).
* Capability probing (container vs edge router, presence of traceroute, txtinfo/jsoninfo endpoints) exposed at `/capabilities` for UI feature gating.
* Traceroute helper (if available) and default‑route enrichment with hostname resolution.
* Zero external runtime deps beyond libc + (optionally) `curl` and `ubnt-discover`; everything else is defensive / best‑effort.
* Clean build with strict warnings; internal helpers for safe string assembly & JSON escaping.

For a detailed list of recent changes and upgrade notes please see `CHANGELOG.md`.

Example `/versions.json` snippet (best-effort fields shown):

```json
{
    "host": "edge1",
    "system": "edge-router",
    "olsrd_running": true,
    "olsr2_running": false,
    "bmk_webstatus": "1.3.7",
    "ipv4": "192.168.1.1",
    "ipv6": "fe80::abcd",
    "linkserial": "A4B1C2D3E4F5",
    "olsrd": "OLSRd 0.9.8 - build x86_64",
    "olsrd_details": {
        "version": "0.9.8",
        "description": "OLSRd built for x86_64",
        "device": "EdgeRouter X",
        "date": "2024-05-04",
        "release": "stable",
        "source": "/usr/sbin/olsrd"
    }
}
```


## HTTP Endpoints (JSON unless stated)
| Endpoint | Purpose |
|----------|---------|
| `/` (web UI) | Static SPA assets (Bootstrap, JS) served from `assetroot`.
| `/status` | Full status: system, versions, devices, links, neighbors, routes/topology raw, diagnostics.
| `/status/olsr` | Focused OLSR subset (fast link/neighbor view + default route + flags).
| `/status/lite` | Very fast status (omits heavy OLSR normalization) – good for frequent polling.
| `/olsr/links` | Normalized link table (same shape as embedded in `/status`).
| `/olsr/raw` | Concatenated raw JSON from OLSR endpoints (links/routes/topology) for debugging.
| `/olsr/routes?via=IP` | Filtered routes via specific neighbor (or all).
| `/nodedb.json` | Node database map (object keyed by IPv4) – synthesizes if remote/unavailable.
| `/connections.json` | Interface -> (MACs, IPs) mapping derived from ARP.
| `/versions.json` | Plugin + host version snapshot.
| `/capabilities` | Booleans describing environment & optional features.
| `/traceroute.json` (if implemented in UI) | Cached traceroute hops (subset included in `/status`).

Raw OLSR sections optionally embedded in `/status`:
* `olsr_neighbors_raw`
* `olsr_routes_raw`
* `olsr_topology_raw`

## Data Sources & Normalization Flow
1. Probe local OLSR JSON endpoints (`:9090`, `:2006`, `:8123`) with short timeouts.
2. Concatenate available raw documents for unified parsing.
3. Extract link objects; for each neighbor IP derive:
     * `routes`: count of route entries whose gateway/nextHop matches remote.
     * `nodes`: unique topology destinations reachable via that remote (fallback chain: topology unique -> topology raw count -> two-Hop neighbor count -> routes count -> pattern scan).
4. Resolve default route and flag matching neighbor `is_default`.
5. Enrich LQ/NLQ, cost, reverse DNS, and hostnames.
6. Optionally merge UBNT discovery + ARP into device inventory & node DB.

## Environment & Runtime Detection
* EdgeRouter detection (filesystem layout) to add `admin_url`.
* Container detection (cgroups / proc) to adjust behavior & disable non‑existent hardware probes.
* Presence of tools (`ubnt-discover`, `traceroute`, `curl`).

## Debugging Aids
Set `OLSR_DEBUG_LINK_COUNTS=1` before starting olsrd to emit stderr diagnostics when route/node fallbacks still yield zero.

## Build / Install
Build:
```bash
make status_plugin_clean && make status_plugin
sudo make status_plugin_install DESTDIR=/
sudo /usr/share/olsrd-status-plugin/fetch-assets.sh
```
Add to olsrd.conf:
```
LoadPlugin "lib/olsrd-status-plugin/build/olsrd_status.so.1.0"
{
    PlParam "bind"       "0.0.0.0"
    PlParam "port"       "11080"
    PlParam "enableipv6" "0"
    PlParam "assetroot"  "/usr/share/olsrd-status-plugin/www"
}

## Plugin Parameters
* `bind` – listen address (default 127.0.0.1 if not set).
* `port` – TCP listen port (default 11080 shown above).
* `enableipv6` – (placeholder) toggle IPv6 support future use.
* `assetroot` – directory containing `www/` assets (index.html, CSS, JS).

## Security Notes
* Endpoint intentionally unauthenticated for embedded deployment simplicity; place behind firewall or restrict `bind` to management network if needed.
* All parsing is defensive with bounds checks; malformed upstream JSON will degrade gracefully (empty arrays / zero counts) rather than crash.

## Extending
Typical extension points inside `src/olsrd_status_plugin.c`:
* Add new counters in `normalize_olsrd_links()`.
* Add alternative discovery sources in `ubnt_discover_output()` or ARP synthesizer.
* Register new HTTP handlers near end of file where existing routes are registered.

## License
Same license as upstream olsrd project (inherit; add explicit file header if distributing separately).

---
This README summarizes functionality; for deeper internals inspect `src/olsrd_status_plugin.c` (single translation unit for easier embedding) and the `www/js/app.js` UI logic.
```

# how to bring it into the build env
scp olsrd.......zip ubnt@172.30.1.96:/home/ubnt/

# from the root of your olsrd repo
unzip -o olsrd-status-plugin-packed-v11.zip -d .

# build the plugin
make -C lib/olsrd-status-plugin status_plugin_clean status_plugin

# install the .so and helper files
sudo make -C lib/olsrd-status-plugin status_plugin_install DESTDIR=/

# fetch Bootstrap + glyphicons to the assetroot
sudo /usr/share/olsrd-status-plugin/fetch-assets.sh


# how to load plugin
LoadPlugin "lib/olsrd-status-plugin/build/olsrd_status.so.1.0"
{
    PlParam "bind"       "0.0.0.0"
    PlParam "port"       "11080"
    PlParam "enableipv6" "0"
    PlParam "assetroot"  "/usr/share/olsrd-status-plugin/www"
}
