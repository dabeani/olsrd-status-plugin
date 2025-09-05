#!/usr/bin/env python3
"""
Simple test harness that verifies longest-prefix CIDR matching behavior intended for
`find_best_nodename_in_nodedb` using a small in-repo Python implementation.
Run: python3 tests/test_cidr_lookup.py
"""
import ipaddress
import sys


def find_best_name(ndb, dest_ip):
    # ndb: dict mapping key->obj where key may be '1.2.3.4' or '1.2.3.0/24'
    dest = ipaddress.ip_address(dest_ip)
    candidates = []
    for key, val in ndb.items():
        try:
            if '/' in key:
                net = ipaddress.ip_network(key, strict=False)
            else:
                net = ipaddress.ip_network(key + '/32')
        except Exception:
            continue
        name = None
        if isinstance(val, dict):
            name = val.get('n') or val.get('name') or val.get('hostname')
        elif isinstance(val, str):
            name = val
        candidates.append((net, name))
    # longest prefix first
    candidates.sort(key=lambda x: x[0].prefixlen, reverse=True)
    for net, name in candidates:
        if dest in net:
            return name
    return None


def run_tests():
    passed = 0
    failed = 0

    # Case 1: exact match
    ndb = { '1.2.3.4': {'n': 'hostA'}, }
    assert find_best_name(ndb, '1.2.3.4') == 'hostA'

    # Case 2: CIDR match
    ndb = { '10.0.0.0/8': {'n': 'net10'}, '10.1.2.0/24': {'n': 'subnet'}, }
    assert find_best_name(ndb, '10.1.2.5') == 'subnet'
    assert find_best_name(ndb, '10.9.9.9') == 'net10'

    # Case 3: overlapping CIDRs choose longest prefix
    ndb = { '192.0.2.0/24': {'n': 'a'}, '192.0.2.128/25': {'n': 'b'} }
    assert find_best_name(ndb, '192.0.2.130') == 'b'
    assert find_best_name(ndb, '192.0.2.50') == 'a'

    # Case 4: no match returns None
    ndb = { '203.0.113.0/24': {'n': 'z'} }
    assert find_best_name(ndb, '198.51.100.5') is None

    # Case 5: keys can be 'ip' or 'cidr' and names come from different fields
    ndb = { '203.0.113.5': {'name': 'nm'}, '203.0.113.0/25': {'hostname': 'hn'} }
    assert find_best_name(ndb, '203.0.113.5') == 'nm'  # exact ip picks name field
    assert find_best_name(ndb, '203.0.113.6') == 'hn'

    print('All tests passed')


if __name__ == '__main__':
    try:
        run_tests()
    except AssertionError as e:
        print('Test failed:', e)
        sys.exit(2)
    sys.exit(0)
