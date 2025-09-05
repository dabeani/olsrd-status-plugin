#!/usr/bin/env bash
# quick dev smoke-check for local plugin (assumes http server is running on localhost:11080)
set -euo pipefail
HOST=193.238.158.74:11080
echo "Checking /capabilities..."
curl -sS --fail http://${HOST}/capabilities | jq .
echo "Checking /status..."
curl -sS --fail http://${HOST}/status | jq .

echo "OK"
