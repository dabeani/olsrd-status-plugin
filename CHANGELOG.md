# Changelog

## [Unreleased]
- feature: Add CIDR-aware node name lookup to resolve destinations via node_db longest-prefix matching (improves node counting in OLSR Links)
- fix: Silence compiler warnings and free temporary gw_stats allocation
- perf: Replace ad-hoc lookups with consistent node_db resolution for both routing-table fan-out and topology parsing

