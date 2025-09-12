#!/usr/bin/env node
const http = require('http');
const fetch = require('node-fetch');

async function runTest() {
  // Simple HTTP server that serves a sample /diagnostics.json
  const sample = {
    versions: { plugin_version: '1.2.3' },
    capabilities: { discover: true },
    fetch_debug: { queue_length: 0, requests: [] },
    summary: { hostname: 'test', ip: '192.0.2.1', uptime_linux: '1 day' }
  };

  const srv = http.createServer((req, res) => {
    if (req.url === '/diagnostics.json') {
      res.writeHead(200, {'Content-Type':'application/json'});
      res.end(JSON.stringify(sample));
    } else {
      res.writeHead(404); res.end('not found');
    }
  });

  await new Promise((resolve) => srv.listen(0, '127.0.0.1', resolve));
  const port = srv.address().port;
  const url = `http://127.0.0.1:${port}/diagnostics.json`;

  try {
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    const keys = ['versions','capabilities','fetch_debug','summary'];
    for (const k of keys) {
      if (!(k in j)) { throw new Error('Missing key: ' + k); }
    }
    console.log('diagnostics smoke test: PASS');
    process.exitCode = 0;
  } catch (err) {
    console.error('diagnostics smoke test: FAIL -', err.message);
    process.exitCode = 2;
  } finally {
    srv.close();
  }
}

runTest();
