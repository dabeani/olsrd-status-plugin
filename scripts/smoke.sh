#!/bin/sh
# Simple smoke test for plugin endpoints
# Default to the remote instance you provided; override with BASE env var if needed.
BASE="${BASE:-http://193.238.158.74:11080}"
set -e
echo "Testing $BASE..."
for e in /status /capabilities /connections.json /connections /versions.json /nodedb.json /airos '/traceroute?target=8.8.8.8'; do
  echo "--- GET $e ---"
  if ! curl -sS --max-time 6 "$BASE$e" | head -c 1000 | sed -e 's/\n/\\n/g'; then
    echo "(request failed)"
  fi
done

exit 0
