#!/usr/bin/env bash
set -euo pipefail
PREFIX="/usr/share/olsrd-status-plugin/www"
mkdir -p "$PREFIX"/{css,js,fonts}
echo "Downloading Bootstrap 3.4.1 + jQuery 1.12.4 assets into $PREFIX"
curl -L -o "$PREFIX/css/bootstrap.min.css" https://cdn.jsdelivr.net/npm/bootstrap@3.4.1/dist/css/bootstrap.min.css
curl -L -o "$PREFIX/js/bootstrap.min.js"   https://cdn.jsdelivr.net/npm/bootstrap@3.4.1/dist/js/bootstrap.min.js
curl -L -o "$PREFIX/js/jquery.min.js"      https://code.jquery.com/jquery-1.12.4.min.js
for f in glyphicons-halflings-regular.eot glyphicons-halflings-regular.svg glyphicons-halflings-regular.ttf glyphicons-halflings-regular.woff glyphicons-halflings-regular.woff2; do
  curl -L -o "$PREFIX/fonts/$f" "https://raw.githubusercontent.com/twbs/bootstrap/v3.4.1/fonts/$f"
done
echo "Done."
