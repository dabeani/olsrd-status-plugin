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

## Environment overrides
The plugin supports a small set of environment variables that can supply runtime defaults or override configuration in specific cases. Use these from systemd unit files, container run commands, or shell wrappers.

Precedence summary
* For `port`, `nodedb_url`, `nodedb_ttl`, and `nodedb_write_disk`: configuration file `PlParam` (olsrd.conf) wins. Environment variables are used only when no `PlParam` is supplied.
* For network allow-list (`Net`): if `OLSRD_STATUS_PLUGIN_NET` is present in the environment it is treated as authoritative and replaces any `PlParam "Net"` entries.

Supported environment variables

* `OLSRD_STATUS_PLUGIN_PORT` – numeric TCP port (1-65535). Example:

```bash
export OLSRD_STATUS_PLUGIN_PORT=8080
```

* `OLSRD_STATUS_PLUGIN_NET` – allow-list entries for HTTP access control. Can contain multiple entries separated by commas, semicolons or whitespace. Each token must be a CIDR (e.g. `192.168.0.0/24`), a single address (`10.0.0.5`) or an address+mask (`192.168.0.0 255.255.252.0`). Example:

```bash
export OLSRD_STATUS_PLUGIN_NET="192.168.1.0/24,10.0.0.0/8"
```

Note: when this env var is present it replaces any `PlParam "Net"` entries from `olsrd.conf`.

* `OLSRD_STATUS_PLUGIN_NODEDB_URL` – URL string for the remote node DB used to populate `nodedb.json`.

```bash
export OLSRD_STATUS_PLUGIN_NODEDB_URL="https://example.com/nodedb.json"
```

* `OLSRD_STATUS_PLUGIN_NODEDB_TTL` – integer seconds TTL for the cached node DB.

```bash
export OLSRD_STATUS_PLUGIN_NODEDB_TTL=600
```

* `OLSRD_STATUS_PLUGIN_NODEDB_WRITE_DISK` – integer (0 or 1) controlling whether the node DB is written to disk.

```bash
export OLSRD_STATUS_PLUGIN_NODEDB_WRITE_DISK=1
```

* `OLSRD_STATUS_FETCH_STARTUP_WAIT` – optional integer seconds to wait during plugin startup for DNS/network readiness before attempting the first remote fetch. Useful in containers where networking may be delayed. Default is 30 seconds.

```bash
export OLSRD_STATUS_FETCH_STARTUP_WAIT=60
```

Additional runtime tuning (PlParam names available in `olsrd.conf` plugin block):

* `OLSRD_STATUS_FETCH_QUEUE_MAX` / PlParam `fetch_queue_max` – maximum number of pending fetch requests queued before new non-waiting requests are dropped. Default: 4.

```bash
export OLSRD_STATUS_FETCH_QUEUE_MAX=8
```

* `OLSRD_STATUS_FETCH_RETRIES` / PlParam `fetch_retries` – number of retry attempts the background fetch worker will make on transient failures. Default: 3.

```bash
export OLSRD_STATUS_FETCH_RETRIES=5
```

* `OLSRD_STATUS_FETCH_BACKOFF_INITIAL` / PlParam `fetch_backoff_initial` – initial backoff in seconds used when retrying; backoff doubles each attempt. Default: 1.

```bash
export OLSRD_STATUS_FETCH_BACKOFF_INITIAL=2
```

Precedence: for all of the above the plugin parameter `PlParam` in `olsrd.conf` wins when present; otherwise the corresponding environment variable is used; otherwise the compiled default applies.

Logging and debugging

* The plugin logs which environment values are applied or ignored during startup to stderr.
* For verbose allow-list tracing set `OLSR_DEBUG_ALLOWLIST=1` to see per-entry add/match traces.
* When `OLSRD_STATUS_PLUGIN_NET` is used the plugin prints a friendly list of the final allow-list entries at startup.

## Security Notes
* Endpoint intentionally unauthenticated for embedded deployment simplicity; place behind firewall or restrict `bind` to management network if needed.
* All parsing is defensive with bounds checks; malformed upstream JSON will degrade gracefully (empty arrays / zero counts) rather than crash.

## Extending
Typical extension points inside `src/olsrd_status_plugin.c`:
* Add new counters in `normalize_olsrd_links()`.
* Add alternative discovery sources in `ubnt_discover_output()` or ARP synthesizer.
* Register new HTTP handlers near end of file where existing routes are registered.

## Extensions
This project has a few small runtime extensions you can use for monitoring and to control noisy log output.

- Prometheus metrics endpoint: the plugin exposes a minimal Prometheus-compatible metrics page at `/metrics` which exports fetch queue and counter metrics (queue length, dropped, retries, successes and per-type enqueue/processed counters). Useful for scraping with Prometheus or Promtail.

- Toggle queue-operation logging: suppress detailed fetch-queue progress messages that are printed to stderr by default. Use either the PlParam or environment variable:
    - PlParam: `fetch_log_queue` (0 = off, 1 = on)
    - Env var: `OLSRD_STATUS_FETCH_LOG_QUEUE` (export before starting olsrd)

- Periodic fetch reporter: a periodic summary of fetch metrics can be enabled via PlParam `fetch_report_interval` or env `OLSRD_STATUS_FETCH_REPORT_INTERVAL` (seconds). When enabled this prints a short summary to stderr every N seconds; it respects the `fetch_log_queue` toggle.

- UI auto-refresh hint: the plugin can suggest an auto-refresh interval to the web UI via PlParam `fetch_auto_refresh_ms` or env `OLSRD_STATUS_FETCH_AUTO_REFRESH_MS` (milliseconds). The front-end honors this as a suggested default for the Fetch Queue auto-refresh control.

Examples

```bash
# disable fetch queue log noise entirely
export OLSRD_STATUS_FETCH_LOG_QUEUE=0

# enable periodic summary every 60s
export OLSRD_STATUS_FETCH_REPORT_INTERVAL=60
```

These extensions are intentionally lightweight so they can be toggled from systemd unit files or container env without changing `olsrd.conf`.

## License
Same license as upstream olsrd project (inherit; add explicit file header if distributing separately).

---
This README summarizes functionality; for deeper internals inspect `src/olsrd_status_plugin.c` (single translation unit for easier embedding) and the `www/js/app.js` UI logic.

## Build-time fetch options

Two Makefile options control how the plugin fetches remote resources (notably `nodedb.json`):

- `ENABLE_CURL_FALLBACK` (default: `1`) — when enabled, if libcurl is not available the plugin will attempt to spawn an external `curl` binary at runtime as a last resort. Set to `0` to disable the external-binary fallback and require libcurl or accept no HTTPS fetching.

    Example: build without external fallback:

    ```bash
    make ENABLE_CURL_FALLBACK=0
    ```

- `REQUIRE_FETCH` (default: `0`) — opt-in hard failure when neither libcurl (dev headers detected at build) nor an external `curl` binary are present. This is useful for CI or production images where missing fetch capability should abort the build early.

    Example: fail build if no fetch capability is present:

    ```bash
    make REQUIRE_FETCH=1
    ```

During `make` you will see diagnostic messages such as:

```
>>> libcurl NOT detected; building without libcurl support
>>> external curl fallback: ENABLED (set ENABLE_CURL_FALLBACK=0 to disable)
```

Runtime expectations

* If libcurl dev headers were available at build time, the binary will use libcurl for HTTPS fetches (recommended).
* If libcurl wasn't available but `ENABLE_CURL_FALLBACK=1`, the plugin will attempt to spawn an external `curl` binary when needed — ensure `curl` is installed in the runtime image.
* If libcurl wasn't available and `ENABLE_CURL_FALLBACK=0`, the plugin will not attempt external fetches; remote HTTPS node DB fetching will not occur (plain http:// URLs may still work via the internal socket helper).

Recommendations:

* For production builds, install libcurl dev packages in the build image so the plugin includes native HTTPS fetching.
* For minimal runtime images, either ensure `curl` binary is present or keep `ENABLE_CURL_FALLBACK=1` and accept the external dependency.

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
