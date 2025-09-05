Title: CIDR-aware node name lookup for more accurate node counts in OLSR Links

Summary

This change improves node name resolution used for node counting in the OLSR Links view by implementing a CIDR-aware, longest-prefix match lookup against the external `node_db.json` mapping. It replaces previous ad-hoc string scans with a consistent lookup used for both (1) routing-table fan-out (Python parity) and (2) topology parsing.

Key changes

- feature: `find_best_nodename_in_nodedb` helper implementing longest-prefix CIDR matching and extraction of `n` (node name).
- refactor: Use CIDR-aware lookup in `count_unique_nodes_for_ip` and in the routing-table fan-out (`gw_stats`) logic.
- fix: Silence compiler warnings and small memory cleanup (free `gw_stats` on all code paths).
- test: Add `tests/test_cidr_lookup.py` validating longest-prefix behavior.
- docs: Add `CHANGELOG.md` and this `PR_DRAFT.md` describing the change.

Why

Previously node resolution relied on brittle string searches that missed CIDR-keyed node_db entries (e.g. "78.41.119.36/30"). This produced undercounts or inconsistent node names where HNA/CIDR keys are in use. Implementing a CIDR-aware longest-prefix match brings behavior closer to expected network semantics and to the Python reference.

Testing performed

- Local build: `make` succeeded after edits.
- Live verification against a running plugin instance: compared reported `/olsr/links` values to computed results derived from `/olsr/raw` + `nodedb.json`. Most gateways matched; a couple of small mismatches identified and noted for follow-up.
- Unit tests: `tests/test_cidr_lookup.py` passes locally.

Checklist for reviewers

- [ ] Code review for correctness and potential edge cases (malformed node_db JSON, huge node_db size).
- [ ] Confirm behavior on representative production nodes (particularly those with CIDR keys in `nodedb.json`).
- [ ] Consider replacing ad-hoc JSON parsing with a lightweight parser for robustness (future work).

Notes / follow-ups

- The current lookup uses a simple textual scan to find keys and object boundaries; it's intentionally lightweight to avoid new dependencies and to keep memory overhead small. For larger node_db files or more varied JSON formatting a JSON parser would be more robust.
- The unit test harness is Python-based for ease; additional C-level tests could be added if desired.

Suggested PR body (copy-paste):

[include the Title, Summary, Key changes, Why, Testing performed and Checklist sections above]
