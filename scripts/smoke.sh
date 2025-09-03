#!/bin/sh
# Simple smoke test for local plugin endpoints
base="http://127.0.0.1:11080"
set -e
echo "Testing $base..."
for e in /status /capabilities /connections.json /connections /versions.json /nodedb.json /airos /traceroute?target=8.8.8.8; do
  echo -n "GET $e -> "
  curl -sS --max-time 3 "$base$e" | head -c 200 | sed -e 's/\n/\\n/g'
  echo "\n---"
done

exit 0
