// JSON polyfill for older browsers
if (!window.JSON) {
  window.JSON = {
    parse: function(s) {
      try {
        return eval('(' + s + ')');
      } catch (e) {
        throw new Error('Invalid JSON');
      }
    },
    stringify: function(obj) {
      var t = typeof obj;
      if (t !== "object" || obj === null) {
        if (t === "string") return '"' + obj + '"';
        return String(obj);
      }
      var json = [];
      var arr = (obj && obj.constructor === Array);
      for (var k in obj) {
        if (obj.hasOwnProperty && obj.hasOwnProperty(k)) {
          json.push((arr ? "" : '"' + k + '":') + window.JSON.stringify(obj[k]));
        }
      }
      return (arr ? "[" : "{") + json.join(",") + (arr ? "]" : "}");
    }
  };
}

// Object.keys polyfill for older browsers
if (!Object.keys) {
  Object.keys = function(obj) {
    if (!obj) return [];
    var keys = [];
    for (var key in obj) {
      if (obj.hasOwnProperty && obj.hasOwnProperty(key)) {
        keys.push(key);
      }
    }
    return keys;
  };
}

// Array.isArray polyfill for older browsers
if (!Array.isArray) {
  Array.isArray = function(arg) {
    return Object.prototype.toString.call(arg) === '[object Array]';
  };
}

// Promise polyfill for older browsers
if (!window.Promise) {
  window.Promise = function(executor) {
    var self = this;
    self.status = 'pending';
    self.value = undefined;
    self.reason = undefined;
    self.onFulfilled = [];
    self.onRejected = [];

    function resolve(value) {
      if (self.status === 'pending') {
        self.status = 'fulfilled';
        self.value = value;
        self.onFulfilled.forEach(function(fn){ fn(value); });
      }
    }
    function reject(reason) {
      if (self.status === 'pending') {
        self.status = 'rejected';
        self.reason = reason;
        self.onRejected.forEach(function(fn){ fn(reason); });
      }
    }
    try {
      executor(resolve, reject);
    } catch (e) {
      reject(e);
    }
  };

  window.Promise.prototype.then = function(onFulfilled, onRejected) {
    var self = this;
    return new window.Promise(function(resolve, reject) {
      function handleFulfilled(value) {
        try {
          if (typeof onFulfilled === 'function') {
            var result = onFulfilled(value);
            if (result && typeof result.then === 'function') {
              result.then(resolve, reject);
            } else {
              resolve(result);
            }
          } else {
            resolve(value);
          }
        } catch (e) { reject(e); }
      }
      function handleRejected(reason) {
        try {
          if (typeof onRejected === 'function') {
            var result = onRejected(reason);
            if (result && typeof result.then === 'function') {
              result.then(resolve, reject);
            } else {
              resolve(result);
            }
          } else {
            reject(reason);
          }
        } catch (e) { reject(e); }
      }
      if (self.status === 'fulfilled') { handleFulfilled(self.value); }
      else if (self.status === 'rejected') { handleRejected(self.reason); }
      else { self.onFulfilled.push(handleFulfilled); self.onRejected.push(handleRejected); }
    });
  };

  window.Promise.prototype.catch = function(onRejected) { return this.then(null, onRejected); };
  window.Promise.resolve = function(value) { return new window.Promise(function(resolve) { resolve(value); }); };
  window.Promise.all = function(promises) {
    return new window.Promise(function(resolve, reject) {
      var results = []; var completed = 0; var total = promises.length;
      if (total === 0) { resolve(results); return; }
      promises.forEach(function(promise, index) {
        promise.then(function(value) { results[index] = value; completed++; if (completed === total) resolve(results); }, reject);
      });
    });
  };
}

// Fetch polyfill for older browsers
if (!window.fetch) {
  window.fetch = function(url, options) {
    options = options || {};
    return new Promise(function(resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhr.open(options.method || 'GET', url, true);
      if (options.headers) {
        for (var header in options.headers) {
          xhr.setRequestHeader(header, options.headers[header]);
        }
      }
      // Add cache control header if cache option is specified
      if (options.cache === 'no-store') {
        xhr.setRequestHeader('Cache-Control', 'no-cache');
      }
      xhr.onreadystatechange = function() {
        if (xhr.readyState === 4) {
          var response = {
            ok: xhr.status >= 200 && xhr.status < 300,
            status: xhr.status,
            statusText: xhr.statusText,
            text: function() { return Promise.resolve(xhr.responseText); },
            json: function() { return Promise.resolve(JSON.parse(xhr.responseText)); }
          };
          if (response.ok) {
            resolve(response);
          } else {
            reject(new Error('HTTP ' + xhr.status));
          }
        }
      };
      xhr.onerror = function() { reject(new Error('Network error')); };
      xhr.send(options.body);
    });
  };
}

// Footer pending-request counter: show the number of in-flight fetches
(function(){
  var _pendingFetches = 0;
  function updateFooterPending() {
    try {
  var el = document.getElementById('footer-pending-count');
      if (!el) return;
      el.textContent = String(_pendingFetches);
  var badge = el;
  if (_pendingFetches > 0) badge.classList.add('nonzero'); else badge.classList.remove('nonzero');
    } catch(e) {}
  }

  var origFetch = window.fetch;
  window.fetch = function(url, opts) {
    try { _pendingFetches++; updateFooterPending(); } catch(e) {}
    return origFetch.apply(this, arguments).then(function(resp){
      try { _pendingFetches = Math.max(0,_pendingFetches-1); updateFooterPending(); } catch(e) {}
      return resp;
    }).catch(function(err){
      try { _pendingFetches = Math.max(0,_pendingFetches-1); updateFooterPending(); } catch(e) {}
      throw err;
    });
  };
})();

// RTT / latency measurement for footer display
(function(){
  var lastRtt = null;
  var pingPathCandidates = ['/status/ping','/status/lite'];
  function updateLatencyText(rtt){
    try{
      var el = document.getElementById('footer-latency');
      if(!el) return;
      if(typeof rtt !== 'number' || isNaN(rtt)) { el.textContent = 'RTT: -- ms'; el.className = ''; return; }
      var txt = 'RTT: ' + String(Math.round(rtt)) + ' ms';
      el.textContent = txt;
      // small heuristic thresholds
      el.className = '';
      if (rtt < 120) el.classList.add('footer-latency-ok');
      else if (rtt < 400) el.classList.add('footer-latency-warn');
      else el.classList.add('footer-latency-bad');
    }catch(e){}
  }

  function tryPingPath(idx){
    if (idx >= pingPathCandidates.length) return Promise.reject(new Error('no ping path'));
    var p = pingPathCandidates[idx];
    var t0 = Date.now();
    return fetch(p, {cache:'no-store', method:'GET'}).then(function(r){
      var t1 = Date.now();
      if (!r || !r.ok) {
        // try next candidate
        return tryPingPath(idx+1);
      }
      lastRtt = t1 - t0;
      updateLatencyText(lastRtt);
      return lastRtt;
    }).catch(function(){
      return tryPingPath(idx+1);
    });
  }

  function pingOnce(){ tryPingPath(0).catch(function(){ updateLatencyText(null); }); }

  // Start periodic ping and run once immediately after DOM ready
  document.addEventListener('DOMContentLoaded', function(){ pingOnce(); window.setInterval(pingOnce, 10000); });
})();

window.refreshTab = function(id, url) {
  var el = document.getElementById(id);
  if (el) el.textContent = 'Loading…';
  fetch(url, {cache:"no-store"}).then(function(r){
    return r.text().then(function(t){
      if (!r.ok) {
        el.textContent = "HTTP " + r.status + "\n" + t;
        return;
      }
      el.textContent = t;
    });
  }).catch(function(e){ el.textContent = "ERR: "+e; });
};

// UI debug flag (set true in console to enable debug logs)
// Default to false during normal operation to avoid verbose console output.
// You can enable diagnostics by setting `window._uiDebug = true` in the browser console.
if (typeof window._uiDebug === 'undefined') window._uiDebug = false;

function setText(id, text) {
  var el = document.getElementById(id);
  if (el) {
    if (typeof el.textContent !== 'undefined') {
      el.textContent = text;
    } else if (typeof el.innerText !== 'undefined') {
      el.innerText = text;
    } else {
      el.innerHTML = text;
    }
    // If we're setting the hostname, attempt to replace the literal token 'nodename'
    // with the real nodename from the cached nodedb (if available). This ensures
    // any codepath that writes the hostname benefits from enrichment.
    try {
      // Replace 'nodename' token when setting hostname or the page-level host element
      // Note: brand element 'main-host' has been repurposed to show compact IP, do not alter it here.
      if ((id === 'hostname' || id === 'main-host-page') && text && typeof text === 'string' && /\bnodename\b/.test(text)) {
        var ip = '';
        var ipEl = document.getElementById('ip');
        if (ipEl && ipEl.textContent) ip = ipEl.textContent.trim();
        if (!ip) {
          // fallback: try to extract first token from nav-host which usually starts with IP
          var nav = document.getElementById('nav-host');
          if (nav && nav.textContent) {
            var tok = nav.textContent.trim().split(/\s+/)[0];
            if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(tok)) ip = tok;
          }
        }
        if (ip && window._nodedb_cache && window._nodedb_cache[ip] && window._nodedb_cache[ip].n) {
          var real = window._nodedb_cache[ip].n;
          if (real) {
            // only replace whole-word occurrences of 'nodename'
            var newText = text.replace(/\bnodename\b/g, real);
            // write back preserving textContent/innerText/innerHTML whichever was used
            if (typeof el.textContent !== 'undefined') el.textContent = newText;
            else if (typeof el.innerText !== 'undefined') el.innerText = newText;
            else el.innerHTML = newText;
          }
        }
      }
    } catch (e) { /* ignore enrichment errors */ }
  }
}

function setHTML(id, html) {
  var el = document.getElementById(id);
  if (el) el.innerHTML = html;
}

// Robustly fetch /status/lite and parse the last top-level JSON object from the
// response text. Returns a Promise that resolves to the parsed object or
// rejects on network/error conditions.
function safeFetchStatus(fetchOptionsOrText) {
  // If caller provided a pre-fetched text (string), parse it directly and
  // avoid making a network request. Otherwise treat the argument as fetch
  // options and perform a fetch to /status/lite.
  function parseStatusText(text) {
    // Try a direct parse first
    try { return JSON.parse(text); } catch (e) {
      // Fallback: extract all complete top-level JSON objects/arrays and
      // choose the most appropriate candidate.
      function extractAllTopLevelJSON(s) {
        var out = [];
        if (!s || !s.length) return out;
        var len = s.length;
        for (var i = 0; i < len; i++) {
          var ch = s[i];
          if (ch !== '{' && ch !== '[') continue;
          var open = ch; var close = (open === '{') ? '}' : ']';
          var depth = 1;
          for (var j = i + 1; j < len; j++) {
            var cj = s[j];
            if (cj === open) depth++;
            else if (cj === close) depth--;
            if (depth === 0) {
              var cand = s.substring(i, j + 1);
              try {
                var obj = JSON.parse(cand);
                out.push(obj);
              } catch (err) {
                // ignore parse errors for this candidate
              }
              i = j; // advance outer loop past this object
              break;
            }
          }
        }
        return out;
      }

      try {
        var candidates = extractAllTopLevelJSON(String(text || ''));
        if (!candidates || candidates.length === 0) throw new Error('Invalid JSON from /status/lite');
        if (candidates.length === 1) return candidates[0];
        // Prefer a candidate that looks like the full status object
        var preferredKeys = ['hostname','devices','links','fetch_stats','ip','uptime'];
        var bestCandidate = null;
        var bestScore = 0;
        for (var k = candidates.length - 1; k >= 0; k--) {
          var c = candidates[k];
          if (c && typeof c === 'object') {
            var score = 0;
            for (var pi = 0; pi < preferredKeys.length; pi++) {
              if (typeof c[preferredKeys[pi]] !== 'undefined') score++;
            }
            if (score > bestScore) {
              bestScore = score;
              bestCandidate = c;
            }
          }
        }
        if (bestCandidate) return bestCandidate;
        // If all candidates look like device entries, wrap as devices array
        var allDevices = true;
        for (var ii = 0; ii < candidates.length; ii++) {
          var cc = candidates[ii];
          if (!cc || typeof cc !== 'object') { allDevices = false; break; }
          if (typeof cc.ipv4 === 'undefined' && typeof cc.hwaddr === 'undefined' && typeof cc.ip === 'undefined') { allDevices = false; break; }
        }
        if (allDevices) return { devices: candidates };
        // Fallback: return the last candidate
        return candidates[candidates.length - 1];
      } catch (err) {
        throw new Error('Invalid JSON from /status/lite');
      }
    }
  }

  // If a string was provided, parse immediately
  if (typeof fetchOptionsOrText === 'string') {
    return new Promise(function(resolve, reject){
      try { resolve(parseStatusText(fetchOptionsOrText)); } catch (e) { reject(e); }
    });
  }

  var fetchOptions = fetchOptionsOrText || {cache:'no-store'};
  // Perform the network fetch when no pre-fetched text is available
  return fetch('/status/lite', fetchOptions).then(function(r){
    if (!r || !r.ok) throw new Error('HTTP ' + (r && r.status));
    return r.text();
  }).then(function(text){
    return parseStatusText(text);
  });
}

// Resolve a node's human name from the loaded nodedb using an IP or CIDR.
function getNodeNameForIp(ip) {
  try {
    if (!ip) return null;
    if (!window._nodedb_cache) return null;
    function ip2int(ipstr){ return ipstr.split('.').reduce(function(acc,x){return (acc<<8)+parseInt(x,10);},0) >>>0; }
    var rip = null;
    if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(ip)) rip = ip2int(ip);
    var best = null;
    Object.keys(window._nodedb_cache).forEach(function(k){
      if (!k) return;
      var v = window._nodedb_cache[k];
      if (!v) return;
      // exact match
      if (k === ip) { best = v.n || best; return; }
      // direct host field match
      if (v.h && v.h === ip) { best = v.n || best; return; }
      // CIDR match
      if (k.indexOf('/') > -1 && rip !== null) {
        try {
          var parts = k.split('/'); var net = parts[0]; var bits = parseInt(parts[1],10);
          var nint = ip2int(net);
          var mask = bits===0?0: (~0 << (32-bits)) >>>0;
          if ((rip & mask) === (nint & mask)) { best = v.n || best; return; }
        } catch(e) {}
      }
    });
    return best || null;
  } catch(e) { return null; }
}

/* Modal helpers: manage aria-hidden and visibility centrally */
function showModal(id) {
  var m = document.getElementById(id);
  if (!m) return;
  try { m.setAttribute('aria-hidden','false'); } catch(e) {}
  try { m.style.display = ''; } catch(e) {}
}
function hideModal(id) {
  var m = document.getElementById(id);
  if (!m) return;
  try { m.setAttribute('aria-hidden','true'); } catch(e) {}
  try { m.style.display = 'none'; } catch(e) {}
}

// Modal keyboard handling: Esc to close and focus trap within modal-panel
window._modalKeyHandler = null;
function _setupModalKeyboard(modalEl) {
  try {
    if (!modalEl) return;
    // remove existing handler if any
    if (window._modalKeyHandler) { document.removeEventListener('keydown', window._modalKeyHandler); window._modalKeyHandler = null; }
    var panel = modalEl.querySelector('.modal-panel');
    var focusableSelector = 'a[href], area[href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), button:not([disabled]), [tabindex]:not([tabindex="-1"])';
    window._modalKeyHandler = function(e){
      if (e.key === 'Escape' || e.key === 'Esc') { hideModal(modalEl.id); return; }
      if (e.key === 'Tab') {
        // focus trap
        var nodes = panel ? Array.prototype.slice.call(panel.querySelectorAll(focusableSelector)) : [];
        if (!nodes || nodes.length === 0) return;
        var first = nodes[0], last = nodes[nodes.length-1];
        if (e.shiftKey) {
          if (document.activeElement === first) { last.focus(); e.preventDefault(); }
        } else {
          if (document.activeElement === last) { first.focus(); e.preventDefault(); }
        }
      }
    };
    document.addEventListener('keydown', window._modalKeyHandler);
    // focus first focusable element if present
    try { var first = panel.querySelector(focusableSelector); if (first) first.focus(); } catch(e){}
  } catch(e) {}
}

// Tab status indicator functions
function updateTabStatus(tabId, status) {
  var statusEl = document.getElementById(tabId + '-status');
  if (statusEl) {
    statusEl.className = 'tab-status ' + status;
    if (status === 'loaded') {
      statusEl.textContent = '✓';
    } else if (status === 'loading') {
      statusEl.textContent = '⟳';
    } else if (status === 'error') {
      statusEl.textContent = '✗';
    }
  }
}

function setTabLoading(tabId) {
  updateTabStatus(tabId, 'loading');
}

function setTabLoaded(tabId) {
  updateTabStatus(tabId, 'loaded');
}

function setTabError(tabId) {
  updateTabStatus(tabId, 'error');
}

function showTab(tabId, show) {
  var el = document.getElementById(tabId);
  if (!el) return;

  // Find the corresponding nav link
    var navLink = document.querySelector('#mainTabs a[href="#' + tabId + '"]');
  if (navLink) {
    var navItem = navLink.parentElement;
    if (show) {
      navItem.style.display = '';
      el.style.display = 'block';
      el.classList.remove('hidden');
    } else {
      navItem.style.display = 'none';
      el.style.display = 'none';
      el.classList.add('hidden');
    }
  }

  // diagnostics: log current state for debugging empty-tab issue
  try {
    if (window._uiDebug) console.debug('showTab called for', tabId, 'show=', !!show, 'el.style.display=', el.style.display);
  } catch(e){}
}

function populateDevicesTable(devices, airos) {
  var tbody = document.querySelector('#devicesTable tbody');
  // diagnostics: lightweight logging to help trace empty tabs issue
  try { if (window._uiDebug) console.debug('populateDevicesTable called, devices=', (devices && devices.length) || 0, 'tbodyExists=', !!tbody, 'tbodyClass=', tbody?tbody.className:'-'); } catch(e) {}
  tbody.innerHTML = '';
  // Ensure parent pane is not accidentally hidden (help with intermittent empty-tab issue)
  try { var pane = document.getElementById('tab-status'); if (pane) { pane.classList.remove('hidden'); pane.style.display = ''; } } catch(e) {}
  // Note: we no longer filter out ARP-like entries here. Instead compute a
  // `source` field per device and render it in the new Source column so users
  // can distinguish devices originating from ubnt-discover vs ARP vs unknown.
  // Determine a single-entry source tag for an incoming device object.
  // Returns 'ubnt-discover', 'arp' or 'unknown' for a single raw record.
  function detectSource(dev) {
    if (!dev) return 'unknown';
    try {
      var hasProduct = (dev.product || '').toString().trim().length > 0;
      var hasHostname = (dev.hostname || '').toString().trim().length > 0;
      var hasExtras = (dev.essid || dev.firmware || dev.uptime || dev.mode || dev.signal);
      if (hasProduct || hasHostname) return 'ubnt-discover';
      if (dev.hwaddr && !hasExtras && !hasProduct && !hasHostname) return 'arp';
    } catch(e) {}
    return 'unknown';
  }

  // If incoming devices is empty but we have a cached view, keep showing the cached devices
  if (!devices || !Array.isArray(devices) || devices.length === 0) {
    try { if (window._devices_data && Array.isArray(window._devices_data) && window._devices_data.length > 0) { devices = window._devices_data.slice(); } } catch(e) {}
  }
  if (!devices || !Array.isArray(devices) || devices.length === 0) {
    var tbody = document.querySelector('#devicesTable tbody');
    if (tbody) tbody.innerHTML = '<tr><td colspan="12" class="text-muted">No devices found</td></tr>';
    return;
  }
  // Ensure we expose a source and wireless property on each device so sorting
  // and display works consistently.
  var warn_frequency = 0;

  // Merge duplicates produced by ARP + ubnt-discover: prefer richer fields and
  // combine source indicators (e.g. 'arp, ubnt-discover'). We merge by
  // hardware address when present, otherwise by IPv4 address.
  try {
    var mergedMap = Object.create(null);
    devices.forEach(function(dev) {
      var key = null;
      if (dev.hwaddr) key = String(dev.hwaddr).toLowerCase();
      else if (dev.ipv4) key = String(dev.ipv4);
      else if (dev.ip) key = String(dev.ip);
      else key = String(dev.hostname || ('_anon_' + Math.random().toString(36).slice(2,8)));

      if (!mergedMap[key]) {
        // shallow copy so we don't mutate original source objects
        var copy = {};
        for (var k in dev) if (dev.hasOwnProperty(k)) copy[k] = dev[k];
        copy.sources = new Set();
        copy.sources.add(detectSource(dev));
        mergedMap[key] = copy;
      } else {
        var cur = mergedMap[key];
        // prefer non-empty values from incoming record
        ['ipv4','ip','hwaddr','hostname','product','uptime','mode','essid','firmware','signal','tx_rate','rx_rate'].forEach(function(f){
          try { if ((!cur[f] || String(cur[f]).trim() === '') && dev[f] && String(dev[f]).trim() !== '') cur[f] = dev[f]; } catch(e) {}
        });
        cur.sources.add(detectSource(dev));
      }
    });
    // rebuild devices array from mergedMap
    var mergedDevices = [];
    Object.keys(mergedMap).forEach(function(k){
      var item = mergedMap[k];
      try { item.source = Array.from(item.sources).sort().filter(function(x){return x && x!=='unknown';}).join(', '); } catch(e){ item.source = 'unknown'; }
      delete item.sources;
      mergedDevices.push(item);
    });
    devices = mergedDevices;
  } catch(e) { /* if merging fails, fall back to original devices array */ }

  // Display all devices provided by the backend (e.g. ubnt discover output)
  devices.forEach(function(device) {
    var tr = document.createElement('tr');
    function td(val) { var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    // Local IP with HW Address (smaller font)
    var localIpCell = '<div style="font-size:60%">' + (device.ipv4 || '') + '<br>' + (device.hwaddr || '') + '</div>';
    var localIpTd = document.createElement('td');
    localIpTd.innerHTML = localIpCell;
    tr.appendChild(localIpTd);
    tr.appendChild(td(device.hostname));
    tr.appendChild(td(device.product));
    tr.appendChild(td(device.uptime));
    tr.appendChild(td(device.mode));
    tr.appendChild(td(device.essid));
  tr.appendChild(td(device.firmware));
  tr.appendChild(td(device.signal || ''));
  tr.appendChild(td(device.tx_rate || ''));
  tr.appendChild(td(device.rx_rate || ''));
    var wireless = '';
    var freq_start = null, freq_end = null, frequency = null, chanbw = null;
    if (airos && airos[device.ipv4] && airos[device.ipv4].wireless) {
      var w = airos[device.ipv4].wireless;
      frequency = parseInt((w.frequency || '').replace('MHz','').trim());
      chanbw = parseInt(w.chanbw || 0);
      if (frequency && chanbw) {
        freq_start = frequency - (chanbw/2);
        freq_end = frequency + (chanbw/2);
        if (freq_start < 5490 && freq_end > 5710) warn_frequency = 1;
      }
      wireless = (w.frequency ? w.frequency + 'MHz ' : '') + (w.mode || '');
    }
  // persist wireless string on device for sorting
  device.wireless = wireless;
  // compute and persist source for display/sorting
  // If merging ran above it already set device.source; otherwise detect from single record
  if (!device.source) {
    try { device.source = detectSource(device); } catch(e) { device.source = 'unknown'; }
  }
  tr.appendChild(td(wireless));
  // create Source cell with colored dot + label (for visual clarity)
  var srcTd = document.createElement('td');
  var srcWrap = document.createElement('span');
  // sanitize source string into a safe class name (comma/space -> single hyphen, remove other chars)
  var scls = 'device-source src-' + (device.source ? device.source.toLowerCase().replace(/[^a-z0-9]+/gi,'-').replace(/-+/g,'-').replace(/^-|-$/g,'') : 'unknown');
  srcWrap.className = scls;
  var dot = document.createElement('span'); dot.className = 'dot';
  var lbl = document.createElement('span'); lbl.className = 'lbl small'; lbl.innerText = device.source || 'unknown';
  srcWrap.appendChild(dot); srcWrap.appendChild(lbl); srcTd.appendChild(srcWrap); tr.appendChild(srcTd);
  tbody.appendChild(tr);
  });
  var table = document.getElementById('devicesTable');
  if (warn_frequency && table) {
    var warn = document.createElement('div');
    warn.className = 'alert alert-warning';
    warn.innerHTML = 'Warnung: Frequenzüberlappung erkannt!';
    table.parentNode.insertBefore(warn, table);
  }
  // attach header sort clicks to sort the provided devices array and re-render
  try {
    var headers = document.querySelectorAll('#devicesTable thead th');
    headers.forEach(function(h, idx){
      // Allow clicking headers to sort by inferred keys (map index to property)
      h.style.cursor = 'pointer';
      h.onclick = function(){
        var key = null;
        switch(idx) {
          case 0: key = 'ipv4'; break;
          case 1: key = 'hostname'; break;
          case 2: key = 'product'; break;
          case 3: key = 'uptime'; break;
          case 4: key = 'mode'; break;
          case 5: key = 'essid'; break;
          case 6: key = 'firmware'; break;
          case 7: key = 'signal'; break;
          case 8: key = 'tx_rate'; break;
          case 9: key = 'rx_rate'; break;
    case 10: key = 'wireless'; break;
    case 11: key = 'source'; break;
        }
        if (!key) return;
        if (_devicesSort.key === key) _devicesSort.asc = !_devicesSort.asc; else { _devicesSort.key = key; _devicesSort.asc = true; }
        try {
          var arr = window._devices_data = window._devices_data || devices.slice();
          arr.sort(function(a,b){
            var ka = a[_devicesSort.key]; var kb = b[_devicesSort.key];
            // numeric-aware fields
            if (_devicesSort.key === 'signal' || _devicesSort.key === 'tx_rate' || _devicesSort.key === 'rx_rate') {
              var an = parseFloat(String(ka||'').replace(/[^0-9\.\-]/g,'')); var bn = parseFloat(String(kb||'').replace(/[^0-9\.\-]/g,''));
              if (!isNaN(an) && !isNaN(bn)) return _devicesSort.asc ? (an - bn) : (bn - an);
            }
            ka = String(ka||'').toLowerCase(); kb = String(kb||'').toLowerCase(); if (ka < kb) return _devicesSort.asc?-1:1; if (ka>kb) return _devicesSort.asc?1:-1; return 0;
          });
          populateDevicesTable(arr, airos);
        } catch(e) { populateDevicesTable(devices, airos); }
      };
    });
  } catch(e){}
}

function populateOlsrLinksTable(links) {
  var tbody = document.querySelector('#olsrLinksTable tbody');
  if (!tbody) return; tbody.innerHTML = '';
  // diagnostics: lightweight logging to help trace empty tabs issue
  try { if (window._uiDebug) console.debug('populateOlsrLinksTable called, links=', (links && links.length) || 0, 'tbodyExists=', !!tbody, 'tbodyClass=', tbody?tbody.className:'-'); } catch(e) {}
  if (!links || !Array.isArray(links) || links.length === 0) {
    var tbody = document.querySelector('#olsrLinksTable tbody');
  if (tbody) tbody.innerHTML = '<tr><td colspan="12" class="text-muted">No links found</td></tr>';
    return;
  }
  // always refresh cached links to reflect latest server data
  try { window._olsr_links = Array.isArray(links) ? links.slice() : (links || []); } catch(e) { window._olsr_links = links || []; }
  links.forEach(function(l){
    var tr = document.createElement('tr');
    if (l.is_default) { tr.style.backgroundColor = '#fff8d5'; }
    function td(val){ var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(l.intf));
    tr.appendChild(td(l.local));
    tr.appendChild(td(l.remote));
    // show remote_host only when present and not identical to remote IP
    try {
      if (l.remote_host && String(l.remote_host).trim() && String(l.remote_host).trim() !== String(l.remote).trim()) {
        var linkHtml = '<a target="_blank" href="https://' + l.remote_host + '">' + l.remote_host + '</a>';
        tr.appendChild(td(linkHtml));
      } else {
        tr.appendChild(td(''));
      }
    } catch(e){ tr.appendChild(td(l.remote_host || '')); }
  // attempt to resolve a primary node name for the remote IP using cached nodedb
  var nodeName = '';
  try { nodeName = getNodeNameForIp(l.remote) || ''; } catch(e) { nodeName = ''; }
  // prefer explicit primary name from node_names (first entry) otherwise resolved name
  var primary = nodeName || '';
  try { if (l.node_names && typeof l.node_names === 'string') { var parts = l.node_names.split(',').map(function(s){return s.trim();}).filter(function(x){return x.length}); if (parts.length) primary = parts[0]; } } catch(e) {}
  var nodeTd = td(primary);
  if (l.node_names) { nodeTd.title = l.node_names; }
  tr.appendChild(nodeTd);
    // LQ / NLQ as numeric badges
    // LQ / NLQ as numeric badges
    // Treat values >= 1.0 as good (green). Values below 1.0 become progressively more red.
    function mkLqBadge(val){
      var v = parseFloat(String(val||'')||'0');
      var cls = 'lq-high'; // default: bad (red)
      if (!isFinite(v) || v <= 0) {
        cls = 'lq-high';
      } else if (v >= 1.0) {
        cls = 'lq-low'; // green
      } else if (v >= 0.85) {
        cls = 'lq-med'; // amber
      } else {
        cls = 'lq-high'; // red
      }
      return '<span class="lq-badge '+cls+'">'+String(val)+'</span>';
    }
    tr.appendChild(td(mkLqBadge(l.lq)));
    tr.appendChild(td(mkLqBadge(l.nlq)));
    // cost numeric rendering
    var costVal = l.cost || '';
    var costNum = parseFloat(String(costVal).replace(/[^0-9\.\-]/g,''));
    var costHtml = '<span class="metric-badge small" style="background:#888;">'+(costVal||'')+'</span>';
    if (!isNaN(costNum)) {
      costHtml = '<span class="metric-badge small" style="background:#5a9bd8;">'+costNum+'</span>';
    }
    tr.appendChild(td(costHtml));
    // ETX rendering: prefer explicit etx field, otherwise compute from cost/lq where possible
    try {
      var etxVal = null;
      if (typeof l.etx !== 'undefined' && l.etx !== null && l.etx !== '') etxVal = parseFloat(String(l.etx).replace(/[^0-9\.\-]/g,''));
      else if (l.cost) {
        var cnum = parseFloat(String(l.cost).replace(/[^0-9\.\-]/g,''));
        if (!isNaN(cnum) && cnum > 0) {
          etxVal = (cnum > 100 ? (cnum / 1000) : cnum);
        }
      }
      var etxHtml = '';
      if (etxVal === null || !isFinite(etxVal)) {
        etxHtml = '<span class="etx-badge etx-med">n/a</span>';
      } else {
        var etxCls = (etxVal <= 1.5) ? 'etx-good' : (etxVal <= 3.0 ? 'etx-med' : 'etx-bad');
        etxHtml = '<span class="etx-badge '+etxCls+'">'+(Math.round(etxVal*100)/100) +'</span>';
      }
      tr.appendChild(td(etxHtml));
    } catch(e) { tr.appendChild(td('')); }
    var routesCell = td(l.routes || '');
    if (l.routes && parseInt(l.routes,10) > 0) {
      routesCell.style.cursor='pointer'; routesCell.title='Click to view routes via this neighbor';
      routesCell.addEventListener('click', function(){ showRoutesFor(l.remote); });
    }
    // incorporate small metric badges into routes cell (avoid an extra column that shifts nodes)
    try {
      var metricsInline = '';
      metricsInline += '<span class="metric-badge routes small" style="margin-left:6px;">R:'+ (l.routes || '0') +'</span>';
      routesCell.innerHTML = (routesCell.innerHTML || '') + metricsInline;
    } catch(e){}
    tr.appendChild(routesCell);
    // nodes column: show numeric count and tooltip with names (avoid duplicating long text in table)
    var nodeCount = '';
    try { nodeCount = (typeof l.nodes !== 'undefined' && l.nodes !== null) ? String(l.nodes) : ''; } catch(e) { nodeCount = ''; }
    var nodesCell = td(nodeCount);
    if (l.node_names) {
      nodesCell.title = l.node_names;
      // make nodes clickable when there are nodes
      if (parseInt(nodeCount,10) > 0) { nodesCell.style.cursor = 'pointer'; nodesCell.title = l.node_names; nodesCell.addEventListener('click', function(){ showNodesFor(l.remote, l.node_names); }); }
    }
  // metric badges (sparklines removed for stability)
  // nodes column (detailed node names + click handler)
  tr.appendChild(nodesCell);
  // Actions column
  var actHtml = '';
  if (l.remote_host) actHtml += '<button class="btn btn-xs btn-default action-open-host" data-host="'+encodeURIComponent(l.remote_host)+'" title="Open remote host">Open</button> ';
  if (l.routes && parseInt(l.routes,10)>0) actHtml += '<button class="btn btn-xs btn-primary action-show-routes" data-remote="'+encodeURIComponent(l.remote)+'">Routes</button> ';
  if (l.nodes && parseInt(l.nodes,10)>0) actHtml += '<button class="btn btn-xs btn-info action-show-nodes" data-remote="'+encodeURIComponent(l.remote)+'" data-names="'+(l.node_names?encodeURIComponent(l.node_names):'')+'">Nodes</button>';
  var actTd = td(actHtml);
  tr.appendChild(actTd);
    tbody.appendChild(tr);
  });
  // after populating, ensure the table is visible when inside an active tab (avoid race where CSS hides it)
  try { var pane = document.getElementById('tab-olsr'); if (pane && pane.classList.contains('active')) { /* no-op but forces style recalculation in some browsers */ pane.style.display = ''; setTimeout(function(){ pane.style.display = ''; }, 10); } } catch(e) {}
  // Also proactively remove any hidden class from the OLSR pane so content is visible
  try { var pane2 = document.getElementById('tab-olsr'); if (pane2) { pane2.classList.remove('hidden'); pane2.style.display = ''; } } catch(e) {}
  // wire header sorting to re-sort the links array and re-render
  try {
    var lheaders = document.querySelectorAll('#olsrLinksTable thead th');
    lheaders.forEach(function(h, idx){
      h.style.cursor = 'pointer';
      h.onclick = function(){
        var key = null;
        switch(idx) {
          case 0: key='intf'; break;
          case 1: key='local'; break;
          case 2: key='remote'; break;
          case 3: key='remote_host'; break;
          case 4: key='remote_node'; break;
          case 5: key='lq'; break;
          case 6: key='nlq'; break;
          case 7: key='cost'; break;
          case 8: key='etx'; break;
          case 9: key='routes'; break;
          case 10: key='nodes'; break;
          case 11: key='actions'; break;
        };
        if (!key) return;
        if (_olsrSort.key === key) _olsrSort.asc = !_olsrSort.asc; else { _olsrSort.key = key; _olsrSort.asc = true; }
        // sort the cached links array and re-render
        try {
          var arr = window._olsr_links = window._olsr_links || links.slice();
          arr.sort(function(a,b){
            var ka = a[_olsrSort.key]; var kb = b[_olsrSort.key];
            // numeric compare for certain keys
          if (_olsrSort.key === 'lq' || _olsrSort.key === 'nlq' || _olsrSort.key === 'cost' || _olsrSort.key === 'routes' || _olsrSort.key === 'nodes' || _olsrSort.key === 'etx') {
              var an = parseFloat(String(ka||'').replace(/[^0-9\.\-]/g,'')); var bn = parseFloat(String(kb||'').replace(/[^0-9\.\-]/g,''));
              if (!isNaN(an) && !isNaN(bn)) return _olsrSort.asc ? (an - bn) : (bn - an);
            }
            ka = String(ka||'').toLowerCase(); kb = String(kb||'').toLowerCase(); if (ka < kb) return _olsrSort.asc?-1:1; if (ka>kb) return _olsrSort.asc?1:-1; return 0;
          });
          populateOlsrLinksTable(arr);
        } catch(e) { try{ populateOlsrLinksTable(links); } catch(_){} }
      };
    });
  } catch(e){}

  // Use event delegation on the table body for action buttons to avoid accumulating handlers
  try {
    tbody.onclick = function(e){
      var target = e.target || e.srcElement;
      // handle button or inner icon
      var btn = target.closest ? target.closest('button') : (target.nodeName==='BUTTON' ? target : null);
      if (!btn) return;
      if (btn.classList.contains('action-open-host')) {
        var h = decodeURIComponent(btn.getAttribute('data-host')||''); if (h) window.open('https://'+h, '_blank');
      } else if (btn.classList.contains('action-show-routes')) {
        var r = decodeURIComponent(btn.getAttribute('data-remote')||''); if (r) showRoutesFor(r);
      } else if (btn.classList.contains('action-show-nodes')) {
        var r = decodeURIComponent(btn.getAttribute('data-remote')||''); var names = decodeURIComponent(btn.getAttribute('data-names')||''); if (r) showNodesFor(r, names);
      }
    };
  } catch(e) {}
}

function showNodesFor(remoteIp, nodeNames) {
  var modal = document.getElementById('node-modal'); if (!modal) return;
  var bodyPre = document.getElementById('node-modal-body');
  var title = document.getElementById('node-modal-title');
  var countBadge = document.getElementById('node-modal-count');
  var tbody = document.getElementById('node-modal-tbody');
  var filterInput = document.getElementById('node-filter');
  var copyBtn = document.getElementById('node-copy');
  if (title) title.textContent = 'Nodes via ' + remoteIp;
  if (countBadge) { countBadge.style.display='none'; countBadge.textContent=''; }
  if (tbody) tbody.innerHTML = '<tr><td colspan="6" class="text-muted">Loading...</td></tr>';
  if (bodyPre) { bodyPre.style.display='none'; bodyPre.textContent='Loading...'; }

  // sorting state for node modal
  var _nodeSort = { key: null, asc: true };
  function renderRows(list) {
    if (!tbody) return;
    if (!list.length) { tbody.innerHTML = '<tr><td colspan="7" class="text-muted">No nodes found</td></tr>'; return; }
    var arr = list.slice();
    if (_nodeSort.key) {
      arr.sort(function(a,b){
        var ka = (a[_nodeSort.key]||'').toString().toLowerCase();
        var kb = (b[_nodeSort.key]||'').toString().toLowerCase();
        if (ka < kb) return _nodeSort.asc ? -1 : 1;
        if (ka > kb) return _nodeSort.asc ? 1 : -1;
        return 0;
      });
    }
    var html = '';
    for (var i=0;i<arr.length;i++) {
      var n = arr[i];
      var rowId = 'node-row-' + i;
      html += '<tr id="'+rowId+'">'+
        '<td style="font-family:monospace">'+ (n.ip||'') +'</td>'+
        '<td>' + (n.n || '') + '</td>'+
        '<td>' + (n.i || '') + '</td>'+
        '<td>' + (n.d || '') + '</td>'+
        '<td>' + (n.h || '') + '</td>'+
        '<td>' + (n.m || '') + '</td>'+
        '<td>' +
          '<button class="btn btn-xs btn-default node-copy-row" data-idx="'+i+'" title="Copy row"><span class="glyphicon glyphicon-copy" aria-hidden="true"></span></button> '+
          '<button class="btn btn-xs btn-default node-expand-row" data-idx="'+i+'" title="Show details"><span class="glyphicon glyphicon-resize-full" aria-hidden="true"></span></button>'+
        '</td>'+
        '</tr>';
    }
    tbody.innerHTML = html;
    // attach per-row handlers
    var copies = document.querySelectorAll('.node-copy-row');
    copies.forEach(function(btn){ btn.addEventListener('click', function(){ var idx = parseInt(btn.getAttribute('data-idx'),10); var item = arr[idx]; try{ var txt = JSON.stringify(item); if(navigator.clipboard && navigator.clipboard.writeText) { navigator.clipboard.writeText(txt); btn.classList.add('btn-success'); setTimeout(function(){ btn.classList.remove('btn-success'); },900);} else { var ta=document.createElement('textarea'); ta.value=txt; document.body.appendChild(ta); ta.select(); document.execCommand('copy'); document.body.removeChild(ta);} }catch(e){} }); });
    var expands = document.querySelectorAll('.node-expand-row');
    expands.forEach(function(btn){ btn.addEventListener('click', function(){ var idx = parseInt(btn.getAttribute('data-idx'),10); var item = arr[idx]; try{ if (bodyPre) { bodyPre.style.display='block'; bodyPre.textContent = JSON.stringify(item, null, 2); bodyPre.scrollTop = 0; } // highlight row
          var prev = document.querySelector('#node-modal-tbody tr.success'); if (prev) prev.classList.remove('success'); var row = document.getElementById('node-row-'+idx); if (row) row.classList.add('success'); }catch(e){} }); });
  }

  function applyFilter(list){
    var q = (filterInput && filterInput.value || '').trim().toLowerCase();
    if (!q) { renderRows(list); return; }
    var f = list.filter(function(n){
      return (n.ip && n.ip.toLowerCase().indexOf(q)>=0) || (n.n && n.n.toLowerCase().indexOf(q)>=0) || (n.d && n.d.toLowerCase().indexOf(q)>=0) || (n.h && n.h.toLowerCase().indexOf(q)>=0);
    });
    renderRows(f);
  }

  if (filterInput) filterInput.oninput = function(){ if (window._nodedb_cache_list) applyFilter(window._nodedb_cache_list); };
  if (copyBtn) copyBtn.onclick = function(){ try{ var visible = []; var rows = document.querySelectorAll('#node-modal-tbody tr'); for(var i=0;i<rows.length;i++){ visible.push(rows[i].textContent.trim()); } var txt = visible.join('\n'); if(navigator.clipboard && navigator.clipboard.writeText){ navigator.clipboard.writeText(txt); copyBtn.classList.add('btn-success'); setTimeout(function(){ copyBtn.classList.remove('btn-success'); },900);} else { var ta=document.createElement('textarea'); ta.value=txt; document.body.appendChild(ta); ta.select(); document.execCommand('copy'); document.body.removeChild(ta); } } catch(e){} };

  // use cached nodedb if present, otherwise fetch
  function findNodes(nodedb) {
    var list = [];
    if (!nodedb) return list;
    // nodedb may be object keyed by ip/net -> value { n,i,d,h,m }
    Object.keys(nodedb).forEach(function(k){
      var v = nodedb[k];
      // match if key equals remoteIp or if key is CIDR that contains remoteIp or if node_names hint provided and matches name
      // simple checks: exact match, prefix with '/', or host field equals remoteIp
      var match = false;
      if (k === remoteIp) match = true;
      else if (k.indexOf('/')>-1) {
        // CIDR: check remoteIp in CIDR using a simple numeric compare
        try{
          var parts = k.split('/'); var net = parts[0]; var bits = parseInt(parts[1],10);
          function ip2int(ip){ return ip.split('.').reduce(function(acc,x){return (acc<<8)+parseInt(x,10);},0) >>>0; }
          var rip = ip2int(remoteIp); var nint = ip2int(net); var mask = bits===0?0: (~0 << (32-bits)) >>>0;
          if ((rip & mask) === (nint & mask)) match = true;
        }catch(e){}
      }
      if (!match && v) {
        if (v.h && v.h === remoteIp) match = true;
        if (v.m && v.m === remoteIp) match = true;
        if (nodeNames && v.n && nodeNames.indexOf(v.n) !== -1) match = true;
      }
      if (match) {
        var entry = { ip: k, n: v.n || '', i: v.i || '', d: v.d || '', h: v.h || '', m: v.m || '' };
        list.push(entry);
      }
    });
    return list;
  }

  if (window._nodedb_cache) {
    var found = findNodes(window._nodedb_cache);
    window._nodedb_cache_list = found;
    if (countBadge) { countBadge.style.display='inline-block'; countBadge.textContent = found.length; }
    renderRows(found);
  if (bodyPre) bodyPre.textContent = JSON.stringify(found, null, 2);
  showModal('node-modal');
    // wire header sort clicks
    try {
      var headers = document.querySelectorAll('#node-modal-table thead th[data-key]');
      headers.forEach(function(h){ h.onclick = function(){ var k = h.getAttribute('data-key'); if (_nodeSort.key === k) _nodeSort.asc = !_nodeSort.asc; else { _nodeSort.key = k; _nodeSort.asc = true; } renderRows(window._nodedb_cache_list || found); }; });
    } catch(e) {}
    return;
  }

  fetch('/nodedb.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ try{ window._nodedb_cache = nb || {}; var found = findNodes(nb || {}); window._nodedb_cache_list = found; if (countBadge) { countBadge.style.display='inline-block'; countBadge.textContent = found.length; } renderRows(found); if (bodyPre) bodyPre.textContent = JSON.stringify(found, null, 2); showModal('node-modal'); }catch(e){ if(tbody) tbody.innerHTML='<tr><td colspan="6" class="text-danger">Error rendering nodes</td></tr>'; if(bodyPre) bodyPre.textContent='Error'; showModal('node-modal'); } }).catch(function(){ if(tbody) tbody.innerHTML='<tr><td colspan="6" class="text-danger">Error loading nodedb.json</td></tr>'; if(bodyPre) bodyPre.textContent='Error loading nodedb.json'; showModal('node-modal'); });
  // attach header sort clicks for fetched path as well
  try {
    var headers = document.querySelectorAll('#node-modal-table thead th[data-key]');
    headers.forEach(function(h){ h.onclick = function(){ var k = h.getAttribute('data-key'); if (_nodeSort.key === k) _nodeSort.asc = !_nodeSort.asc; else { _nodeSort.key = k; _nodeSort.asc = true; } renderRows(window._nodedb_cache_list || []); }; });
  } catch(e) {}
}

// Cache for parsed routes from status payload
var _rawRoutesCache = null;
function ensureRawRoutes(statusObj) {
  if (_rawRoutesCache) return _rawRoutesCache;
  if (statusObj && statusObj.olsr_routes_raw && typeof statusObj.olsr_routes_raw === 'object') {
    _rawRoutesCache = statusObj.olsr_routes_raw; return _rawRoutesCache;
  }
  return null;
}

function showRoutesFor(remoteIp) {
  var modal = document.getElementById('route-modal');
  if (!modal) return;
  var bodyPre = document.getElementById('route-modal-body');
  var title = document.getElementById('route-modal-title');
  var countBadge = document.getElementById('route-modal-count');
  var tbody = document.getElementById('route-modal-tbody');
  var filterInput = document.getElementById('route-filter');
  var copyBtn = document.getElementById('route-copy');
  if (title) title.textContent = 'Routes via ' + remoteIp;
  if (countBadge) { countBadge.style.display='none'; countBadge.textContent=''; }
  if (tbody) tbody.innerHTML = '<tr><td colspan="4" class="text-muted">Loading...</td></tr>';
  if (bodyPre) { bodyPre.style.display='none'; bodyPre.textContent='Loading...'; }
  var allRoutes = [];
  var _routeSort = { key: null, asc: true };
  function renderTable(arr){
    if (!tbody) return;
    if (!arr.length) { tbody.innerHTML='<tr><td colspan="4" class="text-muted">No matching routes</td></tr>'; return; }
    var arr2 = arr.slice();
    if (_routeSort.key) {
      arr2.sort(function(a,b){ var ka=(a[_routeSort.key]||'').toString().toLowerCase(); var kb=(b[_routeSort.key]||'').toString().toLowerCase(); if(ka<kb) return _routeSort.asc?-1:1; if(ka>kb) return _routeSort.asc?1:-1; return 0; });
    }
    var html='';
    for (var i=0;i<arr2.length;i++) {
      var r = arr2[i];
      var rid = 'route-row-'+i;
      html += '<tr id="'+rid+'" title="'+ (r.raw||'') +'">'+
        '<td style="font-family:monospace">'+ r.destination +'</td>'+
        '<td>'+ (r.device||'') +'</td>'+
        '<td>'+ (r.metric||'') +'</td>'+
        '<td style="font-family:monospace; color:#666">'+ r.raw +'</td>'+
      '</tr>';
    }
    tbody.innerHTML = html;
    // make rows focusable for keyboard nav
    var rows = tbody.querySelectorAll('tr');
    rows.forEach(function(r){ r.tabIndex = 0; r.style.cursor='pointer'; });
  }
  function applyFilter(){
    var q = (filterInput && filterInput.value || '').trim().toLowerCase();
    if (!q) { renderTable(allRoutes); return; }
    var f = allRoutes.filter(function(r){ return r.raw.toLowerCase().indexOf(q)>=0; });
    renderTable(f);
  }
  if (filterInput) {
    filterInput.oninput = function(){ applyFilter(); };
  }
  if (copyBtn) {
    copyBtn.onclick = function(){
      try {
        var visible = [];
        var q = (filterInput && filterInput.value || '').trim().toLowerCase();
        for (var i=0;i<allRoutes.length;i++) {
          if (!q || allRoutes[i].raw.toLowerCase().indexOf(q)>=0) visible.push(allRoutes[i].raw);
        }
        var txt = visible.join('\n');
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(txt);
          copyBtn.classList.add('btn-success');
          setTimeout(function(){ copyBtn.classList.remove('btn-success'); }, 900);
        } else {
          var ta = document.createElement('textarea'); ta.value = txt; document.body.appendChild(ta); ta.select(); document.execCommand('copy'); document.body.removeChild(ta);
        }
      } catch(e) {}
    };
  }
  // route modal header sorting wiring
  try {
    var rheaders = document.querySelectorAll('#route-modal-table thead th[data-key]');
    rheaders.forEach(function(h){ h.onclick = function(){ var k=h.getAttribute('data-key'); if(_routeSort.key===k) _routeSort.asc=!_routeSort.asc; else { _routeSort.key=k; _routeSort.asc=true; } renderTable(allRoutes); }; });
  } catch(e) {}
  // route raw-toggle removed from UI
  fetch('/olsr/routes?via=' + encodeURIComponent(remoteIp), {cache:'no-store'})
    .then(function(r){ return r.json ? r.json() : r; })
    .then(function(obj){
      var routesArr = [];
      if (Array.isArray(obj)) routesArr = obj; else if (obj && Array.isArray(obj.routes)) routesArr = obj.routes;
      // Normalize raw strings and parse into structured fields: destination, device, metric
      routesArr = routesArr.map(function(line){
        if (typeof line !== 'string') return '';
        var t = line.replace(/^"+|"+$/g,'').replace(/\\"/g,'"').trim();
        return t;
      }).filter(function(s){ return s.length; });
      allRoutes = routesArr.map(function(line){
        var parts = line.split(/\s+/).filter(function(p){ return p.length; });
        var destination = parts[0] || line;
        var device = '';
        var metric = '';
        if (parts.length === 2) {
          var maybe = parts[1];
          if (!isNaN(Number(maybe))) { metric = maybe; }
          else { device = maybe; }
        } else if (parts.length >= 3) {
          var last = parts[parts.length-1];
          if (!isNaN(Number(last))) {
            metric = last;
            device = parts.slice(1, parts.length-1).join(' ');
          } else {
            device = parts.slice(1).join(' ');
          }
        }
        return { destination: destination, device: device, metric: metric, raw: line };
      });
  if (countBadge) { countBadge.style.display='inline-block'; countBadge.textContent = (obj && typeof obj.count==='number'? obj.count : allRoutes.length); }
  renderTable(allRoutes);
      if (bodyPre) {
        var header=''; if (obj && !Array.isArray(obj) && typeof obj.count==='number') header='Via '+(obj.via||remoteIp)+' ('+obj.count+' routes)\n\n';
        bodyPre.textContent = header + routesArr.join('\n');
      }
    })
    .catch(function(){ if(tbody) tbody.innerHTML='<tr><td colspan="4" class="text-danger">Error loading routes</td></tr>'; if(bodyPre) bodyPre.textContent='Error loading routes'; });
  showModal('route-modal');
}

window.addEventListener('load', function(){
  var c = document.getElementById('route-modal-close');
  if (c) c.addEventListener('click', function(){ hideModal('route-modal'); });
  var m = document.getElementById('route-modal');
  if (m) m.addEventListener('click', function(e){ if (e.target === m) hideModal('route-modal'); });

  var nc = document.getElementById('node-modal-close');
  if (nc) nc.addEventListener('click', function(){ hideModal('node-modal'); window._nodeModal_state = null; if (_nodeModalKeyHandler) { document.removeEventListener('keydown', _nodeModalKeyHandler); _nodeModalKeyHandler = null; } });
  var nm = document.getElementById('node-modal');
  if (nm) nm.addEventListener('click', function(e){ if (e.target === nm) { hideModal('node-modal'); window._nodeModal_state = null; if (_nodeModalKeyHandler) { document.removeEventListener('keydown', _nodeModalKeyHandler); _nodeModalKeyHandler = null; } } });
});

// keyboard nav state for node modal
var _nodeModalKeyHandler = null;

function populateNeighborsTable(neighbors) {
  var tbody = document.querySelector('#neighborsTable tbody');
  if (!tbody) return; tbody.innerHTML = '';
  if (!neighbors || !Array.isArray(neighbors) || neighbors.length === 0) {
    var tbody = document.querySelector('#neighborsTable tbody');
    if (tbody) tbody.innerHTML = '<tr><td colspan="7" class="text-muted">No neighbors found</td></tr>';
    return;
  }
  neighbors.forEach(function(n){
    var tr = document.createElement('tr');
    function td(val){ var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(n.originator));
    tr.appendChild(td(n.hostname));
    tr.appendChild(td(n.bindto));
    tr.appendChild(td(n.lq));
    tr.appendChild(td(n.nlq));
    tr.appendChild(td(n.cost));
    tr.appendChild(td(n.metric));
    tbody.appendChild(tr);
  });
  // ensure neighbors pane is visible
  try { var pane = document.getElementById('tab-neighbors'); if (pane) { pane.classList.remove('hidden'); pane.style.display = ''; } } catch(e) {}
}

function updateUI(data) {
  try {
  // Prefer resolved nodename from nodedb when available
  var resolved = null;
  try { resolved = getNodeNameForIp(data.ipv4 || data.ip || ''); } catch(e) { resolved = null; }
  setText('hostname', (resolved || data.hostname) || 'Unknown');
  setText('ip', data.ip || '');
  // compact status card fields
  try { var hip = document.getElementById('status-host-ip'); if (hip) hip.textContent = data.ip || ''; } catch(e){}
  try { var hn = document.getElementById('status-hostname'); if (hn) hn.textContent = (resolved || data.hostname) || ''; } catch(e){}
  // prefer human-friendly uptime string if provided by backend
  setText('uptime', data.uptime_linux || data.uptime_str || data.uptime || '');
  setText('dl-uptime', data.uptime_linux || data.uptime_str || data.uptime || '');
  try { if (data.hostname) document.title = data.hostname; } catch(e) {}
  try { setText('main-host', data.ip || ''); } catch(e) {}
  try {
    // Build FQDN explicitly from backend fields. Prefer explicit node identifiers
    // (data.node, data.nodename, data.node_name, data.net) and fall back to a
    // hint from data.versions.host. If the node hint is missing or equals the
    // literal token 'nodename', attempt to substitute the real nodename from the
    // cached nodedb using the node's IP (data.ipv4 or data.ip).
    var shortHost = data.hostname || '';
    var nodeHint = data.node || data.nodename || data.node_name || data.net || '';
    if (!nodeHint && data.versions && data.versions.host) {
      try { nodeHint = String(data.versions.host).split('.')[0] || nodeHint; } catch(_) {}
    }
    var ipKey = data.ipv4 || data.ip || '';
    var nodePart = nodeHint || '';
    try {
      if ((!nodePart || nodePart === 'nodename') && ipKey && window._nodedb_cache && window._nodedb_cache[ipKey] && window._nodedb_cache[ipKey].n) {
        nodePart = window._nodedb_cache[ipKey].n;
      }
    } catch(e) { /* ignore */ }
    if (!nodePart) nodePart = 'nodename';
    var fqdn = '';
    if (shortHost) {
      if (nodePart.indexOf('wien.funkfeuer') !== -1 || nodePart.indexOf('.') !== -1) {
        fqdn = shortHost + '.' + nodePart;
      } else {
        fqdn = shortHost + '.' + nodePart + '.wien.funkfeuer.at';
      }
    } else {
      fqdn = shortHost;
    }
    populateNavHost(fqdn, ipKey);
  // Also set the page-level host element (non-duplicating ID)
  try { var mh = document.getElementById('main-host-page'); if (mh) mh.textContent = fqdn || ''; } catch(e) {}
  } catch(e) {}
  // render default route if available (hostname link + ip link + device)
  try {
    if (data.default_route && (data.default_route.ip || data.default_route.dev || data.default_route.hostname)) {
      var host = data.default_route.hostname || '';
      var ip = data.default_route.ip || '';
      var dev = data.default_route.dev || '';
      var parts = [];
      if (host) parts.push('<a target="_blank" href="https://' + host + '">' + host + '</a>');
      if (ip) parts.push('(<a target="_blank" href="https://' + ip + '">' + ip + '</a>)');
      var html = parts.join(' ');
      if (dev) html += ' via ' + dev;
      setHTML('default-route', html);
      setHTML('dl-default-route', html);
    } else {
      setText('default-route', 'n/a');
    }
  } catch(e) { setText('default-route', 'n/a'); }
  // Render lightweight devices if present, but fetch full device inventory
  // asynchronously from /devices.json so /status/lite stays fast.
  populateDevicesTable(data.devices, data.airos);
  try { window._devices_data = Array.isArray(data.devices) ? data.devices : []; } catch(e) { window._devices_data = []; }
  // Show loading row while fetching the full table
  try {
    var tbody = document.querySelector('#devicesTable tbody');
    if (tbody) tbody.innerHTML = '<tr id="devices-loading"><td colspan="12" class="text-muted">Loading devices…</td></tr>';
    fetch('/devices.json', {cache: 'no-store'}).then(function(r){ if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); }).then(function(d){ try { if (d && Array.isArray(d.devices)) populateDevicesTable(d.devices, d.airos || {}); } catch(e){} }).catch(function(){ try { if (window._devices_data && Array.isArray(window._devices_data)) populateDevicesTable(window._devices_data, data.airos); } catch(e){} }).finally(function(){ var el = document.getElementById('devices-loading'); if (el) el.parentNode.removeChild(el); });
  } catch(e){}
  if (data.olsr2_on) {
    showTab('tab-olsr2', true);
    var li = document.getElementById('tab-olsrd2-links'); if (li) li.style.display='';
    setText('olsr2info', data.olsr2info || '');
  } else {
    showTab('tab-olsr2', false);
    var li = document.getElementById('tab-olsrd2-links'); if (li) li.style.display='none';
  }
  try { var olcount = (data.links && Array.isArray(data.links)) ? data.links.length : (window._olsr_links? window._olsr_links.length : 0); var el = document.getElementById('olsr-links-count'); if (el) el.textContent = olcount; } catch(e) {}
  try { var su = document.getElementById('status-uptime'); if (su) su.textContent = data.uptime_linux || data.uptime_str || data.uptime || ''; } catch(e) {}
  // legacy OLSR tab visibility (if backend set olsrd_on or provided links while not olsr2)
  try {
    if (!data.olsr2_on && (data.olsrd_on || (data.links && data.links.length))) {
      var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
      if (linkTab) {
  if (window._uiDebug) console.debug('Showing OLSR tab via updateUI: olsr2_on=', data.olsr2_on, 'olsrd_on=', data.olsrd_on, 'links_len=', (data.links && data.links.length));
        linkTab.parentElement.style.display = '';
      }
    }
  } catch(e){}
  if (data.admin && data.admin.url) {
    showTab('tab-admin', true);
    var adminLink = document.getElementById('adminLink');
    if (adminLink) { adminLink.href = data.admin.url; adminLink.textContent = 'Login'; }
  } else {
    showTab('tab-admin', false);
  }
  } catch(e) {}
  try { setLastUpdated(); } catch(e) {}
}

// --- Statistics graphs: maintain short in-memory series and render sparklines ---
window._stats_series = window._stats_series || { olsr_routes: [], olsr_nodes: [], fetch_queued: [], fetch_processing: [] };
var STATS_SERIES_MAX = 60; // keep last 60 samples (~1 min at 1s sampling)

function pushStat(seriesName, value) {
  try {
    var s = window._stats_series[seriesName] = window._stats_series[seriesName] || [];
    s.push(Number(value) || 0);
    if (s.length > STATS_SERIES_MAX) s.shift();
  } catch(e) {}
}

// renderSparkline removed: replaced with a no-op to keep callers safe
// Render a simple line graph in a canvas for the last 10 datapoints
function renderLineGraph(canvasId, series, color, yLabel) {
  // If an <svg> with this id exists, render a sparkline there for crisper scaling.
  var el = document.getElementById(canvasId);
  var data = (series || []).slice(-10);
  if (!data || !data.length) return;
  if (el && el.tagName && el.tagName.toLowerCase() === 'svg') {
    var svg = el;
    var viewW = 320, viewH = 100;
    svg.setAttribute('viewBox', '0 0 ' + viewW + ' ' + viewH);
    while (svg.firstChild) svg.removeChild(svg.firstChild);
    var minY = Math.min.apply(null, data), maxY = Math.max.apply(null, data);
    if (minY === maxY) { minY = 0; maxY = maxY + 1; }
    var pad = 12;
    var points = data.map(function(v, i){
      var x = pad + ((viewW-2*pad) * i/(data.length-1));
      var y = viewH - pad - ((viewH-2*pad) * (v - minY)/(maxY - minY));
      return [x,y];
    });
    var areaPts = points.map(function(p){ return p[0]+','+p[1]; }).join(' ');
    var area = document.createElementNS('http://www.w3.org/2000/svg','polygon');
    area.setAttribute('points', pad+','+(viewH-pad)+' '+ areaPts +' '+(viewW-pad)+','+(viewH-pad));
    area.setAttribute('fill', color); area.setAttribute('fill-opacity', '0.08'); svg.appendChild(area);
    var pathD = points.map(function(p,i){ return (i===0? 'M':'L') + p[0] + ' ' + p[1]; }).join(' ');
    var path = document.createElementNS('http://www.w3.org/2000/svg','path');
    path.setAttribute('d', pathD); path.setAttribute('stroke', color); path.setAttribute('stroke-width','2'); path.setAttribute('fill','none'); svg.appendChild(path);
    points.forEach(function(p){ var c = document.createElementNS('http://www.w3.org/2000/svg','circle'); c.setAttribute('cx', p[0]); c.setAttribute('cy', p[1]); c.setAttribute('r','3'); c.setAttribute('fill', color); svg.appendChild(c); });
    var txtMin = document.createElementNS('http://www.w3.org/2000/svg','text'); txtMin.setAttribute('x', 4); txtMin.setAttribute('y', viewH-pad); txtMin.setAttribute('fill','#666'); txtMin.setAttribute('font-size','10'); txtMin.textContent = String(Math.round(minY)); svg.appendChild(txtMin);
    var txtMax = document.createElementNS('http://www.w3.org/2000/svg','text'); txtMax.setAttribute('x', 4); txtMax.setAttribute('y', pad+8); txtMax.setAttribute('fill','#666'); txtMax.setAttribute('font-size','10'); txtMax.textContent = String(Math.round(maxY)); svg.appendChild(txtMax);
    return;
  }
  // Fallback: draw to canvas if present
  var canvas = el && el.getContext ? el : document.getElementById(canvasId);
  if (!canvas || !canvas.getContext) return;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  var w = canvas.width, h = canvas.height;
  var minY = Math.min.apply(null, data), maxY = Math.max.apply(null, data);
  if (minY === maxY) { minY = 0; maxY = maxY + 1; }
  var pad = 12;
  // Draw background grid lines
  ctx.strokeStyle = '#f0f0f0';
  ctx.lineWidth = 1;
  for (var gy=0; gy<4; gy++) {
    var yy = pad + ((h-2*pad) * gy/3);
    ctx.beginPath(); ctx.moveTo(pad, yy); ctx.lineTo(w-pad, yy); ctx.stroke();
  }
  ctx.strokeStyle = '#ddd'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(pad, pad); ctx.lineTo(pad, h-pad); ctx.lineTo(w-pad, h-pad); ctx.stroke();
  ctx.fillStyle = '#666'; ctx.font = '11px sans-serif';
  ctx.fillText(yLabel, 4, pad+6);
  ctx.strokeStyle = color || '#0074d9';
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (var i=0; i<data.length; i++) {
    var x = pad + ((w-2*pad) * i/(data.length-1));
    var y = h-pad - ((h-2*pad) * (data[i]-minY)/(maxY-minY));
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
  ctx.globalAlpha = 0.08; ctx.fillStyle = color || '#0074d9';
  ctx.beginPath();
  for (var i=0; i<data.length; i++) {
    var x = pad + ((w-2*pad) * i/(data.length-1));
    var y = h-pad - ((h-2*pad) * (data[i]-minY)/(maxY-minY));
    if (i === 0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.lineTo(w-pad, h-pad); ctx.lineTo(pad, h-pad); ctx.closePath(); ctx.fill();
  ctx.globalAlpha = 1.0;
  ctx.fillStyle = color || '#0074d9';
  for (var i=0; i<data.length; i++) {
    var x = pad + ((w-2*pad) * i/(data.length-1));
    var y = h-pad - ((h-2*pad) * (data[i]-minY)/(maxY-minY));
    ctx.beginPath(); ctx.arc(x, y, 3, 0, 2*Math.PI); ctx.fill();
  }
  ctx.fillStyle = '#666'; ctx.font = '10px sans-serif';
  ctx.fillText(minY, 2, h-pad);
  ctx.fillText(maxY, 2, pad+4);
}

// Generic card renderer: renders a Bootstrap-style panel into a container.
// opts: { container: (id|string|element), title, subtitle, smallHelp, render: function(bodyEl, headerRightEl) }
function renderCard(opts) {
  try {
    var container = null;
    if (!opts) return;
    if (typeof opts.container === 'string') container = document.getElementById(opts.container);
    else container = opts.container || null;
    if (!container) return;
    // clear
    container.innerHTML = '';
    // panel
    var panel = document.createElement('div'); panel.className = opts.panelClass || 'panel panel-default';
    var heading = document.createElement('div'); heading.className = 'panel-heading'; heading.style.display = 'flex'; heading.style.justifyContent = 'space-between'; heading.style.alignItems = 'center';
    var titleWrap = document.createElement('div');
    var title = document.createElement('div'); title.style.fontWeight = '600'; title.style.marginBottom = '4px'; title.textContent = opts.title || '';
    titleWrap.appendChild(title);
    if (opts.subtitle) { var sub = document.createElement('div'); sub.style.fontSize = '12px'; sub.style.color = '#666'; sub.textContent = opts.subtitle; titleWrap.appendChild(sub); }
    heading.appendChild(titleWrap);
    var headerRight = document.createElement('div'); headerRight.style.display = 'flex'; headerRight.style.gap = '8px'; headerRight.style.alignItems = 'center'; heading.appendChild(headerRight);
    panel.appendChild(heading);
    var body = document.createElement('div'); body.className = 'panel-body'; if (opts.bodyStyle) body.style.cssText = opts.bodyStyle;
    panel.appendChild(body);
    // allow render callback to populate body and headerRight
    try { if (typeof opts.render === 'function') opts.render(body, headerRight); }
    catch (e) { /* swallow UI render errors */ }
    if (opts.smallHelp) {
      var help = document.createElement('div'); help.className = 'small-muted'; help.style.marginTop = '8px'; help.innerHTML = opts.smallHelp; body.appendChild(help);
    }
    container.appendChild(panel);
  } catch (e) { /* ignore */ }
}

// Top-level reusable card renderer
function renderCard(opts) {
  // opts: { containerId, title, subtitle, contentHTML, smallHelp }
  try {
    var container = document.getElementById(opts.containerId);
    if (!container) return;
    container.innerHTML = '';
    var title = document.createElement('div'); title.style.fontWeight = '600'; title.style.marginBottom = '6px'; title.textContent = opts.title || '';
    var wrap = document.createElement('div'); wrap.style.display = 'flex'; wrap.style.flexDirection = 'column'; wrap.style.gap = '8px';
    if (opts.subtitle) {
      var sub = document.createElement('div'); sub.style.fontSize='12px'; sub.style.color='#666'; sub.textContent = opts.subtitle; container.appendChild(sub);
    }
    container.appendChild(title);
    if (opts.contentHTML) {
      var content = document.createElement('div'); content.innerHTML = opts.contentHTML; wrap.appendChild(content);
    }
    if (opts.smallHelp) {
      var help = document.createElement('div'); help.style.fontSize='11px'; help.style.color='#666'; help.innerHTML = opts.smallHelp; wrap.appendChild(help);
    }
    container.appendChild(wrap);
  } catch(e) {}
}

// Hook into updateUI to sample stats when status payload contains them
var _original_updateUI = updateUI;
updateUI = function(data) {
  try {
    // sample olsr routes/nodes if available in payload
    try {
      var routes = 0; var nodes = 0;
      if (data.links && Array.isArray(data.links)) {
        // count unique routes approximated by sum of link.routes
        routes = data.links.reduce(function(acc,l){ return acc + (parseInt(l.routes,10)||0); }, 0);
        nodes = data.links.reduce(function(acc,l){ return acc + (parseInt(l.nodes,10)||0); }, 0);
      } else {
        if (typeof data.olsr_routes_count === 'number') {
          routes = data.olsr_routes_count;
        }
        if (typeof data.olsr_nodes_count === 'number') {
          nodes = data.olsr_nodes_count;
        }
      }
      pushStat('olsr_routes', routes);
      pushStat('olsr_nodes', nodes);
      // Update compact header badges when available
      try {
        var bn = document.getElementById('badge-nodes'); if (bn) { bn.textContent = 'Nodes: ' + String(nodes); }
        var br = document.getElementById('badge-routes'); if (br) { br.textContent = 'Routes: ' + String(routes); }
      } catch(e) {}
    } catch(e) {}
    // sample fetch queue metrics if provided
    try {
      var fq = (data.fetch_stats && (data.fetch_stats.queued_count || data.fetch_stats.queue_length || data.fetch_stats.queued)) || 0;
      var fp = (data.fetch_stats && (data.fetch_stats.processing_count || data.fetch_stats.processing || data.fetch_stats.in_progress)) || 0;
      pushStat('fetch_queued', fq);
      pushStat('fetch_processing', fp);
    } catch(e) {}
    // render graphs for nodes and routes (last 10 datapoints)
      try {
        var routesArr = window._stats_series.olsr_routes || [];
        var nodesArr = window._stats_series.olsr_nodes || [];
        var routesCur = routesArr.length ? routesArr[routesArr.length-1] : 0;
        var nodesCur = nodesArr.length ? nodesArr[nodesArr.length-1] : 0;
        var routesEl = document.getElementById('routes-cur'); if (routesEl) routesEl.textContent = String(routesCur);
        var nodesEl = document.getElementById('nodes-cur'); if (nodesEl) nodesEl.textContent = String(nodesCur);
        // Draw graphs
        renderLineGraph('routes-graph', routesArr, '#0074d9', 'Routes');
        renderLineGraph('nodes-graph', nodesArr, '#2ecc40', 'Nodes');
        // compute min/avg/max and trend for routes and nodes
        try {
          function statsFromArr(arr) {
            if (!arr || !arr.length) return { min:0, max:0, avg:0, trend:0 };
            var min = Math.min.apply(null, arr);
            var max = Math.max.apply(null, arr);
            var sum = arr.reduce(function(a,b){return a+(Number(b)||0);},0);
            var avg = sum / arr.length;
            var trend = 0;
            if (arr.length >= 2) {
              var last = Number(arr[arr.length-1])||0;
              var prior = Number(arr[Math.max(0, arr.length-6)])||0; // compare against ~6 samples ago
              trend = last - prior;
            }
            return { min: Math.round(min), max: Math.round(max), avg: Math.round(avg*10)/10, trend: Math.round(trend) };
          }
          var rStats = statsFromArr(routesArr.slice(-10));
          var nStats = statsFromArr(nodesArr.slice(-10));
          var routesMinEl = document.getElementById('routes-min'); if (routesMinEl) routesMinEl.textContent = String(rStats.min);
          var routesAvgEl = document.getElementById('routes-avg'); if (routesAvgEl) routesAvgEl.textContent = String(rStats.avg);
          var routesMaxEl = document.getElementById('routes-max'); if (routesMaxEl) routesMaxEl.textContent = String(rStats.max);
          var nodesMinEl = document.getElementById('nodes-min'); if (nodesMinEl) nodesMinEl.textContent = String(nStats.min);
          var nodesAvgEl = document.getElementById('nodes-avg'); if (nodesAvgEl) nodesAvgEl.textContent = String(nStats.avg);
          var nodesMaxEl = document.getElementById('nodes-max'); if (nodesMaxEl) nodesMaxEl.textContent = String(nStats.max);
          // trend arrows
          var routesTrendEl = document.getElementById('routes-trend'); if (routesTrendEl) {
            routesTrendEl.className = '';
            if (rStats.trend > 0) { routesTrendEl.textContent = '▲ ' + String(rStats.trend); routesTrendEl.classList.add('trend-up'); }
            else if (rStats.trend < 0) { routesTrendEl.textContent = '▼ ' + String(Math.abs(rStats.trend)); routesTrendEl.classList.add('trend-down'); }
            else { routesTrendEl.textContent = '◦'; routesTrendEl.classList.add('trend-flat'); }
          }
          var nodesTrendEl = document.getElementById('nodes-trend'); if (nodesTrendEl) {
            nodesTrendEl.className = '';
            if (nStats.trend > 0) { nodesTrendEl.textContent = '▲ ' + String(nStats.trend); nodesTrendEl.classList.add('trend-up'); }
            else if (nStats.trend < 0) { nodesTrendEl.textContent = '▼ ' + String(Math.abs(nStats.trend)); nodesTrendEl.classList.add('trend-down'); }
            else { nodesTrendEl.textContent = '◦'; nodesTrendEl.classList.add('trend-flat'); }
          }
        } catch(e) {}
      } catch(e) {}
  } catch(e) {}
  // Only call the original full updateUI when the payload looks like a full status
  try {
    var looksFull = (data && (data.hostname || data.devices || data.links || data.ip || data.uptime || data.olsr2_on || data.olsrd_on));
    if (looksFull) {
      try { _original_updateUI(data); } catch(e) {}
    } else {
      // minimal stats-only payload -> do not overwrite the full UI
      if (window._uiDebug) console.debug('Skipping full updateUI for stats-only payload');
    }
  } catch(e) {}
};

  // Lightweight polling to keep statistics live: fetch /status/lite periodically and feed updateUI
  window._stats_poll_interval_ms = 5000; // enforce 5s polling as requested
  function _statsPollOnce() {
    fetch('/status/stats', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){ try { if (s) {
      // Map minimal stats payload into shape expected by updateUI
      var mapped = {};
      if (typeof s.olsr_routes_count !== 'undefined') mapped.olsr_routes_count = s.olsr_routes_count;
      if (typeof s.olsr_nodes_count !== 'undefined') mapped.olsr_nodes_count = s.olsr_nodes_count;
      if (s.fetch_stats) mapped.fetch_stats = s.fetch_stats;
      updateUI(mapped);
    } } catch(e) {} }).catch(function(){});
  }
  function _startStatsPoll() {
    if (window._stats_poll_handle) return;
    _statsPollOnce();
    window._stats_poll_handle = setInterval(_statsPollOnce, window._stats_poll_interval_ms);
  }
  function _stopStatsPoll(){ if (window._stats_poll_handle) { clearInterval(window._stats_poll_handle); window._stats_poll_handle = null; } }

// update last-updated timestamp helper (called from updateUI)
function setLastUpdated(ts) {
  try {
    var el = document.getElementById('last-updated'); if (!el) return;
    var text = ts || new Date().toLocaleString(); el.textContent = text;
  } catch(e) {}
}

function showActionToast(msg, ms) {
  try {
    var toast = document.getElementById('action-toast'); var body = document.getElementById('action-toast-body');
    if (!toast || !body) return;
    body.textContent = msg || '';
    toast.style.display = 'block';
    toast.style.opacity = '1';
    if (window._actionToastTimer) { clearTimeout(window._actionToastTimer); window._actionToastTimer = null; }
    window._actionToastTimer = setTimeout(function(){ try { toast.style.opacity='0'; setTimeout(function(){ toast.style.display='none'; },300); } catch(e){} }, ms || 2000);
  } catch(e) {}
}



// Render fetch queue metrics (provided by backend in status.fetch_stats)
function populateFetchStats(fs) {
  try {
    var tabWrap = document.getElementById('fetch-tab-wrap'); if (!tabWrap) return;
    var container = document.getElementById('fetch-stats');
    if (!container) {
      container = document.createElement('div');
      container.id = 'fetch-stats';
      container.style.margin = '6px 0 12px 0';
      tabWrap.appendChild(container);
    }
  if (!fs || (typeof fs === 'object' && Object.keys(fs).length === 0)) { container.style.display = 'none'; return; }
  container.style.display = '';

    // Normalize likely field names from backend
    var queued = fs.queued_count || fs.queue_length || fs.queue_len || fs.queued || 0;
    var processing = fs.processing_count || fs.in_progress || fs.processing || 0;
    var dropped = fs.dropped_count || fs.dropped || 0;
    var retries = fs.retry_count || fs.retries || 0;
    var processed = fs.total_processed || fs.processed || 0;
    var successes = fs.successes || fs.fetch_successes || 0;
    // New debug counters provided by backend
    var enqueued_total = fs.enqueued || fs.enqueued_total || 0;
    var enqueued_nodedb = fs.enqueued_nodedb || 0;
    var enqueued_discover = fs.enqueued_discover || 0;
    var processed_total = fs.processed || fs.processed_total || 0;
    var processed_nodedb = fs.processed_nodedb || 0;
    var processed_discover = fs.processed_discover || 0;

    // thresholds (backend may export thresholds for front-end convenience)
    var thresholds = (fs.thresholds && typeof fs.thresholds === 'object') ? fs.thresholds : {};
    var q_warn = thresholds.queue_warn || 50;
    var q_crit = thresholds.queue_crit || 200;
    var d_warn = thresholds.dropped_warn || 10;

    // Render using renderCard for consistent layout and easier reuse
    renderCard({
      container: container,
      title: 'Fetch Queue',
      panelClass: 'panel panel-default',
      bodyStyle: 'padding:10px',
      render: function(body, headerRight) {
        // left column (badges + progress)
        var left = document.createElement('div'); left.style.display='inline-block'; left.style.width='62%'; left.style.verticalAlign='top';
        var badges = document.createElement('div'); badges.style.marginBottom='8px';
        function mkBadgeNode(label, val, cls) { var sp=document.createElement('span'); sp.className='fetch-badge '+(cls||''); sp.style.marginRight='6px'; sp.innerHTML = label+': <strong style="margin-left:6px;">'+String(val)+'</strong>'; return sp; }
        var qcls = queued >= q_crit ? 'crit' : (queued >= q_warn ? 'warn' : '');
        var dcls = dropped >= d_warn ? 'warn' : '';
        badges.appendChild(mkBadgeNode('Queued', queued, qcls));
        badges.appendChild(mkBadgeNode('Processing', processing));
        badges.appendChild(mkBadgeNode('Dropped', dropped, dcls));
        badges.appendChild(mkBadgeNode('Retries', retries));
        badges.appendChild(mkBadgeNode('Processed', processed));
        left.appendChild(badges);
        var denom = q_crit > 0 ? q_crit : 200;
        var pct = Math.min(100, Math.round((queued / denom) * 100));
        var prog = document.createElement('div'); prog.className='progress'; prog.style.height='16px'; prog.style.marginBottom='6px';
        var progBar = document.createElement('div'); var progClass = pct >= 100 ? 'progress-bar-danger' : (pct >= Math.round((q_warn/denom)*100) ? 'progress-bar-warning' : 'progress-bar-success');
        progBar.className = 'progress-bar '+progClass; progBar.setAttribute('role','progressbar'); progBar.setAttribute('aria-valuenow', String(pct)); progBar.setAttribute('aria-valuemin','0'); progBar.setAttribute('aria-valuemax','100'); progBar.style.width = pct + '%';
        prog.appendChild(progBar); left.appendChild(prog);
        var pctNode = document.createElement('div'); pctNode.style.marginTop='4px'; pctNode.textContent = pct + '%'; left.appendChild(pctNode);
        body.appendChild(left);

        // right column (small table)
        var right = document.createElement('div'); right.style.display='inline-block'; right.style.width='36%'; right.style.paddingLeft='10px'; right.style.verticalAlign='top';
        var table = document.createElement('table'); table.className = 'table table-condensed'; table.style.margin='0';
        var tb = document.createElement('tbody');
        function mkRow(k,v){ var r=document.createElement('tr'); var th=document.createElement('th'); if (k==='Queued') th.style.width='55%'; th.textContent = k; var td=document.createElement('td'); td.textContent = String(v); r.appendChild(th); r.appendChild(td); return r; }
        tb.appendChild(mkRow('Queued', queued)); tb.appendChild(mkRow('Processing', processing)); tb.appendChild(mkRow('Dropped', dropped)); tb.appendChild(mkRow('Retries', retries)); tb.appendChild(mkRow('Processed', processed)); tb.appendChild(mkRow('Successes', successes));
        table.appendChild(tb); right.appendChild(table); body.appendChild(right);

        // headerRight controls (preserve element IDs for existing wiring)
        var refreshBtn = document.createElement('button'); refreshBtn.id='fetch-stats-refresh'; refreshBtn.className='btn btn-xs btn-default'; refreshBtn.innerHTML = '<i class="glyphicon glyphicon-refresh"></i>';
        var debugBtn = document.createElement('button'); debugBtn.id='fetch-stats-debug'; debugBtn.className='btn btn-xs btn-default'; debugBtn.textContent='Debug';
        var sel = document.createElement('select'); sel.id='fetch-stats-interval'; sel.className='input-sm form-control'; sel.style.width='120px'; sel.style.display='inline-block'; sel.style.marginRight='6px';
        [['0','Auto off'],['5000','5s'],['10000','10s'],['15000','15s'],['30000','30s'],['60000','60s'],['120000','2m'],['300000','5m']].forEach(function(o){ var opt=document.createElement('option'); opt.value=o[0]; opt.textContent=o[1]; sel.appendChild(opt); });
        var autoBtn = document.createElement('button'); autoBtn.id='fetch-stats-autorefresh'; autoBtn.className='btn btn-xs btn-default'; autoBtn.title='Toggle auto-refresh'; autoBtn.textContent='Auto';
        headerRight.appendChild(refreshBtn); headerRight.appendChild(debugBtn); headerRight.appendChild(sel); headerRight.appendChild(autoBtn);

        // thresholds footer
        var thr = document.createElement('div'); thr.className='small-muted'; thr.style.marginTop='8px'; thr.innerHTML = 'Thresholds: warn='+q_warn+' &nbsp; crit='+q_crit+' &nbsp; dropped_warn='+d_warn; body.appendChild(thr);
      }
    });

    // wire actions: refresh
    var ref = document.getElementById('fetch-stats-refresh');
    if (ref) ref.addEventListener('click', function(){
      try { ref.disabled = true; } catch(e){}
      fetch('/status/lite', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){ try { if (s.fetch_stats) populateFetchStats(s.fetch_stats); } catch(e){} }).catch(function(){
    safeFetchStatus({cache:'no-store'}).then(function(s){ try{ if (s.fetch_stats) populateFetchStats(s.fetch_stats); }catch(e){} });
      }).finally(function(){ try{ ref.disabled=false; }catch(e){} });
    });

    // Auto-refresh toggle logic
    var autoBtn = document.getElementById('fetch-stats-autorefresh');
    var intervalSel = document.getElementById('fetch-stats-interval');
    // derive default interval: prefer persisted localStorage, then backend hint, then default 15000
    try {
      var persisted = parseInt(localStorage.getItem('fetch_auto_interval_ms'), 10);
      if (!isNaN(persisted)) {
        window._fetch_auto_interval_ms = persisted;
      } else if (fs && fs.fetch_auto_refresh_ms) {
        window._fetch_auto_interval_ms = parseInt(fs.fetch_auto_refresh_ms, 10) || 15000;
      } else if (!window._fetch_auto_interval_ms) {
        window._fetch_auto_interval_ms = 15000; // default 15s
      }
    } catch(e) { if (!window._fetch_auto_interval_ms) window._fetch_auto_interval_ms = 15000; }
    if (!window._fetch_auto_handle) window._fetch_auto_handle = null;
    // set selector to current value
    try { if (intervalSel) intervalSel.value = String(window._fetch_auto_interval_ms || 0); } catch(e){}
    // change handler: persist & restart if needed
    if (intervalSel) intervalSel.addEventListener('change', function(){
      try {
        var v = parseInt(intervalSel.value, 10) || 0;
        window._fetch_auto_interval_ms = v;
        localStorage.setItem('fetch_auto_interval_ms', String(v));
        // if running, restart with new interval
        if (window._fetch_auto_handle) {
          clearInterval(window._fetch_auto_handle); window._fetch_auto_handle = null;
          if (v > 0) window._fetch_auto_handle = setInterval(function(){ try { document.getElementById('fetch-stats-refresh').click(); } catch(e){} }, v);
        }
      } catch(e){}
    });
    function startAutoRefresh() {
      if (window._fetch_auto_handle) return;
      if (!window._fetch_auto_interval_ms || window._fetch_auto_interval_ms <= 0) return; // disabled
      window._fetch_auto_handle = setInterval(function(){ try { document.getElementById('fetch-stats-refresh').click(); } catch(e){} }, window._fetch_auto_interval_ms);
      if (autoBtn) autoBtn.classList.add('btn-success');
    }
    function stopAutoRefresh() {
      if (!window._fetch_auto_handle) return;
      clearInterval(window._fetch_auto_handle); window._fetch_auto_handle = null;
      if (autoBtn) autoBtn.classList.remove('btn-success');
    }
    if (autoBtn) autoBtn.addEventListener('click', function(){ if (window._fetch_auto_handle) stopAutoRefresh(); else startAutoRefresh(); });

    // debug modal
    var dbg = document.getElementById('fetch-stats-debug');
    if (dbg) dbg.addEventListener('click', function(){
      var modal = document.getElementById('fetch-debug-modal'); var body = document.getElementById('fetch-debug-body');
      if (!modal || !body) return;
  body.textContent = 'Loading...'; showModal('fetch-debug-modal');
      fetch('/fetch_debug', {cache:'no-store'}).then(function(r){ return r.text(); }).then(function(t){ try { var obj = JSON.parse(t); body.textContent = JSON.stringify(obj, null, 2); } catch(e) { body.textContent = t; } }).catch(function(e){ body.textContent = 'ERR: '+e; });
    });

  // Update top header/nav indicator based on severity
    {
  // (status-summary card removed; fetch stats now live in the Fetch Queue tab)
      var hostEl = document.getElementById('nav-host');
      if (hostEl) {
      

      // Render a small SVG sparkline into an <svg> element with given id
      function renderSparkline(svgId, series, color) {
        var svg = document.getElementById(svgId);
        if (!svg) return;
        var viewW = 320, viewH = 100;
        svg.setAttribute('viewBox', '0 0 ' + viewW + ' ' + viewH);
        while (svg.firstChild) svg.removeChild(svg.firstChild);
        var data = (series || []).slice(-10);
        if (!data.length) return;
        var minY = Math.min.apply(null, data), maxY = Math.max.apply(null, data);
        if (minY === maxY) { minY = 0; maxY = maxY + 1; }
        var pad = 12;
        var points = data.map(function(v, i){
          var x = pad + ((viewW-2*pad) * i/(data.length-1));
          var y = viewH - pad - ((viewH-2*pad) * (v - minY)/(maxY - minY));
          return [x,y];
        });
        // background polyline area
        var areaPts = points.map(function(p){ return p[0]+','+p[1]; }).join(' ');
        var area = document.createElementNS('http://www.w3.org/2000/svg','polygon');
        area.setAttribute('points', pad+','+(viewH-pad)+' '+ areaPts +' '+(viewW-pad)+','+(viewH-pad));
        area.setAttribute('fill', color); area.setAttribute('fill-opacity', '0.08'); svg.appendChild(area);
        // line path
        var pathD = points.map(function(p,i){ return (i===0? 'M':'L') + p[0] + ' ' + p[1]; }).join(' ');
        var path = document.createElementNS('http://www.w3.org/2000/svg','path');
        path.setAttribute('d', pathD); path.setAttribute('stroke', color); path.setAttribute('stroke-width','2'); path.setAttribute('fill','none'); svg.appendChild(path);
        // points
        points.forEach(function(p){ var c = document.createElementNS('http://www.w3.org/2000/svg','circle'); c.setAttribute('cx', p[0]); c.setAttribute('cy', p[1]); c.setAttribute('r','3'); c.setAttribute('fill', color); svg.appendChild(c); });
        // min/max labels
        var txtMin = document.createElementNS('http://www.w3.org/2000/svg','text'); txtMin.setAttribute('x', 4); txtMin.setAttribute('y', viewH-pad); txtMin.setAttribute('fill','#666'); txtMin.setAttribute('font-size','10'); txtMin.textContent = String(Math.round(minY)); svg.appendChild(txtMin);
        var txtMax = document.createElementNS('http://www.w3.org/2000/svg','text'); txtMax.setAttribute('x', 4); txtMax.setAttribute('y', pad+8); txtMax.setAttribute('fill','#666'); txtMax.setAttribute('font-size','10'); txtMax.textContent = String(Math.round(maxY)); svg.appendChild(txtMax);
  }
      function extractAllTopLevelJSON(s) {
        var out = [];
        if (!s || !s.length) return out;
        var len = s.length;
        for (var i = 0; i < len; i++) {
          var ch = s[i];
          if (ch !== '{' && ch !== '[') continue;
          var open = ch; var close = (open === '{') ? '}' : ']';
          var depth = 1;
          for (var j = i + 1; j < len; j++) {
            var cj = s[j];
            if (cj === open) depth++;
            else if (cj === close) depth--;
            if (depth === 0) {
              var cand = s.substring(i, j + 1);
              try {
                var obj = JSON.parse(cand);
                out.push(obj);
              } catch (err) {
                // ignore parse errors for this candidate
              }
              i = j; // advance outer loop past this object
              break;
            }
          }
        }
        return out;
      }
      try {
        var candidates = extractAllTopLevelJSON(String(text || ''));
        if (!candidates || candidates.length === 0) return null;
        if (candidates.length === 1) return candidates[0];
        var preferredKeys = ['hostname','devices','links','fetch_stats','ip','uptime'];
        for (var k = candidates.length - 1; k >= 0; k--) {
          var c = candidates[k];
          if (c && typeof c === 'object') {
            for (var pi = 0; pi < preferredKeys.length; pi++) {
              if (typeof c[preferredKeys[pi]] !== 'undefined') return c;
            }
          }
        }
        var allDevices = true;
        for (var ii = 0; ii < candidates.length; ii++) {
          var cc = candidates[ii];
          if (!cc || typeof cc !== 'object') { allDevices = false; break; }
          if (typeof cc.ipv4 === 'undefined' && typeof cc.hwaddr === 'undefined' && typeof cc.ip === 'undefined') { allDevices = false; break; }
        }
        if (allDevices) return { devices: candidates };
        return candidates[candidates.length - 1];
      } catch(_e) { return null; }
    }
  }
  fetch('/capabilities', {cache: 'no-store'})
    .then(function(r) { return r.text(); })
    .then(function(capText) {
      var caps = safeParse('capabilities', capText) || {};
      // show/hide traceroute tab
      try {
        var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
        if (trTab) trTab.parentElement.style.display = (caps.traceroute? '': 'none');
        var adminTabLink = document.getElementById('tab-admin-link');
        if (adminTabLink) adminTabLink.style.display = (caps.show_admin_link? '' : 'none');
      } catch(e){}
  var data = { hostname: '', ip: '', uptime: '', devices: [], airos: {}, olsr2_on: false, olsrd_on: false, olsr2info: '', admin: null };
  // Fetch nodedb early so we can enrich hostnames when rendering
  try { fetch('/nodedb.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ window._nodedb_cache = nb || {}; }).catch(function(){}); } catch(e){}
  // Fetch summary first for fast paint (use tolerant parser because some devices
  // emit concatenated top-level JSON fragments). We try to keep the fast-path
  // but avoid r.json() which will throw on non-strict JSON streams.
  try {
    // Fast-path: fetch a lightweight summary text and render immediately.
    // Keep the raw text so we can pass it to safeFetchStatus and avoid a
    // second network request when the full parsing happens below.
    fetch('/status/lite', {cache: 'no-store'}).then(function(r){ return r.text(); }).then(function(t){
      try {
        var s = safeParse('status-lite', t) || {};
        if (s.hostname) data.hostname = s.hostname;
        if (s.ip) data.ip = s.ip;
        if (s.uptime_linux) { data.uptime_linux = s.uptime_linux; }
        updateUI(data);
      } catch(e) { /* ignore fast-path errors */ }
      // store the fast text so the robust parser can reuse it without re-fetching
      data._fast_status_text = t;
    }).catch(function(){});
  } catch(e) {}
  // Fetch remaining heavier status in background (full status still used for deep data)
  // Use safeFetchStatus which already implements robust extraction of the
  // last/top-level JSON object and candidate selection. If we have the
  // fast-path text available, pass it in to avoid a duplicate network request.
  var maybeText = data._fast_status_text;
  safeFetchStatus(maybeText || {cache: 'no-store'})
        .then(function(status) {
          // status is already parsed or null on failure
          if (!status) return; // abort if irreparable
            try { if (status.fetch_stats) populateFetchStats(status.fetch_stats); else populateFetchStats({}); } catch(e){}
          data.hostname = status.hostname || '';
          data.ip = status.ip || '';
          data.uptime = status.uptime || '';
          data.devices = status.devices || [];
          data.airos = status.airosdata || {};
          // ensure defaults so updateUI can safely use them
          data.default_route = status.default_route || {};
          data.links = status.links || [];
      if (status.olsr2_on) {
            data.olsr2_on = true;
            fetch('/olsr2', {cache: 'no-store'})
              .then(function(r) { return r.text(); })
              .then(function(t) { data.olsr2info = t; updateUI(data); try { if (data.links && data.links.length) populateOlsrLinksTable(data.links); } catch(e){} });
          } else {
            updateUI(data);
            try {
              // OLSR Links
                if (data.links && data.links.length) {
                var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
                if (linkTab) { if (window._uiDebug) console.debug('Showing OLSR tab with links from /status'); linkTab.parentElement.style.display = ''; }
                try { window._olsr_links = Array.isArray(data.links) ? data.links : []; } catch(e) { window._olsr_links = data.links || []; }
                populateOlsrLinksTable(window._olsr_links);
              } else {
                // Keep tab visible; it will lazy-load on click
                var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
                if (linkTab) { if (window._uiDebug) console.debug('Keeping OLSR tab visible (no links yet)'); linkTab.parentElement.style.display = ''; }
              }
        // capture legacy olsrd_on flag if provided by backend full status later
        if (typeof status.olsrd_on === 'boolean') data.olsrd_on = status.olsrd_on;
              // Neighbors
              if (status.olsr2_on && status.neighbors && Array.isArray(status.neighbors) && status.neighbors.length) {
                var nLi = document.getElementById('tab-olsrd2-links'); if (nLi) nLi.style.display='';
                populateNeighborsTable(status.neighbors);
              } else {
                var nLi = document.getElementById('tab-olsrd2-links'); if (nLi) nLi.style.display='none';
              }
            } catch(e){}
          }
          if (status.admin_url) {
            data.admin = { url: status.admin_url };
            updateUI(data);
            setLoginLink(status.admin_url);
          }
          // older code used status.admin_url; also support status.admin.url
          if (status.admin && status.admin.url) {
            data.admin = { url: status.admin.url };
            updateUI(data);
            setLoginLink(status.admin.url);
          }
          var nodedb = {};
          var nodedbReady = false;
          function tryRenderConnections() {
            if (!nodedbReady) return; // wait until nodedb loaded once
            return fetch('/connections.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(c){
              renderConnectionsTable(c, nodedb);
              var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = '';
            }).catch(function(e){ var el=document.getElementById('connections-status'); if(el) el.textContent='ERR: '+e; });
          }
          // load nodedb first then connections
          fetch('/nodedb.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ nodedb = nb || {}; nodedbReady = true; tryRenderConnections(); }).catch(function(){ nodedb = {}; nodedbReady = true; tryRenderConnections(); });
          function loadConnections() {
            // force refresh after nodedb already loaded
            if (!nodedbReady) return; // will auto-run when ready
            var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = 'Loading...';
            // Return a Promise so callers can finalize UI state
            return new Promise(function(resolve, reject){
              tryRenderConnections().then(function(){ if(statusEl) statusEl.textContent=''; resolve(); }).catch(function(err){ if(statusEl) statusEl.textContent='ERR: '+err; reject(err); });
            });
          }
          // defer loading connections and versions until tab activation to speed initial paint
          var _connectionsLoaded = false;
          var _versionsLoaded = false;
          // auto-load Versions tab once on startup for faster access
          try { if (typeof loadVersions === 'function') { loadVersions(); } } catch(e) {}
          function loadConnections() {
            if (_connectionsLoaded) return Promise.resolve();
            _connectionsLoaded = true;
            var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = 'Loading...';
            return fetch('/nodedb.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ var nodedb = nb || {}; return fetch('/connections.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(c){ renderConnectionsTable(c, nodedb); if(statusEl) statusEl.textContent=''; }).catch(function(e){ if(statusEl) statusEl.textContent='ERR: '+e; }); }).catch(function(){ if(statusEl) statusEl.textContent=''; });
          }
          function loadVersions() {
            if (_versionsLoaded) return Promise.resolve();
            _versionsLoaded = true;
            var statusEl = document.getElementById('versions-status'); if(statusEl) statusEl.textContent = 'Loading...';
            return fetch('/versions.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(v){ renderVersionsPanel(v); if(statusEl) statusEl.textContent = ''; }).catch(function(e){ var el=document.getElementById('versions-status'); if(el) el.textContent='ERR: '+e; });
          }
          document.getElementById('tr-run').addEventListener('click', function(){ runTraceroute(); });
          // Wire refresh buttons with consistent spinner + disable behavior
          var refreshConnBtn = document.getElementById('refresh-connections');
          if (refreshConnBtn) {
            refreshConnBtn.addEventListener('click', function(){
        try { refreshConnBtn.disabled = true; } catch(e){}
              // call loadConnections which returns a Promise
              try {
                var p = loadConnections();
                if (p && typeof p.finally === 'function') {
          p.finally(function(){ try{ refreshConnBtn.disabled=false; }catch(e){} });
                } else {
                  // Not a promise (nodedb not ready), just re-enable
          try{ refreshConnBtn.disabled=false; }catch(e){}
                }
              } catch(e){ try{ refreshConnBtn.disabled=false; }catch(e){} }
            });
          }
          var refreshVerBtn = document.getElementById('refresh-versions');
          if (refreshVerBtn) {
            refreshVerBtn.addEventListener('click', function(){
              try { refreshVerBtn.disabled = true; } catch(e){}
              // Always fetch fresh versions.json on manual refresh (force bypassing loadVersions cache)
              fetch('/versions.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(v){ try { renderVersionsPanel(v); } catch(e){} }).catch(function(e){ var el=document.getElementById('versions-status'); if(el) el.textContent='ERR: '+e; }).finally(function(){ try{ refreshVerBtn.disabled=false; }catch(e){} });
            });
          }
          // render fixed traceroute-to-uplink results when provided by /status
          try {
            if (status.trace_to_uplink && Array.isArray(status.trace_to_uplink) && status.trace_to_uplink.length) {
              var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
              if (trTab) trTab.parentElement.style.display = '';
              var hops = status.trace_to_uplink.map(function(h){ return { hop: h.hop || '', ip: h.ip || h.host || '', hostname: h.host || h.hostname || h.ip || '', ping: h.ping || '' }; });
              populateTracerouteTable(hops);
              var summaryEl = document.getElementById('traceroute-summary');
              if (summaryEl) summaryEl.textContent = 'Traceroute to ' + (status.trace_target || '') + ': ' + hops.length + ' hop(s)';
            }
          } catch(e) { /* ignore */ }
          // If backend provided a trace target but didn't include trace_to_uplink here,
          // ensure the traceroute input is populated and run once on initial load.
          try {
            if (!window._tracerouteAutoRunDone && status && status.trace_target && !(status.trace_to_uplink && status.trace_to_uplink.length)) {
              var trInput = document.getElementById('tr-host');
              if (trInput) {
                trInput.value = status.trace_target;
                window._tracerouteAutoRunDone = true;
                setTimeout(function(){ try { runTraceroute(); } catch(e){} }, 10);
              }
            }
          } catch(e) {}
        });
    });
  } catch(e) {
    // Fallback: try to load data without capabilities
    try {
      fetch('/status/lite', {cache: 'no-store'})
        .then(function(r) { return r.json(); })
        .then(function(status) {
          try { if (status.fetch_stats) populateFetchStats(status.fetch_stats); else populateFetchStats({}); } catch(e){}
          updateUI({
            hostname: status.hostname || 'Unknown',
            ip: status.ip || '',
            uptime: status.uptime_str || status.uptime || '',
            uptime_str: status.uptime_str || status.uptime || '',
            devices: status.devices || [],
            airos: status.airosdata || {},
            default_route: status.default_route || {},
            links: status.links || []
          });
          // Fallback traceroute initial display
          try {
            if (status.trace_to_uplink && Array.isArray(status.trace_to_uplink) && status.trace_to_uplink.length) {
              var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
              if (trTab) trTab.parentElement.style.display = '';
              var hops = status.trace_to_uplink.map(function(h){ return { hop: h.hop || '', ip: h.ip || h.host || '', hostname: h.host || h.hostname || h.ip || '', ping: h.ping || '' }; });
              populateTracerouteTable(hops);
              var summaryEl = document.getElementById('traceroute-summary');
              if (summaryEl) summaryEl.textContent = 'Traceroute to ' + (status.trace_target || '') + ': ' + hops.length + ' hop(s)';
            }
          } catch(e) {}
        });
    } catch(e2) {}
  }
}

// Lazy load OLSR links when OLSR tab clicked first time
var _olsrLoaded = false;
window.addEventListener('load', function(){
  var mt = document.getElementById('mainTabs');
  if (!mt) return;
  // Automatically start traceroute with predefined traceroute_to destination at initial load
  ensureTraceroutePreloaded();
  function ensureTraceroutePreloaded(){
    try { console.log('ensureTraceroutePreloaded: start'); } catch(e){}
  var tbody = document.querySelector('#tracerouteTable tbody');
    // Always use dedicated traceroute endpoint which returns a clean JSON payload.
    fetch('/status/traceroute', {cache:'no-store'})
      .then(function(r){
        if (!r || !r.ok) throw new Error('traceroute endpoint unavailable');
        return r.json();
      })
      .then(function(st){
          var summaryEl = document.getElementById('traceroute-summary');
          // Only replace content when we actually have hops to show. This avoids
          // clearing a manually-run traceroute or recent results when /status
          // does not include traceroute data.
          if (st && st.trace_to_uplink && Array.isArray(st.trace_to_uplink) && st.trace_to_uplink.length) {
            var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
            if (trTab) trTab.parentElement.style.display = '';
            var hops = st.trace_to_uplink.map(function(h){
              return {
                hop: h.hop || '',
                ip: h.ip || h.host || '',
                hostname: h.host || h.hostname || h.ip || '',
                ping: h.ping || ''
              };
            });
            populateTracerouteTable(hops);
            if (summaryEl) {
              summaryEl.textContent = 'Traceroute to ' + (st.trace_target || '') + ': ' + hops.length + ' hop(s)';
              summaryEl.setAttribute('aria-label', 'Traceroute summary: destination ' + (st.trace_target || '') + ', ' + hops.length + ' hops');
            }
          } else {
            // No precomputed hops available: only show a placeholder if the table
            // is currently empty (no manual run or previous data present).
            var hasRows = tbody && tbody.querySelectorAll && tbody.querySelectorAll('tr').length > 0;
            if (!hasRows) {
              if (tbody) {
                var tr = document.createElement('tr');
                var td = document.createElement('td');
                td.colSpan = 4;
                td.textContent = 'No traceroute data available.';
                tr.appendChild(td);
                tbody.appendChild(tr);
              }
              if (summaryEl) {
                summaryEl.textContent = 'Traceroute data not available.';
                summaryEl.setAttribute('aria-label', 'Traceroute summary: no data');
              }
            }
            try {
              if (!window._tracerouteAutoRunDone && st && st.trace_target) {
                var trInput = document.getElementById('tr-host');
                if (trInput) {
                  trInput.value = st.trace_target;
                  window._tracerouteAutoRunDone = true;
                  setTimeout(function(){ try { runTraceroute(); } catch(e){} }, 10);
                }
              }
            } catch(e) {}
          }
      })
      .catch(function(){
        // Treat any failure as 'no traceroute data' and keep UI quiet (no debug logs).
        var summaryEl = document.getElementById('traceroute-summary');
        if (tbody && !window._traceroutePopulatedAt) {
          var tr = document.createElement('tr');
          var td = document.createElement('td');
          td.colSpan = 4;
          td.textContent = 'No traceroute data available.';
          tr.appendChild(td);
          tbody.appendChild(tr);
        }
        if (summaryEl) {
          summaryEl.textContent = 'Traceroute data not available.';
          summaryEl.setAttribute('aria-label', 'Traceroute summary: no data');
        }
      });
  }
  mt.addEventListener('click', function(e){
    var a = e.target.closest('a'); if(!a) return;
    if (a.getAttribute('href') === '#tab-olsr' && !_olsrLoaded) {
  if (window._uiDebug) console.debug('OLSR tab clicked, lazy-loading /olsr/links');
      fetch('/olsr/links',{cache:'no-store'}).then(function(r){return r.json();}).then(function(o){
  if (window._uiDebug) console.debug('Received /olsr/links', o && (o.links? o.links.length : 'no links'));
        if (o.links && o.links.length) { populateOlsrLinksTable(o.links); }
        _olsrLoaded = true;
  }).catch(function(e){ if (window._uiDebug) console.debug('Failed to load /olsr/links', e); });
    } else if (a.getAttribute('href') === '#tab-traceroute') {
      ensureTraceroutePreloaded();
    } else if (a.getAttribute('href') === '#tab-log') {
      // lazy-load /log content
      if (window._logLoaded) return;
      var pre = document.getElementById('log-pre'); if (pre) pre.textContent = 'Loading...';
  fetch('/log?lines=100', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(obj){ try { var lines = obj && Array.isArray(obj.lines) ? obj.lines : []; renderLogArray(lines); window._logLoaded = true; } catch(e){ if (pre) pre.textContent = 'Error rendering log'; } }).catch(function(e){ if (pre) pre.textContent = 'Error loading /log: '+e; });
    }
  });
  // Refresh button support for OLSR Links
  var refreshLinksBtn = document.getElementById('refresh-links');
  if (refreshLinksBtn) {
    refreshLinksBtn.addEventListener('click', function(){
  // clear client-side cache to avoid mixing stale DOM rows with new data
  try { window._olsr_links = []; } catch(e) {}
  var statusEl = document.getElementById('links-status');
      try { refreshLinksBtn.disabled = true; } catch(e){}
  if (statusEl) statusEl.textContent = 'Refreshing…';
      // First force-update node_db (non-blocking by default). If server returns queued status
      // we proceed to fetch links immediately; if the client wants to ensure fresh names,
      // they can use ?wait=1 but that may block the UI.
      fetch('/nodedb/refresh',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(res){
        // If server indicated queued, we still continue to fetch links; the node_db
        // enrichment may arrive slightly later but UI remains responsive.
        return fetch('/olsr/links',{cache:'no-store'});
      }).then(function(r2){ return r2.json(); }).then(function(o){
        if (o.links && o.links.length) { populateOlsrLinksTable(o.links); }
        if (statusEl) statusEl.textContent = '';
        _olsrLoaded = true;
        showActionToast('Links refreshed', 2500);
  }).catch(function(e){ if (statusEl) statusEl.textContent = 'ERR'; showActionToast('Error refreshing links', 4000); }).finally(function(){ try{ refreshLinksBtn.disabled=false; }catch(e){} });
    });
  }
});

// console polyfill for older browsers
if (!window.console) {
  window.console = {
    log: function() {},
    error: function() {},
    warn: function() {},
    info: function() {}
  };
}

// trim polyfill for older browsers
if (!String.prototype.trim) {
  String.prototype.trim = function() {
    return this.replace(/^\s+|\s+$/g, '');
  };
}

// forEach polyfill for older browsers
if (!Array.prototype.forEach) {
  Array.prototype.forEach = function(callback, thisArg) {
    for (var i = 0; i < this.length; i++) {
      callback.call(thisArg, this[i], i, this);
    }
  };
}

// Array.prototype.map polyfill for older browsers
if (!Array.prototype.map) {
  Array.prototype.map = function(callback, thisArg) {
    var result = [];
    for (var i = 0; i < this.length; i++) {
      result[i] = callback.call(thisArg, this[i], i, this);
    }
    return result;
  };
}

// classList polyfill for older browsers (avoids Illegal invocation by using a getter with element closure)
;(function(){
  var testEl = document && document.documentElement;
  if (!testEl) return;
  if (!('classList' in testEl)) {
    function trim(str){ return str.replace(/^\s+|\s+$/g,''); }
    function makeTokenList(el){
      function getClasses(){ return trim(el.className || '').split(/\s+/).filter(function(c){ return c.length; }); }
      return {
        add: function(token){
          if (!token) return;
            var classes = getClasses();
            if (classes.indexOf && classes.indexOf(token) === -1) { classes.push(token); }
            else if (!classes.indexOf) { // old browsers without indexOf
              var found = false; for (var i=0;i<classes.length;i++){ if(classes[i]===token){ found=true; break; } }
              if(!found) classes.push(token);
            }
            el.className = classes.join(' ');
        },
        remove: function(token){
          if (!token) return;
          var classes = getClasses();
          var out = [];
          for (var i=0;i<classes.length;i++){ if(classes[i]!==token) out.push(classes[i]); }
          el.className = out.join(' ');
        },
        contains: function(token){
          if (!token) return false;
          var classes = getClasses();
          if (classes.indexOf) return classes.indexOf(token) !== -1;
          for (var i=0;i<classes.length;i++){ if (classes[i]===token) return true; }
          return false;
        },
        toggle: function(token){ if (this.contains(token)) { this.remove(token); return false; } else { this.add(token); return true; } }
      };
    }
    try {
      Object.defineProperty(Element.prototype, 'classList', { get: function(){ return makeTokenList(this); } });
    } catch(e) { // fallback for very old browsers without defineProperty on DOM prototypes
      // Overwrite via simple function returning fresh object each time
      if (HTMLElement && HTMLElement.prototype) {
        HTMLElement.prototype.__getClassList = function(){ return makeTokenList(this); };
        HTMLElement.prototype.classList = makeTokenList(document.createElement('div')); // dummy to avoid errors
      }
    }
  }
})();

// querySelector and querySelectorAll polyfills for older browsers
if (!document.querySelector) {
  document.querySelector = function(selector) {
    if (selector.charAt(0) === '#') {
      return document.getElementById(selector.substring(1));
    }
    return document.getElementsByTagName(selector.substring(1))[0];
  };
}

if (!document.querySelectorAll) {
  document.querySelectorAll = function(selector) {
    if (selector.charAt(0) === '#') {
      var el = document.getElementById(selector.substring(1));
      return el ? [el] : [];
    }
    return document.getElementsByTagName(selector.substring(1));
  };
}

// addEventListener polyfill for older browsers
if (!document.addEventListener) {
  document.addEventListener = function(event, callback) {
    if (event === 'DOMContentLoaded') {
      if (document.readyState === 'complete' || document.readyState === 'interactive') {
        callback();
      } else {
        document.attachEvent('onreadystatechange', function() {
          if (document.readyState === 'complete') {
            callback();
          }
          // wire tab lazy-load for connections and versions
          var connTabLink = document.querySelector('#mainTabs a[href="#tab-connections"]');
          if (connTabLink) connTabLink.addEventListener('click', function(){ loadConnections(); });
          var verTabLink = document.querySelector('#mainTabs a[href="#tab-versions"]');
          if (verTabLink) verTabLink.addEventListener('click', function(){ loadVersions(); });
        });
      }
    } else {
      document.attachEvent('on' + event, callback);
    }
  };
}

document.addEventListener('DOMContentLoaded', function() {
  // Simple, reliable tab system
  const tabLinks = document.querySelectorAll('#mainTabs a');
  const tabPanes = document.querySelectorAll('.tab-pane');

  function showTab(targetId) {
    // Hide all panes
    tabPanes.forEach(pane => {
      pane.style.display = 'none';
      pane.classList.remove('active');
      // Don't add hidden class here to avoid conflicts
    });

    // Remove active class from all links
    tabLinks.forEach(link => {
      link.parentElement.classList.remove('active');
    });

    // Show target pane
    const targetPane = document.querySelector(targetId);
    if (targetPane) {
      targetPane.style.display = 'block';
      targetPane.classList.add('active');
      targetPane.classList.remove('hidden'); // Ensure hidden class is removed

      // Add active class to target link
      const targetLink = document.querySelector(`#mainTabs a[href="${targetId}"]`);
      if (targetLink) {
        targetLink.parentElement.classList.add('active');
      }

      // Debug logging
      console.log('Tab switched to:', targetId);
      console.log('Active pane:', targetPane.id, 'display:', targetPane.style.display, 'classes:', targetPane.className);

      // Load content for this tab
      onTabShown(targetId);
    } else {
      console.error('Target pane not found:', targetId);
    }
  }

  // Add click handlers
  tabLinks.forEach(link => {
    link.addEventListener('click', function(e) {
      e.preventDefault();
      const targetId = this.getAttribute('href');
      showTab(targetId);
    });
  });

  // Ensure traceroute Run button is wired
  try {
    var runBtn = document.getElementById('tr-run');
    if (runBtn && !runBtn._wired) {
      runBtn.addEventListener('click', function(){ runTraceroute(); });
      runBtn._wired = true;
    }
  } catch(e) {}

  // Show initial active tab (only if not already visible)
  const initialActive = document.querySelector('#mainTabs li.active a');
  if (initialActive) {
    const initialTabId = initialActive.getAttribute('href');
    const initialPane = document.querySelector(initialTabId);
    if (initialPane && initialPane.style.display !== 'block') {
      showTab(initialTabId);
    } else {
      // Initial tab is already visible, just load its content
      onTabShown(initialTabId);
    }
  } else {
    // Fallback: show first tab
    showTab('#tab-main');
  }

  // ...existing code...

  // onTabShown: called whenever a tab becomes active
  function onTabShown(id) {
  try { if (window._uiDebug) { var pane = document.querySelector(id); if (pane) { var cs = window.getComputedStyle(pane); console.debug('onTabShown: id=', id, 'classes=', pane.className, 'computedDisplay=', cs.display, 'visible=', cs.display!=='none'); } } } catch(e){}
    console.log('Loading content for tab:', id);
    try {
      if (id === '#tab-log') {
        // if log not yet loaded, auto-refresh
        if (!window._logLoaded) refreshLog();
        // if already loaded and large, ensure virtualization is enabled
        try { if ((window._log_cache && window._log_cache.lines && window._log_cache.lines.length > 500) && !window._log_virtual_enabled) { enableVirtualLogs(); } } catch(e) {}
      }
      // Ensure other tabs lazy-load reliably even if some listeners didn't attach
      if (id === '#tab-main') {
        console.log('Main tab has static content');
        setTabLoaded('tab-main');
        // Load status data to populate main-host if not already loaded
        try {
          if (!window._statusLoaded || (Date.now() - (window._statusLoadedAt || 0) > 10000)) {
            safeFetchStatus({cache:'no-store'}).then(function(st){ try { if (st) { updateUI(st); } window._statusLoaded = true; window._statusLoadedAt = Date.now(); } catch(e){} }).catch(function(){});
          }
        } catch(e) {}
      }
      if (id === '#tab-contact') {
        // Contact tab has static content, ensure it's visible
        console.log('Contact tab has static content');
        setTabLoaded('tab-contact');
        try {
          var contactPane = document.getElementById('tab-contact');
          if (contactPane) {
            contactPane.style.display = 'block';
          }
        } catch(e) {}
      }
      if (id === '#tab-status') {
        // fetch full status and populate UI if not already loaded recently
        console.log('Loading status tab content...');
        setTabLoading('tab-status');
        try {
            if (!window._statusLoaded || (Date.now() - (window._statusLoadedAt || 0) > 10000)) {
            console.log('Fetching /status/lite API...');
            safeFetchStatus({cache:'no-store'}).then(function(st){ try { if (st) { console.log('Status data received:', Object.keys(st)); updateUI(st); if (st.links && Array.isArray(st.links) && st.links.length) { try { window._olsr_links = st.links.slice(); populateOlsrLinksTable(window._olsr_links); } catch(e){} } } window._statusLoaded = true; window._statusLoadedAt = Date.now(); setTabLoaded('tab-status'); } catch(e){ setTabError('tab-status'); } }).catch(function(){ setTabError('tab-status'); });
          } else {
            console.log('Status already loaded recently');
            setTabLoaded('tab-status');
          }
        } catch(e) { setTabError('tab-status'); }
      }
      if (id === '#tab-olsr') {
        console.log('Loading OLSR links tab content...');
        setTabLoading('tab-olsr');
        try {
          // If we haven't loaded yet, or the table is present but empty, fetch and populate.
          var olsrtbody = document.querySelector('#olsrLinksTable tbody');
          var needFetch = !window._olsrLoaded || !olsrtbody || (olsrtbody && olsrtbody.querySelectorAll('tr').length === 0);
          console.log('OLSR tab - needFetch:', needFetch, 'tbody exists:', !!olsrtbody, 'current rows:', olsrtbody ? olsrtbody.querySelectorAll('tr').length : 0);
          if (needFetch) {
            try { if (olsrtbody) { olsrtbody.innerHTML = '<tr><td colspan="12" class="text-muted">Loading…</td></tr>'; } } catch(e){}
            console.log('Fetching /olsr/links API...');
            fetch('/olsr/links', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(o){ try { if (o && o.links) { console.log('OLSR links received:', o.links.length, 'links'); window._olsr_links = o.links.slice(); populateOlsrLinksTable(window._olsr_links); setTabLoaded('tab-olsr'); } window._olsrLoaded = true; } catch(e){ setTabError('tab-olsr'); } }).catch(function(){
              // Show fallback content when API fails
              console.error('Failed to fetch OLSR links');
              if (olsrtbody) {
                olsrtbody.innerHTML = '<tr><td colspan="12" class="text-muted">No OLSR data available. This tab requires a running OLSR daemon.</td></tr>';
              }
              setTabError('tab-olsr');
            });
          } else {
            console.log('OLSR links already loaded');
            setTabLoaded('tab-olsr');
          }
        } catch(e) { setTabError('tab-olsr'); }
      }
      if (id === '#tab-connections') {
        console.log('Loading connections tab content...');
        setTabLoading('tab-connections');
        try {
          if (!window._connectionsLoadedGlobal) {
            console.log('Fetching connections APIs...');
            Promise.all([ fetch('/nodedb.json', {cache:'no-store'}).then(function(r){return r.json();}).catch(function(){return {}; }), fetch('/connections.json', {cache:'no-store'}).then(function(r){return r.json();}).catch(function(){return {}; }) ]).then(function(results){ try { var ndb = results[0] || {}; var con = results[1] || {}; console.log('Connections data received - nodedb keys:', Object.keys(ndb).length, 'connections:', Array.isArray(con) ? con.length : 'not array'); renderConnectionsTable(con, ndb); window._connectionsLoadedGlobal = true; setTabLoaded('tab-connections'); } catch(e){ setTabError('tab-connections'); } }).catch(function(){
              // Show fallback content when API fails
              console.error('Failed to fetch connections data');
              var tbody = document.querySelector('#connectionsTable tbody');
              if (tbody) {
                tbody.innerHTML = '<tr><td colspan="6" class="text-muted">No connection data available. This tab requires backend API access.</td></tr>';
              }
              setTabError('tab-connections');
            });
          } else {
            console.log('Connections already loaded');
            setTabLoaded('tab-connections');
          }
        } catch(e) { setTabError('tab-connections'); }
      }
      if (id === '#tab-versions') {
        console.log('Loading versions tab content...');
        setTabLoading('tab-versions');
        try {
          if (!window._versionsLoadedGlobal) {
            console.log('Fetching /versions.json API...');
            fetch('/versions.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(v){ try { console.log('Versions data received:', Object.keys(v)); renderVersionsPanel(v); window._versionsLoadedGlobal = true; setTabLoaded('tab-versions'); } catch(e){ setTabError('tab-versions'); } }).catch(function(){
              // Show fallback content when API fails
              console.error('Failed to fetch versions data');
              var wrap = document.getElementById('versions-wrap');
              if (wrap) {
                wrap.innerHTML = '<div class="alert alert-info">Version information not available. This tab requires backend API access.</div>';
              }
              setTabError('tab-versions');
            });
          } else {
            console.log('Versions already loaded');
            setTabLoaded('tab-versions');
          }
        } catch(e) { setTabError('tab-versions'); }
      }
      if (id === '#tab-traceroute') {
        try {
          // Use the dedicated traceroute endpoint which returns a clean JSON payload
          // and avoid fetching /status here because /status may omit trace_to_uplink
          // and would otherwise overwrite a valid traceroute table.
          fetch('/status/traceroute', {cache:'no-store'})
            .then(function(r){ if (!r || !r.ok) throw new Error('traceroute endpoint unavailable'); return r.json(); })
            .then(function(st){
              try {
                if (st && st.trace_to_uplink && Array.isArray(st.trace_to_uplink) && st.trace_to_uplink.length) {
                  var hops = st.trace_to_uplink.map(function(h){ return { hop: h.hop || '', ip: h.ip || h.host || '', hostname: h.host || h.hostname || h.ip || '', ping: h.ping || '' }; });
                  populateTracerouteTable(hops);
                  return;
                }
                // If server provided only a trace_target (no precomputed hops), populate input and maybe auto-run
                if (st && st.trace_target && !(st.trace_to_uplink && st.trace_to_uplink.length)) {
                  try {
                    var trInput = document.getElementById('tr-host');
                    if (trInput) {
                      trInput.value = st.trace_target;
                      if (!window._traceroutePopulatedAt && !window._tracerouteAutoRunDone) {
                        window._tracerouteAutoRunDone = true;
                        setTimeout(function(){ try { runTraceroute(); } catch(e){} }, 10);
                      }
                    }
                  } catch(e) {}
                }
              } catch(e) { /* ignore per-tab errors */ }
            }).catch(function(){
              // Only show fallback content if traceroute table is empty to avoid
              // clobbering results populated by a recent /status/traceroute or manual run.
              var tbody = document.querySelector('#tracerouteTable tbody');
              var hasRows = tbody && tbody.querySelectorAll && tbody.querySelectorAll('tr').length > 0;
              if (!hasRows) {
                if (tbody) {
                  tbody.innerHTML = '<tr><td colspan="4" class="text-muted">Traceroute data not available. This tab requires backend API access.</td></tr>';
                }
              }
            });
        } catch(e) {}
      }
      if (id === '#tab-neighbors') {
        try {
          // Show fallback content for neighbors tab
          var tbody = document.querySelector('#neighborsTable tbody');
          if (tbody && tbody.querySelectorAll('tr').length === 0) {
            tbody.innerHTML = '<tr><td colspan="7" class="text-muted">Neighbor data not available. This tab requires a running OLSR daemon.</td></tr>';
          }
        } catch(e) {}
      }
    } catch(e) {}
  }

  detectPlatformAndLoad();
  try { _startStatsPoll(); } catch(e) {}
  // Badge interactivity: clicking a badge forces a stats refresh and shows timestamp
  function _setBadgeInteractive(){
    try {
      var bn = document.getElementById('badge-nodes'); var br = document.getElementById('badge-routes');
      function setClick(el){ if(!el) return; if(el._wired) return; el.style.cursor='pointer'; el.addEventListener('click', function(){ try { el.textContent = el.textContent + ' • refreshing…'; fetch('/status/stats', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){ try { if (s && typeof s.olsr_nodes_count !== 'undefined') { var n = document.getElementById('badge-nodes'); if(n) n.textContent = 'Nodes: '+String(s.olsr_nodes_count); } if (s && typeof s.olsr_routes_count !== 'undefined') { var rEl = document.getElementById('badge-routes'); if(rEl) rEl.textContent = 'Routes: '+String(s.olsr_routes_count); } var now = new Date(); el.title = 'Last refreshed: ' + now.toLocaleString(); }catch(e){} }).catch(function(){ /* ignore */ }); } catch(e){} }); el._wired = true; }
      setClick(bn); setClick(br);
    } catch(e) {}
  }
  // wire badge interactivity once DOM ready
  document.addEventListener('DOMContentLoaded', function(){ try{ _setBadgeInteractive(); }catch(e){} });

  // Dropdown: toggle and content population
  function _wireStatusDropdown(){
    try {
      var wrap = document.getElementById('status-dropdown-wrap'); if (!wrap) return;
      var badges = document.getElementById('status-badges'); var menu = document.getElementById('status-dropdown-menu'); var details = document.getElementById('status-details'); var ts = document.getElementById('status-dropdown-ts'); var refresh = document.getElementById('status-dropdown-refresh');
      function closeMenu(){ if (!menu) return; menu.setAttribute('aria-hidden','true'); }
      function openMenu(){ if (!menu) return; menu.setAttribute('aria-hidden','false'); }
      // toggle on badge click
      if (badges && !badges._wired) {
        badges.addEventListener('click', function(){ try { if (menu && menu.getAttribute('aria-hidden') === 'false') { closeMenu(); } else { openMenu(); _populateDropdown(); } } catch(e){} }); badges._wired = true; }
      // close when clicking outside
      document.addEventListener('click', function(e){ try { if (!wrap.contains(e.target)) { closeMenu(); } } catch(e){} });
      if (refresh) refresh.addEventListener('click', function(){ _populateDropdown(); });
      function applyThresholds(s){
        try {
          var thresholds = (s && s.thresholds) ? s.thresholds : {};
          var q_warn = thresholds.queue_warn || 50; var q_crit = thresholds.queue_crit || 200;
          // nodes / routes threshold examples: user may provide nodes_warn/nodes_crit
          var n_warn = thresholds.nodes_warn || 100; var n_crit = thresholds.nodes_crit || 10; // default: low nodes critical
          var r_warn = thresholds.routes_warn || 200; var r_crit = thresholds.routes_crit || 1000;
          var bn = document.getElementById('badge-nodes'); var br = document.getElementById('badge-routes');
          if (bn && typeof s.olsr_nodes_count !== 'undefined') {
            var n = Number(s.olsr_nodes_count) || 0; bn.classList.remove('warn','crit'); if (n <= n_crit) bn.classList.add('crit'); else if (n <= n_warn) bn.classList.add('warn');
          }
          if (br && typeof s.olsr_routes_count !== 'undefined') {
            var r = Number(s.olsr_routes_count) || 0; br.classList.remove('warn','crit'); if (r >= r_crit) br.classList.add('crit'); else if (r >= r_warn) br.classList.add('warn');
          }
        } catch(e) {}
      }
      // populate dropdown content
      function _populateDropdown(){
        if (!details) return;
        details.textContent = 'Loading…';
        fetch('/status/stats', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){ try {
            // Use the reusable card renderer to render dropdown details
            try {
              renderCard({
                container: details,
                title: '',
                panelClass: 'panel panel-default',
                bodyStyle: 'padding:8px',
                render: function(body, headerRight) {
                  // top badges for nodes/routes
                  var top = document.createElement('div'); top.style.display = 'flex'; top.style.gap = '8px'; top.style.alignItems = 'center';
                  var nodesCount = (typeof s.olsr_nodes_count !== 'undefined') ? s.olsr_nodes_count : '-';
                  var routesCount = (typeof s.olsr_routes_count !== 'undefined') ? s.olsr_routes_count : '-';
                  var bn = document.createElement('div'); bn.className = 'metric-badge'; bn.textContent = 'Nodes: ' + String(nodesCount); top.appendChild(bn);
                  var br = document.createElement('div'); br.className = 'metric-badge'; br.textContent = 'Routes: ' + String(routesCount); top.appendChild(br);
                  body.appendChild(top);

                  // Fetch queue section
                  if (s.fetch_stats) {
                    var sep = document.createElement('div'); sep.style.marginTop = '8px'; sep.style.fontWeight = '600'; sep.textContent = 'Fetch Queue'; body.appendChild(sep);
                    var qwrap = document.createElement('div'); qwrap.style.marginTop = '6px';
                    function val(name, alt){ return s.fetch_stats[name] || s.fetch_stats[alt] || 0; }
                    var queued = val('queued_count','queue_length') || val('queued','queue_len');
                    var processing = val('processing_count','in_progress') || val('processing');
                    var dropped = val('dropped_count','dropped') || val('dropped');
                    var queuedDiv = document.createElement('div'); queuedDiv.textContent = 'Queued: ' + String(queued); qwrap.appendChild(queuedDiv);
                    var procDiv = document.createElement('div'); procDiv.textContent = 'Processing: ' + String(processing); qwrap.appendChild(procDiv);
                    var dropDiv = document.createElement('div'); dropDiv.textContent = 'Dropped: ' + String(dropped); qwrap.appendChild(dropDiv);
                    body.appendChild(qwrap);
                  }
                }
              });
            } catch(e) {
              details.textContent = 'Error rendering details';
            }

            if (ts) { var now = new Date(); ts.textContent = now.toLocaleTimeString(); ts.title = now.toString(); }
            applyThresholds(s);
          } catch(e){ details.textContent = 'Error rendering details'; } }).catch(function(){ details.textContent = 'Error fetching stats'; });
      }
    } catch(e) {}
  }
  document.addEventListener('DOMContentLoaded', function(){ try{ _wireStatusDropdown(); }catch(e){} });
  // ...existing code...
});

// --- Log tab helpers ---
function parseLogLine(line) {
  // naive parse: try to extract timestamp and level prefix like "2025-09-09 12:00:00 [INFO] ..."
  try {
    var m = line.match(/^\s*(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})\s*(?:\[([^\]]+)\])?\s*(.*)$/);
    if (m) return { ts: m[1], level: (m[2]||'').toUpperCase(), msg: m[3] };
  } catch(e) {}
  return { ts: '', level: '', msg: line };
}

// Render log lines (paginated). Expects an array of lines (strings) in data.lines
window._log_cache = window._log_cache || { lines: [], filtered: [], page: 0, pageSize: 20 };
function renderLogPage() {
  var tbody = document.getElementById('log-tbody'); if (!tbody) return;
  var pagerWrap = document.getElementById('log-pager');
  var cache = window._log_cache; var lines = cache.filtered.length ? cache.filtered : cache.lines;
  var total = lines.length || 0;
  var pages = Math.max(1, Math.ceil(total / cache.pageSize));
  if (cache.page >= pages) cache.page = pages - 1;
  var start = cache.page * cache.pageSize; var end = Math.min(total, start + cache.pageSize);
  var slice = lines.slice(start, end);
  // render rows into tbody
  tbody.innerHTML = '';
  if (!slice.length) {
    var tr = document.createElement('tr'); var td = document.createElement('td'); td.colSpan = 3; td.className = 'text-muted'; td.style.padding = '12px'; td.textContent = '(no log lines)'; tr.appendChild(td); tbody.appendChild(tr);
  } else {
    slice.forEach(function(ln){ var p = parseLogLine(ln); var tr = document.createElement('tr'); var tdTs = document.createElement('td'); tdTs.style.color='#666'; tdTs.style.fontSize='11px'; tdTs.textContent = p.ts || ''; var tdLevel = document.createElement('td'); tdLevel.style.fontWeight='600'; tdLevel.textContent = p.level || ''; if (p.level==='ERROR' || p.level==='ERR' || p.level==='CRITICAL') tdLevel.style.color='#a94442'; else if (p.level==='WARN' || p.level==='WARNING') tdLevel.style.color='#8a6d3b'; else if (p.level==='INFO') tdLevel.style.color='#31708f'; else if (p.level==='DEBUG') tdLevel.style.color='#3a3a3a'; var tdMsg = document.createElement('td'); tdMsg.style.whiteSpace='pre-wrap'; tdMsg.textContent = p.msg || ''; tr.appendChild(tdTs); tr.appendChild(tdLevel); tr.appendChild(tdMsg); tbody.appendChild(tr); });
  }
  // pager html
  if (pagerWrap) {
    pagerWrap.innerHTML = '';
    var info = document.createElement('div'); info.textContent = 'Showing '+(start+1)+'-'+(end)+' of '+total+' entries';
    var controls = document.createElement('div'); controls.style.display='inline-block'; controls.style.marginLeft='12px';
    var prev = document.createElement('button'); prev.id='log-prev'; prev.className='btn btn-xs btn-default'; prev.textContent='Prev'; if (cache.page===0) prev.disabled=true;
    var span = document.createElement('span'); span.style.margin='0 6px'; span.textContent = 'Page '+(cache.page+1)+'/'+pages;
    var next = document.createElement('button'); next.id='log-next'; next.className='btn btn-xs btn-default'; next.textContent='Next'; if (cache.page+1>=pages) next.disabled=true;
    controls.appendChild(prev); controls.appendChild(span); controls.appendChild(next);
    pagerWrap.appendChild(info); pagerWrap.appendChild(controls);
    // wire buttons
    try { prev.addEventListener('click', function(){ if (window._log_cache.page>0) { window._log_cache.page--; renderLogPage(); } }); next.addEventListener('click', function(){ if ((window._log_cache.page+1)*window._log_cache.pageSize < (window._log_cache.filtered.length || window._log_cache.lines.length)) { window._log_cache.page++; renderLogPage(); } }); } catch(e){}
  }
  // Ensure log tab pane is visible after rendering
  try { var pane = document.getElementById('tab-log'); if (pane) { pane.classList.remove('hidden'); pane.style.display = ''; } } catch(e){}
}

function renderLogArray(lines) {
  window._log_cache.lines = Array.isArray(lines) ? lines.slice().reverse() : [];
  window._log_cache.filtered = [];
  window._log_cache.page = 0;
  var container = document.getElementById('log-table-wrap');
  if (!container) {
    // fallback: show log lines in log-pre if table container is missing
    var pre = document.getElementById('log-pre');
    if (pre) {
      pre.textContent = lines.length ? lines.join('\n') : '(no log lines)';
    }
    return;
  }
  // If logs are large, enable virtualized rendering for CPU/memory efficiency
  try {
    var len = window._log_cache.lines.length || 0;
    var threshold = 500;
    if (len > threshold) {
      enableVirtualLogs();
      virtualRender();
      try { var pager = document.getElementById('log-pager'); if (pager) pager.innerHTML = ''; } catch(e) {}
      return;
    }
  } catch(e) {}
  disableVirtualLogs();
  renderLogPage();
}

// --- Virtualized log table helpers (lightweight) ---
function enableVirtualLogs() {
  if (window._log_virtual_enabled) return;
  var container = document.getElementById('log-table-wrap');
  if (!container) return;
  window._log_virtual_enabled = true;
  // clear existing content
  container.innerHTML = '';
  container.style.overflow = 'auto';
  container.style.height = container.style.height || '400px';

  // create table skeleton
  var table = document.createElement('table');
  table.className = 'table table-condensed';
  table.id = 'log-table-virtual';
  var thead = document.createElement('thead');
  thead.innerHTML = '<tr><th style="width:160px">Timestamp</th><th style="width:100px">Level</th><th>Message</th></tr>';
  table.appendChild(thead);
  var tbody = document.createElement('tbody');
  tbody.id = 'log-virtual-tbody';
  tbody.style.position = 'relative';
  table.appendChild(tbody);

  // spacer to emulate full height
  var spacer = document.createElement('div');
  spacer.id = 'log-virtual-spacer';
  spacer.style.width = '1px';
  spacer.style.height = '0px';

  container.appendChild(table);
  container.appendChild(spacer);
  container.addEventListener('scroll', virtualScrollHandler);
}

function disableVirtualLogs() {
  if (!window._log_virtual_enabled) return;
  var container = document.getElementById('log-table-wrap');
  if (!container) return;
  window._log_virtual_enabled = false;
  try { container.removeEventListener('scroll', virtualScrollHandler); } catch(e) {}
  // restore basic table
  container.innerHTML = '';
  var table = document.createElement('table');
  table.className = 'table table-condensed';
  table.id = 'log-table';
  table.innerHTML = '<thead><tr><th style="width:160px">Timestamp</th><th style="width:100px">Level</th><th>Message</th></tr></thead><tbody id="log-tbody"><tr><td colspan="3" class="text-muted" style="padding:12px">(no log lines)</td></tr></tbody>';
  container.appendChild(table);
}

function virtualScrollHandler() {
  if (window._log_virtual_raf) return;
  window._log_virtual_raf = requestAnimationFrame(function(){ window._log_virtual_raf = null; virtualRender(); });
}

function virtualRender() {
  var container = document.getElementById('log-table-wrap');
  var spacer = document.getElementById('log-virtual-spacer');
  var tbody = document.getElementById('log-virtual-tbody');
  if (!container || !spacer || !tbody) return;
  var lines = (window._log_cache && window._log_cache.lines) ? window._log_cache.lines : [];
  var total = lines.length;
  var rowHeight = 26; // estimated row height in px
  spacer.style.height = (total * rowHeight) + 'px';
  var scrollTop = container.scrollTop || 0;
  var viewportHeight = container.clientHeight || 400;
  var firstIdx = Math.max(0, Math.floor(scrollTop / rowHeight) - 5);
  var visibleCount = Math.ceil(viewportHeight / rowHeight) + 10;
  var lastIdx = Math.min(total, firstIdx + visibleCount);
  // position tbody
  tbody.style.transform = 'translateY(' + (firstIdx * rowHeight) + 'px)';
  // render visible rows
  tbody.innerHTML = '';
  for (var i = firstIdx; i < lastIdx; i++) {
    var ln = lines[i] || '';
    var p = parseLogLine(ln);
    var tr = document.createElement('tr');
    var tdTs = document.createElement('td'); tdTs.style.color='#666'; tdTs.style.fontSize='11px'; tdTs.textContent = p.ts || '';
    var tdLevel = document.createElement('td'); tdLevel.style.fontWeight='600'; tdLevel.textContent = p.level || '';
    if (p.level==='ERROR' || p.level==='ERR' || p.level==='CRITICAL') tdLevel.style.color='#a94442'; else if (p.level==='WARN' || p.level==='WARNING') tdLevel.style.color='#8a6d3b'; else if (p.level==='INFO') tdLevel.style.color='#31708f'; else if (p.level==='DEBUG') tdLevel.style.color='#3a3a3a';
    var tdMsg = document.createElement('td'); tdMsg.style.whiteSpace='pre-wrap'; tdMsg.textContent = p.msg || '';
    tr.appendChild(tdTs); tr.appendChild(tdLevel); tr.appendChild(tdMsg);
    tbody.appendChild(tr);
  }
}

function refreshLog() {
  var btn = document.getElementById('refresh-log');
  var classBtns = document.querySelectorAll('.refresh-log-btn');
  var status = document.getElementById('log-status');
  try {
    if (btn) btn.disabled = true;
    if (classBtns && classBtns.length) classBtns.forEach(function(b){ b.disabled = true; });
    if (status) status.textContent = 'Refreshing...';
  } catch(e){}
  // Request all available log lines; backend will limit automatically
  fetch('/log', {cache:'no-store'})
    .then(function(r){ return r.json(); })
    .then(function(obj){
      try {
        // Accept both array and object responses from backend
        var arr = [];
        if (Array.isArray(obj)) {
          arr = obj;
        } else if (obj && Array.isArray(obj.lines)) {
          arr = obj.lines;
        } else if (obj && typeof obj === 'object' && Object.keys(obj).length > 0) {
          // If backend returns an object with numbered keys (log lines)
          arr = Object.values(obj);
        }
        renderLogArray(arr);
        window._logLoaded = true;
        if (status) status.textContent = '';
      } catch(e) {
        var tbody = document.getElementById('log-tbody');
        if (tbody) tbody.innerHTML = '<tr><td colspan="3" class="text-danger">Error rendering log</td></tr>';
        if (status) status.textContent = 'Error';
      }
    })
    .catch(function(e){
      var tbody = document.getElementById('log-tbody');
      if (tbody) tbody.innerHTML = '<tr><td colspan="3" class="text-danger">Error loading /log: '+escapeHtml(String(e))+'</td></tr>';
      if (status) status.textContent = 'Error';
    })
    .finally(function(){
      try {
        if (btn) btn.disabled = false;
        if (classBtns && classBtns.length) classBtns.forEach(function(b){ b.disabled = false; });
      } catch(e){}
    });
}

// escape HTML simple helper
function escapeHtml(s){ return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

// wire refresh button on load
window.addEventListener('load', function(){
  // support both legacy ID and new class for refresh buttons
  try {
    var r = document.getElementById('refresh-log'); if (r) r.addEventListener('click', function(){ refreshLog(); });
  } catch(e){}
  try {
    var r2 = document.querySelectorAll('.refresh-log-btn'); if (r2 && r2.length) { r2.forEach(function(b){ b.addEventListener('click', function(){ refreshLog(); }); }); }
  } catch(e){}
  var f = document.getElementById('log-filter'); if (f) { f.addEventListener('keydown', function(e){ if (e.key === 'Enter') { e.preventDefault(); refreshLog(); } }); }
});
// filter wiring
document.addEventListener('DOMContentLoaded', function(){ var f = document.getElementById('log-filter'); if (!f) return; f.addEventListener('input', function(){ var q = (f.value||'').trim().toLowerCase(); if (!q) { window._log_cache.filtered = []; window._log_cache.page = 0; renderLogPage(); return; } var lines = window._log_cache.lines || []; window._log_cache.filtered = lines.filter(function(l){ return l.toLowerCase().indexOf(q) !== -1; }); window._log_cache.page = 0; renderLogPage(); }); });

// Enable simple client-side sorting for tables whose TH elements contain a data-key attribute
function enableTableSorting() {
  function sortTable(table, colIndex, asc) {
    var tbody = table.tBodies[0]; if (!tbody) return;
    var rows = Array.prototype.slice.call(tbody.querySelectorAll('tr'));
    rows.sort(function(a,b){
      var ta = (a.children[colIndex] && a.children[colIndex].textContent) ? a.children[colIndex].textContent.trim() : '';
      var tb = (b.children[colIndex] && b.children[colIndex].textContent) ? b.children[colIndex].textContent.trim() : '';
      // try numeric compare
      var na = parseFloat(ta.replace(/[^0-9\-\.]/g, ''));
      var nb = parseFloat(tb.replace(/[^0-9\-\.]/g, ''));
      if (!isNaN(na) && !isNaN(nb)) {
        return asc ? na - nb : nb - na;
      }
      // fallback: localeCompare
      return asc ? ta.localeCompare(tb) : tb.localeCompare(ta);
    });
    // re-append rows
    rows.forEach(function(r){ tbody.appendChild(r); });
  }

  var tables = document.querySelectorAll('table');
  tables.forEach(function(table){
    var ths = table.querySelectorAll('th');
    ths.forEach(function(th, idx){
      if (th.getAttribute('data-key')) {
        th.style.cursor = 'pointer';
        var asc = true;
        th.addEventListener('click', function(){
          sortTable(table, idx, asc);
          // toggle asc/desc and show a simple mark
          var mark = th.querySelector('.sort-mark');
          if (!mark) { mark = document.createElement('span'); mark.className='sort-mark'; th.appendChild(mark); }
          mark.textContent = asc ? ' ▲' : ' ▼';
          asc = !asc;
        });
      }
    });
  });
}

// initialize sorting behaviors early
try { enableTableSorting(); } catch(e) {}

// Populate dynamic nav entries (host + login) after status load
function populateNavHost(host, ip) {
  var el = document.getElementById('nav-host');
  if (!el) return;
  // Ensure the nav shows "IP - hostname". Preserve any icon markup if present.
  try {
    var hostnameSpan = el.querySelector('#hostname');
    // detect an existing icon (a glyphicon inside a wrapper span)
    var iconElem = el.querySelector('.glyphicon');
    var iconHtml = '';
    if (iconElem) {
      // prefer the immediate parent so we keep styling wrappers
      if (iconElem.parentNode && iconElem.parentNode.outerHTML) iconHtml = iconElem.parentNode.outerHTML; else iconHtml = iconElem.outerHTML || '';
    }
    // Show IP then hostname; if the hostname contains the literal token 'nodename'
    // try to replace it with the real nodename from the cached nodedb (if available).
    var ipPart = (ip ? ip : '');
    var displayHost = (host || '');
    try {
      if (ipPart && window._nodedb_cache && window._nodedb_cache[ipPart] && window._nodedb_cache[ipPart].n) {
        var realNode = window._nodedb_cache[ipPart].n;
        if (realNode && typeof displayHost === 'string') {
          displayHost = displayHost.replace(/\bnodename\b/g, realNode);
        }
      }
    } catch(e) { /* ignore lookup errors */ }

    // Update or create hostname span without touching other children (preserve debug button etc.)
    var hostnameSpan = el.querySelector('#hostname');
    if (!hostnameSpan) {
      hostnameSpan = document.createElement('span');
      hostnameSpan.id = 'hostname';
      // append at the end by default
      el.appendChild(hostnameSpan);
    }
    try { hostnameSpan.textContent = displayHost || ''; } catch(e) { hostnameSpan.innerText = displayHost || ''; }
    // ensure an icon wrapper exists if needed
    if (iconHtml) {
      var iconWrap = el.querySelector('.fetch-icon');
      if (!iconWrap) {
        iconWrap = document.createElement('span');
        iconWrap.className = 'fetch-icon';
        // insert before hostnameSpan if possible
        el.insertBefore(iconWrap, hostnameSpan);
      }
      iconWrap.innerHTML = iconHtml;
    } else {
      var iconWrapRem = el.querySelector('.fetch-icon'); if (iconWrapRem) iconWrapRem.parentNode.removeChild(iconWrapRem);
    }
    // Also update compact header host element if present
  // Do not overwrite the brand 'main-host' which is reserved for compact IP display.
  // Update page-level host element instead.
  try { var pageHostEl = document.getElementById('main-host-page'); if (pageHostEl) { pageHostEl.textContent = displayHost || ''; } } catch(e) {}
  // Also update the page-level host element where present
  try { var pageHost = document.getElementById('main-host-page'); if (pageHost) { pageHost.textContent = displayHost || ''; } } catch(e) {}
  } catch (e) {
    // fallback simple text
    el.textContent = (ip ? ip + ' - ' : '') + (host || '');
  }
  // make nav-host clickable to open fetch-debug modal (non-destructive)
  try {
    el.style.cursor = 'pointer';
    // attach click handler only once
    if (!el._fetchDebugHandlerAttached) {
      el.addEventListener('click', function(){
        var body = document.getElementById('fetch-debug-body');
        if (!body) return;
        body.textContent = 'Loading...';
        fetch('/fetch_debug', {cache:'no-store'}).then(function(r){ return r.text(); }).then(function(t){ try { var obj = JSON.parse(t); body.textContent = JSON.stringify(obj, null, 2); } catch(e) { body.textContent = t; } }).catch(function(e){ body.textContent = 'ERR: '+e; });
        showModal('fetch-debug-modal');
      });
      el._fetchDebugHandlerAttached = true;
    }
  } catch(e) {}
}

function setLoginLink(url, port) {
  var nav = document.getElementById('nav-login');
  var link = document.getElementById('nav-login-link');
  if (!nav || !link) return;
  if (!url) { nav.style.display = 'none'; return; }
  // if port provided, include it
  link.href = url;
  nav.style.display = '';
}

function renderConnectionsTable(c, nodedb) {
  var tbody = document.querySelector('#connectionsTable tbody');
  tbody.innerHTML = '';
  if (!c || !c.ports) return;
  // Build simple IP->hostname map if nodedb given as array
  // Build IP -> { hostname, node, any } mapping to allow separating hostname vs node name
  var ipToHost = {};
  if (nodedb) {
    if (Array.isArray(nodedb)) {
      nodedb.forEach(function(entry){
        if (!entry) return;
        var ip = entry.ipv4 || entry.ip || '';
        if (!ip) return;
  // nodedb array entries may provide device/name/id fields.
  // Prefer device (d) or hostname field for the Hostname column,
  // and the node/group name (n or name) for the Node column.
  var hostname = entry.d || entry.hostname || entry.h || '';
  var node = entry.name || entry.n || '';
  var any = hostname || node || entry.m || '';
        ipToHost[ip] = { hostname: hostname, node: node, any: any };
      });
    } else if (typeof nodedb === 'object') {
      Object.keys(nodedb).forEach(function(k){
        var v = nodedb[k]; if (!v) return;
        // The live nodedb uses keys like 'n' (node name), 'd' (device/hostname),
        // 'h' (host hint) and 'm' (master). Prefer device/host fields for Hostname.
        var hostname = v.d || v.hostname || v.h || '';
        var node = v.name || v.n || '';
        var any = hostname || node || v.m || '';
        ipToHost[k] = { hostname: hostname, node: node, any: any };
      });
    }
  }
  c.ports.forEach(function(p){
    var tr = document.createElement('tr');
    function td(val){ var td=document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(p.port));
    tr.appendChild(td(p.bridge || ''));
    tr.appendChild(td((p.macs || []).join('<br>')));
    tr.appendChild(td((p.ips || []).join('<br>')));
    /* Resolve Hostname and Node separately from nodedb mapping (prefer explicit hostname field) */
    var hostnameVal = '';
    var nodeNames = [];
    if (p.ips && p.ips.length > 0) {
      p.ips.forEach(function(ip){
        var m = ipToHost[ip];
        if (!m) return;
        // prefer an explicit hostname for the Hostname column
        if (!hostnameVal && m.hostname) hostnameVal = m.hostname;
        // collect node names (if present)
        if (m.node) nodeNames.push(m.node);
        // if no explicit node name but we have a hostname and no hostnameVal yet, fallback will have used it
      });
      // if we still have no hostnameVal but some mapping exists, fallback to first available 'any' value
      if (!hostnameVal) {
        for (var ii=0; ii<p.ips.length; ii++) { var mm = ipToHost[p.ips[ii]]; if (mm && mm.any) { hostnameVal = mm.any; break; } }
      }
    }
    // dedupe nodeNames
    if (nodeNames.length) {
      var uniq = [];
      nodeNames.forEach(function(nm){ if (uniq.indexOf(nm) === -1) uniq.push(nm); });
      nodeNames = uniq;
    }
    // Build full hostname: resolved hostname + ".<node>.wien.funkfeuer.at" when node present
    var fullHostname = (hostnameVal || '').toString();
    var primaryNode = (nodeNames && nodeNames.length) ? nodeNames[0] : '';
    // sanitize trailing dot
    if (fullHostname && fullHostname.slice(-1) === '.') fullHostname = fullHostname.slice(0, -1);
    if (primaryNode) {
      if (fullHostname) fullHostname = fullHostname + '.' + primaryNode + '.wien.funkfeuer.at';
      else fullHostname = primaryNode + '.wien.funkfeuer.at';
    }
    // Build host cell HTML: prefer clickable anchor when we have a fullHostname
    var hostCellHtml = '';
    if (fullHostname) {
      // escape fullHostname minimally for href/text
      var safeHost = String(fullHostname).replace(/</g,'&lt;').replace(/>/g,'&gt;');
      hostCellHtml = '<a target="_blank" href="https://' + safeHost + '">' + safeHost + '</a>';
    } else if (hostnameVal) {
      hostCellHtml = String(hostnameVal).replace(/</g,'&lt;').replace(/>/g,'&gt;');
    }
    tr.appendChild(td(hostCellHtml || ''));
    tr.appendChild(td(nodeNames.join('<br>')));
    tbody.appendChild(tr);
  });
  var headers = document.querySelectorAll('#connectionsTable th');
  headers.forEach(function(h){ h.style.cursor='pointer'; h.onclick = function(){ sortTableByColumn(h.getAttribute('data-key')); }; });
  // Ensure the connections tab pane is visible after rendering (fix intermittent hidden-tab issue)
  try { var pane = document.getElementById('tab-connections'); if (pane) { pane.classList.remove('hidden'); pane.style.display = ''; } } catch(e){}
}

function renderVersionsPanel(v) {
  var wrap = document.getElementById('versions-wrap'); if(!wrap) return; wrap.innerHTML='';
  if(!v) { wrap.innerHTML = '<div class="alert alert-warning"><i class="glyphicon glyphicon-exclamation-sign"></i> No versions data available</div>'; return; }

  // Main container with improved styling
  var container = document.createElement('div');
  container.className = 'versions-container';

  // Compact Header section (single-line) with system overview and right-aligned badges
  var headerCard = document.createElement('div');
  // use panel-default for a smaller visual footprint
  headerCard.className = 'panel panel-default';
  var headerBody = document.createElement('div');
  headerBody.className = 'panel-body';
  // make it a compact single-line flex row
  headerBody.style.display = 'flex';
  headerBody.style.alignItems = 'center';
  headerBody.style.justifyContent = 'space-between';
  headerBody.style.padding = '6px 10px';

  // Left side - minimal System info
  var leftCol = document.createElement('div');
  leftCol.className = 'header-left';
  leftCol.style.display = 'flex';
  leftCol.style.flexDirection = 'column';
  leftCol.style.fontSize = '13px';
  leftCol.style.fontWeight = '600';
  var systemInfo = document.createElement('div');
  var smallInfo = (v.system ? v.system + ' • ' : '') + (v.ipv4 || 'No IP');
  systemInfo.innerHTML = '<div style="font-weight:700">' + (v.hostname || 'System') + '</div><div style="font-size:11px;color:#666;margin-top:2px;">' + smallInfo + '</div>';
  leftCol.appendChild(systemInfo);

  // Right side - Status badges
  var rightCol = document.createElement('div');
  rightCol.className = 'header-right';
  rightCol.style.display = 'flex';
  rightCol.style.alignItems = 'center';
  rightCol.style.gap = '10px';
  var badgeContainer = document.createElement('div');
  badgeContainer.className = 'status-badges';
  // Row wrapper for left and right columns
  var headerRow = document.createElement('div');
  headerRow.style.display = 'flex';
  headerRow.style.alignItems = 'center';
  headerRow.style.justifyContent = 'space-between';
  headerRow.style.width = '100%';

  // Compact status badge helper per user rules: show green OK (with check) when binary exists and running,
  // red X when binary exists but not running, grey X when binary not present.
  function createOlsrStatusBadge(binaryName, exists, running) {
    var badge = document.createElement('span');
    badge.className = 'status-olsr-badge';
    badge.style.display = 'inline-flex';
    badge.style.alignItems = 'center';
    badge.style.gap = '6px';
    badge.style.padding = '4px 8px';
    badge.style.borderRadius = '12px';
    badge.style.fontSize = '12px';
    badge.style.fontWeight = '600';
    var icon = document.createElement('span');
    icon.style.display = 'inline-block';
    icon.style.width = '14px';
    icon.style.height = '14px';
    icon.style.borderRadius = '50%';
    icon.style.textAlign = 'center';
    icon.style.lineHeight = '14px';
    icon.style.color = '#fff';
    icon.style.fontSize = '11px';
    var label = document.createElement('span');
    label.textContent = binaryName;

    if (!exists) {
      // grey X
      icon.style.background = '#6c757d';
      icon.textContent = '✖';
      badge.style.background = '#ececec';
      badge.style.color = '#333';
    } else if (exists && running) {
      // green OK
      icon.style.background = '#2ecc71';
      icon.textContent = '✓';
      badge.style.background = '#e6f9ee';
      badge.style.color = '#145a2b';
    } else {
      // exists but not running -> red X
      icon.style.background = '#e74c3c';
      icon.textContent = '✖';
      badge.style.background = '#fff0f0';
      badge.style.color = '#7a1d1d';
    }
    badge.appendChild(icon);
    badge.appendChild(label);
    return badge;
  }

  // Append OLSRd / OLSRd2 compact badges based on versions payload (versions.json renders into `v`)
  try {
    if (typeof v.olsrd_exists !== 'undefined') {
      badgeContainer.appendChild(createOlsrStatusBadge('olsrd', v.olsrd_exists, v.olsrd_running));
    }
    if (typeof v.olsr2_exists !== 'undefined') {
      badgeContainer.appendChild(createOlsrStatusBadge('olsrd2', v.olsr2_exists, v.olsr2_running));
    }
  } catch (e) { /* ignore UI errors */ }

  // append badges to the right column (no refresh button per user request)
  rightCol.appendChild(badgeContainer);
  headerRow.appendChild(leftCol);
  headerRow.appendChild(rightCol);
  headerBody.appendChild(headerRow);
  headerCard.appendChild(headerBody);
  // Move header into the global header placeholder so it's visible across tabs.
  var globalHeader = document.getElementById('global-versions-header');
  try {
    if (globalHeader) {
      // Clear any previous content and append the header card
      globalHeader.innerHTML = '';
      globalHeader.appendChild(headerCard);
    } else {
      // Fallback: keep header in the versions container if placeholder is missing
      container.appendChild(headerCard);
    }
  } catch (e) {
    // If DOM manipulation fails for any reason, fallback to container append
    try { container.appendChild(headerCard); } catch (ee) { /* ignore */ }
  }

  // Information cards in a grid
  var infoGrid = document.createElement('div');
  infoGrid.className = 'row';
  infoGrid.style.marginTop = '20px';

  // System Information Card
  var systemCard = createInfoCard('System Information', 'hdd', [
    { label: 'System Type', value: v.system || 'Unknown', icon: 'tag' },
    { label: 'Hostname', value: v.hostname || 'Unknown', icon: 'user' },
    { label: 'IPv4 Address', value: v.ipv4 || 'Not available', icon: 'globe' },
    { label: 'IPv6 Address', value: v.ipv6 || 'Not available', icon: 'globe' },
    { label: 'Link Serial', value: v.linkserial || 'Not available', icon: 'link' }
  ]);
  infoGrid.appendChild(systemCard);

  // OLSR Information Card
  var olsrCard = createInfoCard('OLSR Routing', 'transfer', [
    { label: 'OLSRd Status', value: v.olsrd_running ? '<span class="text-success">Running</span>' : '<span class="text-danger">Stopped</span>', icon: 'play-circle' },
    { label: 'OLSRv2 Status', value: v.olsr2_running ? '<span class="text-success">Running</span>' : '<span class="text-danger">Stopped</span>', icon: 'play-circle' },
    { label: 'OLSRd Version', value: v.olsrd_details ? v.olsrd_details.version : (v.olsrd || 'Unknown'), icon: 'info-sign' },
    { label: 'Watchdog', value: v.olsrd4watchdog ? '<span class="text-success">Active</span>' : '<span class="text-muted">Inactive</span>', icon: 'eye-open' }
  ]);
  infoGrid.appendChild(olsrCard);

  // Software Information Card
  var softwareItems = [
    { label: 'BMK Webstatus', value: v.bmk_webstatus || 'Not available', icon: 'cloud' },
    { label: 'AutoUpdate Wizards', value: v.autoupdate_wizards_installed || 'Not installed', icon: 'refresh' }
  ];

  if (v.olsrd_details && v.olsrd_details.description) {
    softwareItems.push({ label: 'OLSRd Description', value: v.olsrd_details.description, icon: 'file' });
  }

  if (v.bootimage && v.bootimage.md5 && v.bootimage.md5 !== 'n/a') {
    softwareItems.push({ label: 'Boot Image MD5', value: v.bootimage.md5, icon: 'lock' });
  }

  var softwareCard = createInfoCard('Software & Updates', 'cog', softwareItems);
  infoGrid.appendChild(softwareCard);

  // AutoUpdate Settings Card (if available)
  if (v.autoupdate_settings) {
    var autoupdateItems = [];
    Object.keys(v.autoupdate_settings).forEach(function(key) {
      var value = v.autoupdate_settings[key];
      var displayValue = typeof value === 'boolean' ? (value ? 'Enabled' : 'Disabled') : String(value);
      autoupdateItems.push({
        label: key.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase()),
        value: displayValue,
        icon: value ? 'ok' : 'remove'
      });
    });
    var autoupdateCard = createInfoCard('AutoUpdate Configuration', 'wrench', autoupdateItems);
    infoGrid.appendChild(autoupdateCard);
  }

  container.appendChild(infoGrid);

  // Collapsible raw data section
  var rawDataSection = document.createElement('div');
  rawDataSection.className = 'panel panel-default';
  rawDataSection.style.marginTop = '20px';

  var rawDataHeader = document.createElement('div');
  rawDataHeader.className = 'panel-heading';
  rawDataHeader.innerHTML = '<h4 class="panel-title"><a data-toggle="collapse" href="#rawVersionsData"><i class="glyphicon glyphicon-chevron-down"></i> Raw Version Data</a></h4>';
  rawDataSection.appendChild(rawDataHeader);

  var rawDataBody = document.createElement('div');
  rawDataBody.id = 'rawVersionsData';
  rawDataBody.className = 'panel-collapse collapse';
  var rawDataContent = document.createElement('div');
  rawDataContent.className = 'panel-body';
  var pre = document.createElement('pre');
  pre.style.maxHeight = '300px';
  pre.style.overflow = 'auto';
  pre.className = 'json-data';
  pre.textContent = JSON.stringify(v, null, 2);
  rawDataContent.appendChild(pre);
  rawDataBody.appendChild(rawDataContent);
  rawDataSection.appendChild(rawDataBody);

  container.appendChild(rawDataSection);

  // Add custom CSS for better styling
  if (!document.getElementById('versions-custom-css')) {
    var style = document.createElement('style');
    style.id = 'versions-custom-css';
    style.textContent = `
      .versions-container .panel { margin-bottom: 15px; }
      .versions-container .info-item { margin-bottom: 8px; padding: 8px; border-radius: 4px; background: #f8f9fa; }
      .versions-container .info-item:hover { background: #e9ecef; }
      .versions-container .info-label { font-weight: 600; color: #495057; }
      .versions-container .info-value { color: #6c757d; word-break: break-all; }
      .versions-container .status-badge { font-size: 12px; margin-right: 8px; margin-bottom: 4px; }
      .versions-container .json-data { background: #f8f9fa; border: 1px solid #dee2e6; border-radius: 4px; padding: 10px; }
      .versions-container .panel-primary .panel-heading { background: linear-gradient(135deg, #007bff, #0056b3); }
    `;
    document.head.appendChild(style);
  }

  wrap.appendChild(container);

  // Helper function to create info cards
  function createInfoCard(title, icon, items) {
    var col = document.createElement('div');
    col.className = 'col-md-6';

    var card = document.createElement('div');
    card.className = 'panel panel-info';

    var cardHeader = document.createElement('div');
    cardHeader.className = 'panel-heading';
    cardHeader.innerHTML = '<h5><i class="glyphicon glyphicon-' + icon + '"></i> ' + title + '</h5>';
    card.appendChild(cardHeader);

    var cardBody = document.createElement('div');
    cardBody.className = 'panel-body';

    items.forEach(function(item) {
      var itemDiv = document.createElement('div');
      itemDiv.className = 'info-item';
      itemDiv.innerHTML = '<div class="info-label"><i class="glyphicon glyphicon-' + item.icon + '"></i> ' + item.label + '</div>' +
                         '<div class="info-value">' + item.value + '</div>';
      cardBody.appendChild(itemDiv);
    });

    card.appendChild(cardBody);
    col.appendChild(card);
    return col;
  }
}

function sortTableByColumn(key) {
  var tbody = document.querySelector('#connectionsTable tbody');
  if(!tbody) return;
  var rows = Array.prototype.slice.call(tbody.querySelectorAll('tr'));
  rows.sort(function(a,b){
    var idx = getColumnIndexByKey(key);
    var va = a.cells[idx] ? a.cells[idx].textContent.trim() : '';
    var vb = b.cells[idx] ? b.cells[idx].textContent.trim() : '';
    // Fallback for older browsers that don't support localeCompare options
    if (typeof va.localeCompare === 'function' && va.localeCompare.length >= 2) {
      return va.localeCompare(vb, undefined, {numeric:true});
    } else {
      return va.localeCompare(vb);
    }
  });
  rows.forEach(function(r){ tbody.appendChild(r); });
}

function getColumnIndexByKey(key){
  var ths = document.querySelectorAll('#connectionsTable th');
  for(var i=0;i<ths.length;i++){ if(ths[i].getAttribute('data-key')===key) return i; }
  return 0;
}

function populateTracerouteTable(tracerouteData) {
  var tbody = document.querySelector('#tracerouteTable tbody');
  if (!tbody) return;
  tbody.innerHTML = '';
  try { if (window._uiDebug) console.debug('populateTracerouteTable called, rows=', (tracerouteData && tracerouteData.length) || 0); } catch(e){}
  if (!tracerouteData || !Array.isArray(tracerouteData)) return;
  tracerouteData.forEach(function(hop) {
    var tr = document.createElement('tr');
    function td(val) { var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    var ip = hop.ip || '';
    var hostname = hop.hostname || hop.host || '';
    // if hostname empty but ip looks like a hostname? leave blank; if ip not numeric maybe treat as hostname
    if (!hostname && ip && !/^\d{1,3}(?:\.\d{1,3}){3}$/.test(ip) && ip.indexOf(':')<0) { hostname = ip; }
    tr.appendChild(td(hop.hop || ''));
    tr.appendChild(td(ip ? ('<a href="https://' + ip + '" target="_blank">' + ip + '</a>') : ''));
    var hostnameCell = td(hostname ? ('<a href="https://' + hostname + '" target="_blank">' + hostname + '</a>') : '');
    // attempt lightweight reverse lookup via existing connections/nodedb if available globally
    if (!hostname && ip && window._nodedb_cache) {
      try {
        var hnObj = window._nodedb_cache[ip];
        if (hnObj) {
          // prefer the human name field (.n), then hostname/h, then fallback to string
          var resolvedName = (typeof hnObj === 'string') ? hnObj : (hnObj.n || hnObj.hostname || hnObj.h || null);
          if (resolvedName) hostnameCell.innerHTML = '<a href="https://' + resolvedName + '" target="_blank">' + resolvedName + '</a>';
        }
      } catch(e){}
    }
    tr.appendChild(hostnameCell);
    var pingVal = hop.ping || '';
    if (pingVal) {
      // Preserve original formatting if it already includes ms (avoid adding extra space)
      var pv = String(pingVal).trim();
      if (/ms$/i.test(pv)) {
        // normalize to lowercase ms
        pv = pv.replace(/MS$/,'ms');
        tr.appendChild(td(pv));
      } else if (pv === '*') {
        tr.appendChild(td('*'));
      } else {
        // just a number, append ms without extra space to match python reference style
        tr.appendChild(td(pv + 'ms'));
      }
    } else tr.appendChild(td(''));
    tbody.appendChild(tr);
  });
  // Ensure traceroute tab pane is visible after rendering
  try { var pane = document.getElementById('tab-traceroute'); if (pane) { pane.classList.remove('hidden'); pane.style.display = ''; } } catch(e){}
  try { window._traceroutePopulatedAt = Date.now(); } catch(e){}
  // If nodedb wasn't available at render time, attempt a short re-enrichment
  try {
    if ((!window._nodedb_cache || Object.keys(window._nodedb_cache).length === 0) && typeof window._traceroute_enrich_retry === 'undefined') {
      window._traceroute_enrich_retry = 0;
      var enrichInterval = setInterval(function(){
        try {
          if (window._nodedb_cache && Object.keys(window._nodedb_cache).length > 0) {
            // re-run lightweight enrichment on table rows
            var tbody = document.querySelector('#tracerouteTable tbody');
            if (!tbody) { clearInterval(enrichInterval); return; }
            var rows = tbody.querySelectorAll('tr');
            for (var ri = 0; ri < rows.length; ri++) {
              try {
                var cells = rows[ri].children;
                if (!cells || cells.length < 3) continue;
                var ipLink = cells[1].querySelector('a');
                var hostCell = cells[2];
                if (ipLink && ipLink.textContent && (!hostCell || !hostCell.textContent || hostCell.textContent.trim() === '')) {
                  var ip = ipLink.textContent.trim();
                  var obj = window._nodedb_cache[ip];
                  if (obj) {
                    var name = (typeof obj === 'string') ? obj : (obj.n || obj.hostname || obj.h || null);
                    if (name) {
                      hostCell.innerHTML = '<a href="https://' + name + '" target="_blank">' + name + '</a>';
                    }
                  }
                }
              } catch(e) {}
            }
            clearInterval(enrichInterval);
          }
          window._traceroute_enrich_retry++;
          if (window._traceroute_enrich_retry > 6) { clearInterval(enrichInterval); }
        } catch(e) { clearInterval(enrichInterval); }
      }, 1000);
    }
  } catch(e) {}
}
          // expose nodedb for traceroute hostname enrichment
          try { window._nodedb_cache = nodedb; } catch(e){}

function runTraceroute(){
  try { if (window._uiDebug) console.debug('runTraceroute invoked'); } catch(e){}
  var targetEl = document.getElementById('tr-host');
  if (!targetEl) { console.error('Traceroute input element #tr-host not found'); return; }
  var target = (targetEl.value || '').trim();
  if(!target) return alert('Enter target for traceroute');
  var pre = document.getElementById('p-traceroute');
  // add a small visible summary near the hostname so users get immediate feedback
  var summaryEl = document.getElementById('traceroute-summary');
  if (!summaryEl) {
    var hostEl = document.getElementById('hostname');
    if (hostEl && hostEl.parentNode) {
      summaryEl = document.createElement('div');
      summaryEl.id = 'traceroute-summary';
      summaryEl.style.margin = '6px 0';
      summaryEl.style.fontSize = '90%';
      hostEl.parentNode.insertBefore(summaryEl, hostEl.nextSibling);
    }
  }
  if (summaryEl) summaryEl.textContent = 'Traceroute: running ...';
  if (pre) pre.style.display='block';
  if (pre) pre.textContent='Running traceroute...';
  fetch('/traceroute?target='+encodeURIComponent(target),{cache:'no-store'}).then(function(r){ return r.text(); }).then(function(t){
  if (pre) pre.textContent = t;
  try { if (window._uiDebug) console.debug('traceroute raw output length=', t.length); } catch(e){}
    // Try to parse and populate table (supports numeric -n output and hostname (ip) formats)
    try {
      var lines = t.split('\n');
      var hops = [];
      var lineRegex = /^\s*(\d+)\s+(.*)$/;
      for (var i = 0; i < lines.length; i++) {
        var line = lines[i].trim();
        if (!line) continue;
        var m = line.match(lineRegex);
        if (!m) continue;
        var hopnum = m[1];
        var rest = m[2];
        var ip = '';
        var hostname = '';
        var ping = '';
        // capture ip in parentheses: hostname (ip) ...
        var paren = rest.match(/\(([^)]+)\)/);
        if (paren) {
          ip = paren[1];
          hostname = rest.replace(/\([^)]*\)/, '').trim().split(/\s+/)[0] || '';
        } else {
          var tokens = rest.split(/\s+/);
          if (tokens.length > 0) {
            var tok0 = tokens[0];
            var isIpv4 = /^\d{1,3}(?:\.\d{1,3}){3}$/.test(tok0);
            var isIpv6 = tok0.indexOf(':') >= 0;
            if (isIpv4 || isIpv6) { ip = tok0; }
            else { hostname = tok0; if (tokens.length > 1) { var maybeIp = tokens[1].replace(/^[()]+|[()]+$/g, ''); if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(maybeIp) || maybeIp.indexOf(':') >= 0) ip = maybeIp; } }
          }
        }
        var pingMatch = rest.match(/(\d+(?:\.\d+)?)\s*ms/);
        if (pingMatch) ping = pingMatch[1]; else { var numMatch = rest.match(/(\d+(?:\.\d+)?)(?!.*\d)/); if (numMatch) ping = numMatch[1]; }
        hops.push({ hop: hopnum, ip: ip, hostname: hostname, ping: ping });
      }
      if (hops.length > 0) {
        populateTracerouteTable(hops);
        if (pre) { pre.style.display='none'; }
        if (summaryEl) summaryEl.textContent = 'Traceroute: ' + hops.length + ' hop(s)';
  try { if (window._uiDebug) console.debug('Traceroute parsed', hops.length, 'hops'); } catch(e){}
      } else {
        // fallback: try simpler token parsing per-line
        try {
          var fhops = [];
          for (var i = 0; i < lines.length; i++) {
            var line = lines[i].trim(); if (!line) continue;
            var tokens = line.split(/\s+/);
            if (tokens.length === 0) continue;
            if (!/^\d+$/.test(tokens[0])) continue;
            var hnum = tokens[0];
            var hip = '';
            var hname = '';
            var hping = '';
            if (tokens.length === 2) { hip = tokens[1]; }
            else if (tokens.length >= 3) {
              // tokens like: hop host (ip) t ms  OR hop ip t ms OR hop host ip t ms
              // find token that looks like IP
              for (var j = 1; j < tokens.length; j++) {
                var tok = tokens[j].replace(/^[()]+|[()]+$/g, '');
                if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(tok) || tok.indexOf(':')>=0) { hip = tok; }
              }
              // hostname is first token after hop that is not the ip
              for (var j = 1; j < tokens.length; j++) {
                var tok = tokens[j].replace(/^[()]+|[()]+$/g, '');
                if (tok && tok !== hip) { hname = tok; break; }
              }
              // ping: look for ms
              var pMatch = line.match(/(\d+(?:\.\d+)?)\s*ms/);
              if (pMatch) hping = pMatch[1];
            }
            fhops.push({ hop: hnum, ip: hip, hostname: hname, ping: hping });
          }
          if (fhops.length > 0) { populateTracerouteTable(fhops); if (pre) pre.style.display='none'; if (summaryEl) summaryEl.textContent = 'Traceroute: ' + fhops.length + ' hop(s)'; }
        } catch(e) { /* ignore */ }
        if (summaryEl && (!fhops || fhops.length === 0)) {
          var one = t.split('\n')[0] || t;
          summaryEl.textContent = 'Traceroute: no hops parsed. First line: ' + (one.length>200?one.substr(0,200)+'...':one);
        }
      }
    } catch (e) { /* ignore parsing errors */ }
  }).catch(function(e){ if (pre) pre.textContent = 'ERR: '+e; });
}

// Legacy OLSRd presence detection
  function detectLegacyOlsrd(cb){
    // Be tolerant: /status may sometimes include non-JSON wrapper text on some systems.
    // Fetch as text and try to extract the first JSON object instead of relying on r.json().
    // Use the tolerant safeFetchStatus to parse /status (handles concatenated
    // JSON and wrapper text). This avoids hard JSON.parse failures on some
    // embedded systems that emit multiple objects.
    safeFetchStatus({cache:'no-store'}).then(function(st){
      // st may be null if parsing failed inside safeFetchStatus
      // Determine whether to show legacy OLSR tab. If status doesn't explicitly
      // indicate legacy olsrd, probe the /olsr/links endpoint before hiding the tab
      // so we don't incorrectly hide it when live data exists elsewhere.
      var show = false;
      try {
        if (st) {
          if (!st.olsr2_on && (st.olsrd_on || (st.links && Array.isArray(st.links) && st.links.length))) show = true;
        }
      } catch(e) { show = false; }

      var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
      if (!show && linkTab) {
        // status didn't indicate OLSRd; probe /olsr/links to be sure
        try {
          fetch('/olsr/links', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(res){
            try {
              if (res && Array.isArray(res.links) && res.links.length) {
                show = true;
              }
            } catch(e) {}
            if (window._uiDebug) console.debug('detectLegacyOlsrd: setting OLSR tab visibility to', show, 'after probing /olsr/links');
            linkTab.parentElement.style.display = show ? '' : 'none';
            if (cb) cb(show);
          }).catch(function(){
            // probe failed — do not aggressively hide; keep current behavior conservative
            if (window._uiDebug) console.debug('detectLegacyOlsrd: /olsr/links probe failed; leaving OLSR tab hidden');
            linkTab.parentElement.style.display = 'none';
            if (cb) cb(false);
          });
          return; // early return since probe will call cb
        } catch(e) { /* fallthrough to default hide */ }
      }
      if (linkTab) {
        if (window._uiDebug) console.debug('detectLegacyOlsrd: setting OLSR tab visibility to', show);
        linkTab.parentElement.style.display = show? '' : 'none';
      }
      if (cb) cb(show);
  }).catch(function(err){ var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]'); if (linkTab) { if (window._uiDebug) console.debug('detectLegacyOlsrd: fetch failed, leaving OLSR tab visibility unchanged:', err); } if(cb) cb(false); });
  }
  detectLegacyOlsrd();

// --- Per-table search & generic sorting wiring (applies to main tables, avoids modal tables) ---
;(function(){
  function normalizeText(s){ return (s||'').toString().toLowerCase(); }

  function filterTableByQuery(table, q){
    if (!table) return;
    q = normalizeText(q || '');
    var tbody = table.tBodies[0]; if(!tbody) return;
    var rows = Array.prototype.slice.call(tbody.rows || []);
    rows.forEach(function(r){ var txt = normalizeText(r.textContent || ''); r.style.display = (q === '' || txt.indexOf(q) !== -1) ? '' : 'none'; });
  }

  function sortTable(table, colIndex){
    if (!table) return;
    var tbody = table.tBodies[0]; if(!tbody) return;
    var rows = Array.prototype.slice.call(tbody.querySelectorAll('tr'));
    var asc = true;
    var prevIdx = table.getAttribute('data-sort-col');
    if (prevIdx !== null && prevIdx !== undefined && String(prevIdx) === String(colIndex)) {
      asc = table.getAttribute('data-sort-asc') !== 'true';
    }
    // store new state
    table.setAttribute('data-sort-col', String(colIndex));
    table.setAttribute('data-sort-asc', asc ? 'true' : 'false');

    rows.sort(function(a,b){
      var aCell = a.cells[colIndex] ? (a.cells[colIndex].textContent || '') : '';
      var bCell = b.cells[colIndex] ? (b.cells[colIndex].textContent || '') : '';
      var an = parseFloat(aCell.replace(/[^0-9\.\-]/g,''));
      var bn = parseFloat(bCell.replace(/[^0-9\.\-]/g,''));
      if (!isNaN(an) && !isNaN(bn)) {
        return asc ? (an - bn) : (bn - an);
      }
      var av = normalizeText(aCell);
      var bv = normalizeText(bCell);
      if (av < bv) return asc ? -1 : 1;
      if (av > bv) return asc ? 1 : -1;
      return 0;
    });
    rows.forEach(function(r){ tbody.appendChild(r); });
    // update header indicators
    try {
      var ths = table.tHead.querySelectorAll('th');
      ths.forEach(function(th, i){
        var mark = th.querySelector('.sort-mark'); if (!mark) { mark = document.createElement('span'); mark.className='sort-mark'; mark.style.marginLeft='6px'; mark.textContent='↕'; th.appendChild(mark); }
        if (i === colIndex) { mark.textContent = table.getAttribute('data-sort-asc') === 'true' ? '▲' : '▼'; }
        else { mark.textContent = '↕'; }
      });
    } catch(e) {}
  }

  // wire search inputs
  try {
    var inputs = document.querySelectorAll('.table-search-input');
    inputs.forEach(function(inp){
      var tgt = inp.getAttribute('data-target');
      if (!tgt) return;
      var table = document.querySelector(tgt);
      inp.addEventListener('input', function(){ filterTableByQuery(table, inp.value); });
    });
  } catch(e) {}

  // wire generic header sorting for non-modal tables
  try {
    var allTables = document.querySelectorAll('table');
    allTables.forEach(function(table){
      if (!table.id) return;
      // skip modal/internal tables that already have dedicated sorting
      if (table.id === 'route-modal-table' || table.id === 'node-modal-table') return;
      var thead = table.tHead; if(!thead) return;
      var ths = thead.querySelectorAll('th');
      ths.forEach(function(th, idx){
        // attach pointer cursor
        th.style.cursor = 'pointer';
  // ensure a small sort mark element exists (default hint '↕')
  if (!th.querySelector('.sort-mark')) { var sm = document.createElement('span'); sm.className='sort-mark'; sm.style.marginLeft='6px'; sm.textContent='↕'; th.appendChild(sm); }
        th.addEventListener('click', function(){ sortTable(table, idx); });
      });
    });
  } catch(e) {}
})();

// Wire fetch debug modal close on DOM load so it's always available
document.addEventListener('DOMContentLoaded', function(){
  var closeBtn = document.getElementById('fetch-debug-close');
  if (closeBtn) closeBtn.addEventListener('click', function(){ hideModal('fetch-debug-modal'); });
  var rClose = document.getElementById('route-modal-close'); if (rClose) rClose.addEventListener('click', function(){ hideModal('route-modal'); });
  var nClose = document.getElementById('node-modal-close'); if (nClose) nClose.addEventListener('click', function(){ hideModal('node-modal'); });
  // overlay clicks: close any modal when clicking the overlay outside the panel
  var overlays = document.querySelectorAll('.modal-overlay');
  overlays.forEach(function(o){ o.addEventListener('click', function(e){ if (e.target === o) { try { o.setAttribute('aria-hidden','true'); o.style.display='none'; } catch(e){} } }); });
});

// Runtime debug overlay (non-invasive): shows computed display and table row counts for key tabs
(function(){
  function createDebugOverlay(){
    if (document.getElementById('ui-debug-overlay')) return;
    var o = document.createElement('div');
    o.id = 'ui-debug-overlay';
    o.style.position = 'fixed'; o.style.right = '12px'; o.style.top = '12px'; o.style.zIndex = '9999';
    o.style.background = 'rgba(0,0,0,0.7)'; o.style.color = '#fff'; o.style.padding = '8px 10px';
    o.style.fontSize = '12px'; o.style.lineHeight = '1.2'; o.style.borderRadius = '6px'; o.style.maxWidth = '360px';
    o.style.fontFamily = 'monospace'; o.style.whiteSpace = 'pre-wrap'; o.style.pointerEvents = 'none';
    document.body.appendChild(o);
    function update(){
      try {
        var ids = ['#tab-status','#tab-olsr','#tab-stats','#tab-connections','#tab-versions','#tab-traceroute','#tab-log'];
        var out = [];
        ids.forEach(function(id){
          var el = document.querySelector(id);
          if (!el) { out.push(id + ': MISSING'); return; }
          var cs = window.getComputedStyle(el);
          var tables = el.querySelectorAll('table');
          var rows = [];
          tables.forEach(function(t){ try { var r = t.querySelectorAll('tbody tr').length; rows.push((t.id||t.className||t.tagName)+':'+r); } catch(e){} });
          out.push(id + ' display=' + cs.display + ' visible=' + (cs.display!=='none') + ' childNodes=' + (el.childNodes?el.childNodes.length:0) + ' rows={' + rows.join(',') + '}');
        });
        out.push('window._olsr_links=' + (window._olsr_links? window._olsr_links.length : 0) + '  _logLoaded=' + (window._logLoaded?1:0) + '  _statusLoaded=' + (window._statusLoaded?1:0));
        o.textContent = out.join('\n');
      } catch(e) { /* ignore */ }
    }
    update();
    o._interval = setInterval(update, 1500);
  }
  if (window._uiDebug) {
    if (document.readyState === 'complete' || document.readyState === 'interactive') createDebugOverlay();
    else document.addEventListener('DOMContentLoaded', createDebugOverlay);
  }
})();