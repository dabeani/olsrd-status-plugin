#!/usr/bin/env python3
"""
compare_status.py

Fetch /status.py?get=... from two hosts, normalize JSON/text, diff and produce a summary and artifacts.

Usage: python3 scripts/compare_status.py --hostA http://a/status.py --hostB http://b/status.py [--outdir /tmp/status_cmp] [--gets test,ipv4,...]

This script uses only the Python standard library.
"""
from __future__ import annotations
import argparse
import json
import os
import sys
import urllib.request
import urllib.parse
import shutil
import time
from typing import List, Tuple
import difflib


DEFAULT_GETS = ["", "default", "test", "ipv4", "ipv6", "olsrd", "jsoninfo", "txtinfo", "traffic", "airos", "status", "discover", "connections", "html"]


def fetch(url: str, timeout: int = 12) -> Tuple[int, dict, bytes]:
    """Return (http_code, headers_dict, body_bytes)"""
    req = urllib.request.Request(url, headers={"User-Agent": "compare_status/1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            code = resp.getcode()
            headers = {k.lower(): v for k, v in resp.getheaders()}
            body = resp.read()
            return code, headers, body
    except Exception as e:
        return 0, {"error": str(e)}, b""


def normalize(body: bytes) -> str:
    """Try to parse as JSON and return a canonical string; otherwise decode text and strip."""
    if not body:
        return ""
    try:
        j = json.loads(body.decode("utf-8"))
        return json.dumps(j, sort_keys=True, separators=(",",":"))
    except Exception:
        try:
            return body.decode("utf-8").strip()
        except Exception:
            # fallback binary repr
            return repr(body[:400])


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def save(path: str, data: bytes) -> None:
    with open(path, "wb") as f:
        f.write(data)


def save_text(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def run_compare(hostA: str, hostB: str, gets: List[str], outdir: str) -> int:
    ensure_dir(outdir)
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifacts = []
    summary_lines: List[str] = []
    summary_lines.append(f"compare_status report {timestamp}")
    summary_lines.append(f"hostA: {hostA}")
    summary_lines.append(f"hostB: {hostB}")
    summary_lines.append("")

    for g in gets:
        q = ("?get=" + urllib.parse.quote(g)) if g else ""
        label = g or "(no param)"
        summary_lines.append(f"=== {label} ===")
        a_code, a_hdr, a_body = fetch(hostA + q)
        b_code, b_hdr, b_body = fetch(hostB + q)

        a_pref = f"hostA_{g or 'noparam'}"
        b_pref = f"hostB_{g or 'noparam'}"
        a_raw = os.path.join(outdir, a_pref + ".raw")
        b_raw = os.path.join(outdir, b_pref + ".raw")
        save(a_raw, a_body)
        save(b_raw, b_body)
        artifacts.extend([a_raw, b_raw])

        a_norm = normalize(a_body)
        b_norm = normalize(b_body)
        a_norm_path = os.path.join(outdir, a_pref + ".norm")
        b_norm_path = os.path.join(outdir, b_pref + ".norm")
        save_text(a_norm_path, a_norm)
        save_text(b_norm_path, b_norm)
        artifacts.extend([a_norm_path, b_norm_path])

        summary_lines.append(f"hostA HTTP:{a_code} CT:{a_hdr.get('content-type','-') if isinstance(a_hdr, dict) else a_hdr}")
        summary_lines.append(f"hostB HTTP:{b_code} CT:{b_hdr.get('content-type','-') if isinstance(b_hdr, dict) else b_hdr}")

        if a_norm == b_norm:
            summary_lines.append("MATCH")
        else:
            summary_lines.append("DIFFER")
            # produce short unified diff
            ad = a_norm.splitlines(keepends=True)
            bd = b_norm.splitlines(keepends=True)
            ud = difflib.unified_diff(ad, bd, fromfile='hostA', tofile='hostB')
            summary_lines.append("--- diff start ---")
            for i, line in enumerate(ud):
                if i >= 200:
                    summary_lines.append("...diff truncated...")
                    break
                summary_lines.append(line.rstrip('\n'))
            summary_lines.append("--- diff end ---")

        summary_lines.append("")

    summary_path = os.path.join(outdir, f"summary_{timestamp}.txt")
    save_text(summary_path, "\n".join(summary_lines))
    print("Summary written to:", summary_path)
    print("Artifacts directory:", outdir)
    print()
    print("--- begin summary ---")
    print("\n".join(summary_lines))
    print("--- end summary ---")
    return 0


def parse_gets(s: str) -> List[str]:
    if not s:
        return DEFAULT_GETS
    return [x for x in (item.strip() for item in s.split(',')) if x != '']


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description="Compare /status.py outputs from two hosts")
    p.add_argument("--hostA", required=False, default="http://193.238.158.74/status.py", help="First host base URL")
    p.add_argument("--hostB", required=False, default="http://78.41.112.141/status.py", help="Second host base URL")
    p.add_argument("--gets", required=False, help="Comma-separated get values to test (default: all legacy)")
    p.add_argument("--outdir", required=False, default="/tmp/status_cmp", help="Directory to write artifacts")
    args = p.parse_args(argv)

    gets = parse_gets(args.gets) if args.gets is not None else DEFAULT_GETS
    ensure_dir(args.outdir)
    return run_compare(args.hostA, args.hostB, gets, args.outdir)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
