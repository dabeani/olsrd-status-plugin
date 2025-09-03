#!/usr/bin/env bash
# Comprehensive smoke test for olsrd-status-plugin endpoints
# Override BASE to point at another host (e.g. http://127.0.0.1:11080)
BASE="${BASE:-http://193.238.158.74:11080}"
TIMEOUT=${TIMEOUT:-6}
SNIP=${SNIP:-300}

echo "Testing $BASE..."

check_one() {
  local path="$1"
  # use curl to fetch into a temp file, capture HTTP code and size
  tmp=$(mktemp /tmp/smoke.XXXXXX) || tmp=/tmp/smoke.tmp
  meta=$(curl -sS --max-time "$TIMEOUT" -w "%{http_code} %{size_download}" -o "$tmp" "$BASE$path" 2>/dev/null) || meta="000 0"
  code=$(printf "%s" "$meta" | awk '{print $1}')
  size=$(printf "%s" "$meta" | awk '{print $2}')
  if [ "$code" = "200" ] && [ "$size" -gt 0 ]; then
    status="PASS"
  else
    status="FAIL"
  fi
  msg="--- $status $path (code=$code size=$size) ---"
  printf '%s\n' "$msg"
  if [ "$size" -gt 0 ]; then
    head -c "$SNIP" "$tmp" | sed -e 's/\n/\\n/g'
    echo
  fi
  rm -f "$tmp"
}

# endpoints to check
endpoints=(
  "/"
  "/index.html"
  "/js/app.js"
  "/js/jquery.min.js"
  "/js/bootstrap.min.js"
  "/status"
  "/capabilities"
  "/connections.json"
  "/connections"
  "/versions.json"
  "/nodedb.json"
  "/airos"
  "/traceroute?target=8.8.8.8"
)

olsrd_qs=("links" "neighbors" "routes" "all" "version" "interfaces" "topology" "mid" "2hop" "neighbours")

for e in "${endpoints[@]}"; do
  check_one "$e"
done

echo "--- OLSRd proxy checks ---"
for q in "${olsrd_qs[@]}"; do
  check_one "/olsrd.json?q=$q"
done

echo "Done."
exit 0
