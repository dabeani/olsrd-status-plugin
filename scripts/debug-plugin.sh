#!/bin/bash
# OLSR Status Plugin Debug Script
# Run this on the target device to check plugin status

echo "=== OLSR Status Plugin Debug ==="
echo

echo "1. Checking OLSR processes:"
ps aux | grep olsrd | grep -v grep || echo "No OLSR processes found"
echo

echo "2. Checking OLSR API endpoints:"
for port in 9090 2006 8123; do
    echo "Testing http://127.0.0.1:$port/links"
    curl -s --max-time 2 http://127.0.0.1:$port/links | head -c 100
    echo
done
echo

echo "3. Checking ubnt-discover:"
which ubnt-discover || echo "ubnt-discover not found"
echo

echo "4. Checking traceroute:"
which traceroute || echo "traceroute not found"
echo

echo "5. Checking asset files:"
ls -la /usr/share/olsrd-status-plugin/www/ 2>/dev/null || echo "Asset directory not found"
echo

echo "6. Checking configuration files:"
ls -la /config/custom/www/settings.inc 2>/dev/null || echo "Settings file not found"
ls -la /config/custom/versions.sh 2>/dev/null || echo "Versions script not found"
ls -la /config/custom/connections.sh 2>/dev/null || echo "Connections script not found"
echo

echo "7. Checking temporary files:"
ls -la /tmp/10-all.json 2>/dev/null || echo "10-all.json not found"
echo

echo "8. Testing plugin HTTP endpoints (if running):"
curl -s http://127.0.0.1:2006/status | head -c 200 || echo "Status endpoint not accessible"
echo
curl -s http://127.0.0.1:2006/capabilities | head -c 200 || echo "Capabilities endpoint not accessible"
echo

echo "Debug complete. Check the plugin logs for more details."
