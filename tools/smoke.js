// Simple Puppeteer smoke tester
// Usage: node tools/smoke.js <url>
// Writes smoke-console.log and smoke-network.log in the current working dir

const fs = require('fs');
const puppeteer = require('puppeteer');

async function run(url) {
  const consoleEvents = [];
  const networkEvents = [];
  const browser = await puppeteer.launch({ args: ['--no-sandbox','--disable-setuid-sandbox'] });
  const page = await browser.newPage();

  page.on('console', msg => {
    try {
      const args = msg.args();
      const text = msg.text();
      consoleEvents.push({ type: 'console', level: msg.type(), text: text, location: msg.location() });
    } catch (e) {
      consoleEvents.push({ type: 'console', level: msg.type(), text: String(msg) });
    }
  });

  page.on('pageerror', err => {
    consoleEvents.push({ type: 'pageerror', error: (err && err.stack) ? err.stack : String(err) });
  });

  page.on('response', response => {
    try {
      const req = response.request();
      const url = response.url();
      const status = response.status();
      const r = { type: 'response', url: url, status: status, method: req.method(), resourceType: req.resourceType() };
      if (status >= 400) r.note = 'HTTP error';
      networkEvents.push(r);
    } catch (e) {
      networkEvents.push({ type: 'response', error: String(e) });
    }
  });

  page.on('requestfailed', req => {
    try {
      networkEvents.push({ type: 'requestfailed', url: req.url(), method: req.method(), failure: req.failure() });
    } catch (e) {
      networkEvents.push({ type: 'requestfailed', error: String(e) });
    }
  });

  page.on('request', req => {
    // record requests for visibility (lightweight)
    networkEvents.push({ type: 'request', url: req.url(), method: req.method(), resourceType: req.resourceType() });
  });

  let summary = { navigated: false };
  try {
    console.log('Opening', url);
    const resp = await page.goto(url, { waitUntil: 'networkidle2', timeout: 30000 });
    summary.navigated = true;
    summary.status = resp ? resp.status() : null;
    // wait a little for async console logs to appear
    await page.waitForTimeout(2000);
  } catch (e) {
    console.error('Navigation error:', e && e.message ? e.message : e);
    consoleEvents.push({ type: 'navigation_error', error: String(e) });
  }

  // Take a snapshot of console & network
  try {
    // extract some table cell contents for key tables
    const extracted = await page.evaluate(() => {
      function tableToArray(sel){
        const t = document.querySelector(sel); if(!t) return null;
        const rows = Array.from(t.querySelectorAll('tbody tr'));
        // use innerHTML to preserve any HTML fragments in cells
        return rows.map(r => Array.from(r.querySelectorAll('td')).map(td=> (td.innerHTML || '').trim()));
      }
      return {
        neighbors: tableToArray('#neighborsTable'),
        connections: tableToArray('#connectionsTable'),
        traceroute: tableToArray('#tracerouteTable'),
        olsrLinks: tableToArray('#olsrLinksTable')
      };
    });
    const outConsole = { events: consoleEvents, extractedTables: extracted };
    fs.writeFileSync('smoke-console.log', JSON.stringify(outConsole, null, 2));
    fs.writeFileSync('smoke-network.log', JSON.stringify(networkEvents, null, 2));
    console.log('Wrote smoke-console.log and smoke-network.log');
  } catch (e) {
    console.error('Failed writing logs:', e);
  }

  // Print a small summary
  const errors = consoleEvents.filter(x => x.type === 'pageerror' || (x.type === 'console' && (x.level === 'error' || x.level === 'warning')));
  const failedReqs = networkEvents.filter(x => x.type === 'requestfailed' || (x.type === 'response' && x.status && x.status >= 400));
  console.log('\nSummary:');
  console.log('  navigated:', summary.navigated, 'status:', summary.status);
  console.log('  console events:', consoleEvents.length);
  console.log('  JS errors / console errors:', errors.length);
  console.log('  network events recorded:', networkEvents.length);
  console.log('  failing requests / 4xx-5xx responses:', failedReqs.length);

  if (errors.length) {
    console.log('\nTop console/JS errors (first 5):');
    errors.slice(0,5).forEach((e,i) => { console.log(i+1, e); });
  }
  if (failedReqs.length) {
    console.log('\nTop network failures (first 5):');
    failedReqs.slice(0,5).forEach((r,i) => { console.log(i+1, r); });
  }

  try { await browser.close(); } catch(e){}
}

if (require.main === module) {
  const url = process.argv[2] || 'http://193.238.158.74';
  run(url).catch(err => { console.error('Fatal error:', err); process.exit(2); });
}
