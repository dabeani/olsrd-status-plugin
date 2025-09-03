# olsrd-status-plugin (v10)
Build:
```bash
make status_plugin_clean && make status_plugin
sudo make status_plugin_install DESTDIR=/
sudo /usr/share/olsrd-status-plugin/fetch-assets.sh
```
Add to olsrd.conf:
```
LoadPlugin "lib/olsrd-status-plugin/build/olsrd_status.so.1.0"
{
    PlParam "bind"       "0.0.0.0"
    PlParam "port"       "11080"
    PlParam "enableipv6" "0"
    PlParam "assetroot"  "/usr/share/olsrd-status-plugin/www"
}
```
