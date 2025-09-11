Title: fix(ui): Prefer node_db canonical names for OLSR Links node resolution

Body:

This patch fixes a UI issue where the OLSR Links tab showed truncated device names (e.g. `eno`, `ramp8`) in the NODE column instead of canonical node names from the node database. The links view now loads `nodedb.json` (lazy fetch) and uses the same CIDR-aware / direct-key resolution as the Connections tab. It also avoids selecting short device-like `node_names` hints when a canonical node name is available.

Changes:
- `www/js/app.js`: lazy load `nodedb.json` in `populateOlsrLinksTable`, prefer `getNodeNameForIp` and `_nodedb_cache` lookups, and add heuristic to avoid short device hints.
- `CHANGELOG.md`: add Unreleased entry describing the fix.

Testing:
- Deploy to a staging instance, open OLSR Links tab, verify canonical names appear. Enable `window._uiDebug = true` for diagnostic logs.
