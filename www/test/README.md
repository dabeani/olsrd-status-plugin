Quick UI test harness

This minimal harness uses jsdom to load `www/index.html` and `www/js/app.js`, stubs `/versions.json`, calls `renderVersionsPanel()` and checks that the global header is rendered and that it fits common viewport widths.

Usage:

1. cd www/test
2. npm install
3. npm test

This is intentionally minimal and avoids adding a full test framework. It helps verify DOM structure changes and the refresh wiring.
