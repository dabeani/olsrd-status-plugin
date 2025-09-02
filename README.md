# from the root of your olsrd repo
unzip -o olsrd-status-plugin-packed-v11.zip -d .

# build the plugin
make -C lib/olsrd-status-plugin status_plugin_clean status_plugin

# install the .so and helper files
sudo make -C lib/olsrd-status-plugin status_plugin_install DESTDIR=/

# fetch Bootstrap + glyphicons to the assetroot
sudo /usr/share/olsrd-status-plugin/fetch-assets.sh
