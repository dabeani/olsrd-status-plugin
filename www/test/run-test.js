const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

async function main() {
  const html = fs.readFileSync(path.resolve(__dirname, '../index.html'), 'utf8');
  const js = fs.readFileSync(path.resolve(__dirname, '../js/app.js'), 'utf8');
  // create a JSDOM instance with basic globals
  const dom = new JSDOM(html, { runScripts: 'outside-only', resources: 'usable' });
  const { window } = dom;
  global.window = window;
  global.document = window.document;
  global.navigator = window.navigator;

  // minimal fetch stub for /versions.json
  window.fetch = (url, opts) => {
    if (url === '/versions.json') {
      return Promise.resolve({ ok: true, json: () => Promise.resolve({ hostname: 'test-host', system: 'testsys', ipv4: '192.0.2.1', olsrd_exists: true, olsrd_running: true, olsr2_exists: false, olsr2_running: false }) });
    }
    return Promise.resolve({ ok: true, json: () => Promise.resolve({}) });
  };

  // evaluate the app.js into the window context
  const scriptEl = dom.window.document.createElement('script');
  scriptEl.textContent = js;
  dom.window.document.body.appendChild(scriptEl);

  // ensure global header placeholder exists
  let globalHeader = dom.window.document.getElementById('global-versions-header');
  if (!globalHeader) {
    globalHeader = dom.window.document.createElement('div');
    globalHeader.id = 'global-versions-header';
    dom.window.document.body.insertBefore(globalHeader, dom.window.document.body.firstChild);
  }

  // call renderVersionsPanel by fetching /versions.json to simulate app behavior
  const v = await window.fetch('/versions.json').then(r=>r.json());
  if (typeof window.renderVersionsPanel !== 'function') {
    console.error('renderVersionsPanel not found in app.js'); process.exit(2);
  }
  window.renderVersionsPanel(v);

  // Assertions
  const header = dom.window.document.querySelector('#global-versions-header .panel');
  if (!header) { console.error('Header panel not rendered'); process.exit(2); }
  const badges = dom.window.document.querySelectorAll('#global-versions-header .status-badges .status-olsr-badge');
  if (!badges || badges.length === 0) { console.error('No OLSR badges rendered'); process.exit(2); }

  // Check that headerRow is a flex container and that computed width fits single line for common widths
  const headerBody = dom.window.document.querySelector('#global-versions-header .panel-body');
  const styleDisplay = headerBody && headerBody.style && headerBody.style.display;
  if (styleDisplay !== 'flex') {
    console.error('Header body not configured as flex'); process.exit(2);
  }

  // Simulate multiple viewport widths and ensure header content doesn't exceed width (basic check using scrollWidth)
  const widths = [320, 480, 768, 1024, 1366];
  let failed = false;
  widths.forEach(w => {
    dom.window.innerWidth = w;
    // force reflow
    const sw = header.scrollWidth;
    if (sw > w) {
      console.warn('Header overflow at width', w, 'scrollWidth', sw);
      failed = true;
    }
  });
  if (failed) { console.error('Header does not fit at all tested widths'); process.exit(2); }

  console.log('All UI checks passed');
  process.exit(0);
}

main().catch(err => { console.error(err); process.exit(3); });
