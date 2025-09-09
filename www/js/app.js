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
      // Replace 'nodename' token when setting hostname or main-host so UI shows real nodename
      if ((id === 'hostname' || id === 'main-host') && text && typeof text === 'string' && /\bnodename\b/.test(text)) {
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

// Robustly fetch /status and parse the last top-level JSON object from the
// response text. Returns a Promise that resolves to the parsed object or
// rejects on network/error conditions.
function safeFetchStatus(fetchOptions) {
  fetchOptions = fetchOptions || {cache:'no-store'};
  return fetch('/status', fetchOptions).then(function(r){ return r.text(); }).then(function(txt){
    // Fast-path: attempt JSON.parse of the whole body
    try { return JSON.parse(txt); } catch(e) {}
    // Otherwise extract last balanced JSON object and parse
    function extractLastJsonObject(str) {
      if (!str) return null;
      var lastCandidate = null; var depth = 0; var start = -1;
      for (var i = 0; i < str.length; i++) {
        var ch = str.charAt(i);
        if (ch === '{') { if (depth === 0) start = i; depth++; }
        else if (ch === '}') { depth--; if (depth === 0 && start !== -1) { lastCandidate = str.substring(start, i+1); start = -1; } }
      }
      return lastCandidate;
    }
    try {
      var candidate = extractLastJsonObject(txt);
      if (candidate) return JSON.parse(candidate);
    } catch(e2) {}
    // If all attempts fail, throw to let callers handle error
    throw new Error('Failed to parse /status response');
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
  if (!devices || !Array.isArray(devices) || devices.length === 0) {
    var tbody = document.querySelector('#devicesTable tbody');
    if (tbody) tbody.innerHTML = '<tr><td colspan="8" class="text-muted">No devices found</td></tr>';
    return;
  }
  var warn_frequency = 0;
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
    tr.appendChild(td(wireless));
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
    function mkLqBadge(val){ var v = parseFloat(String(val||'')||'0'); var cls = 'lq-low'; if (!isFinite(v) || v <= 0) cls='lq-high'; else if (v < 0.5) cls='lq-low'; else if (v < 0.85) cls='lq-med'; else cls='lq-high'; return '<span class="lq-badge '+cls+'">'+String(val)+'</span>'; }
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
  var metricsHtml = '';
  metricsHtml += '<span class="metric-badge routes small">R:'+ (l.routes || '0') +'</span>';
  var metricsTd = td(metricsHtml);
  metricsTd.title = 'Routes: ' + (l.routes||'0') + (l.is_default? ' • Default route':'');
  tr.appendChild(metricsTd);
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
  try { setText('main-host', (resolved || data.hostname) || ''); } catch(e) {}
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
  populateDevicesTable(data.devices, data.airos);
  // cache devices for client-side sorting and re-render
  try { window._devices_data = Array.isArray(data.devices) ? data.devices : []; } catch(e) { window._devices_data = []; }
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
  var canvas = document.getElementById(canvasId);
  if (!canvas || !canvas.getContext) return;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  var w = canvas.width, h = canvas.height;
  var data = series.slice(-10); // last 10 points
  if (!data.length) return;
  var minY = Math.min.apply(null, data), maxY = Math.max.apply(null, data);
  if (minY === maxY) { minY = 0; maxY = maxY + 1; }
  var pad = 18;
  // Draw axes
  ctx.strokeStyle = '#bbb';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(pad, pad); ctx.lineTo(pad, h-pad); ctx.lineTo(w-pad, h-pad);
  ctx.stroke();
  // Y axis label
  ctx.fillStyle = '#666';
  ctx.font = '11px sans-serif';
  ctx.fillText(yLabel, 2, pad-4);
  // X axis label
  ctx.fillText('Samples', w-pad-40, h-pad+14);
  // Draw line
  ctx.strokeStyle = color || '#0074d9';
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (var i=0; i<data.length; i++) {
    var x = pad + ((w-2*pad) * i/(data.length-1));
    var y = h-pad - ((h-2*pad) * (data[i]-minY)/(maxY-minY));
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
  // Draw points
  ctx.fillStyle = color || '#0074d9';
  for (var i=0; i<data.length; i++) {
    var x = pad + ((w-2*pad) * i/(data.length-1));
    var y = h-pad - ((h-2*pad) * (data[i]-minY)/(maxY-minY));
    ctx.beginPath(); ctx.arc(x, y, 3, 0, 2*Math.PI); ctx.fill();
  }
  // Y axis ticks
  ctx.fillStyle = '#666';
  ctx.font = '10px sans-serif';
  ctx.fillText(minY, 2, h-pad);
  ctx.fillText(maxY, 2, pad+4);
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
      } else if (typeof data.olsr_routes_count === 'number') {
        routes = data.olsr_routes_count;
      }
      pushStat('olsr_routes', routes);
      pushStat('olsr_nodes', nodes);
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
        // set live-dot to updating briefly
        var routesDot = document.getElementById('routes-live'); if (routesDot) { routesDot.classList.remove('live-dot-stale'); routesDot.classList.add('live-dot-updating'); clearTimeout(routesDot._staleTO); routesDot._staleTO = setTimeout(function(){ try{ routesDot.classList.remove('live-dot-updating'); routesDot.classList.add('live-dot-stale'); }catch(e){} }, 2000); }
        var nodesDot = document.getElementById('nodes-live'); if (nodesDot) { nodesDot.classList.remove('live-dot-stale'); nodesDot.classList.add('live-dot-updating'); clearTimeout(nodesDot._staleTO); nodesDot._staleTO = setTimeout(function(){ try{ nodesDot.classList.remove('live-dot-updating'); nodesDot.classList.add('live-dot-stale'); }catch(e){} }, 2000); }
        // update small timestamp next to live dots (human and title tooltip)
        try {
          var now = new Date();
          var iso = now.toLocaleTimeString();
          var routesTs = document.getElementById('routes-ts'); if (routesTs) { routesTs.textContent = iso; routesTs.title = now.toString(); }
          var nodesTs = document.getElementById('nodes-ts'); if (nodesTs) { nodesTs.textContent = iso; nodesTs.title = now.toString(); }
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

    // build panel using bootstrap styles
    var html = '';
    html += '<div class="panel panel-default">';
    html += '<div class="panel-heading" style="display:flex; justify-content:space-between; align-items:center;">';
    html += '<div style="font-weight:600">Fetch Queue</div>';
  html += '<div style="display:flex; gap:8px; align-items:center;">';
  html += '<button id="fetch-stats-refresh" class="btn btn-xs btn-default"><span class="spin" id="fetch-stats-refresh-spin"></span>Refresh</button>';
  html += '<button id="fetch-stats-debug" class="btn btn-xs btn-default">Debug</button>';
  // interval selector: persisted in localStorage as 'fetch_auto_interval_ms'
  html += '<select id="fetch-stats-interval" class="input-sm form-control" style="width:120px; display:inline-block; margin-right:6px;">';
  html += '<option value="0">Auto off</option>';
  html += '<option value="5000">5s</option>';
  html += '<option value="10000">10s</option>';
  html += '<option value="15000">15s</option>';
  html += '<option value="30000">30s</option>';
  html += '<option value="60000">60s</option>';
  html += '<option value="120000">2m</option>';
  html += '<option value="300000">5m</option>';
  html += '</select>';
  html += '<button id="fetch-stats-autorefresh" class="btn btn-xs btn-default" title="Toggle auto-refresh">Auto</button>';
  html += '</div></div>';
    html += '<div class="panel-body" style="padding:10px">';
    html += '<div class="row">';
    // left: badges + progress
    html += '<div class="col-xs-8">';
    html += '<div style="margin-bottom:8px">';
    function mkBadge(label, val, cls) { return '<span class="fetch-badge '+(cls||'')+'" style="margin-right:6px">'+label+': <strong style="margin-left:6px;">'+String(val)+'</strong></span>'; }
    var qcls = queued >= q_crit ? 'crit' : (queued >= q_warn ? 'warn' : '');
    var dcls = dropped >= d_warn ? 'warn' : '';
    html += mkBadge('Queued', queued, qcls);
    html += mkBadge('Processing', processing);
    html += mkBadge('Dropped', dropped, dcls);
    html += mkBadge('Retries', retries);
    html += mkBadge('Processed', processed);
    html += '</div>';
  // progress bar: percent of queue_crit as a practical max
  var denom = q_crit > 0 ? q_crit : 200;
  var pct = Math.min(100, Math.round((queued / denom) * 100));
  var progClass = pct >= 100 ? 'progress-bar-danger' : (pct >= Math.round((q_warn/denom)*100) ? 'progress-bar-warning' : 'progress-bar-success');
  // single progress bar (controls live in the header to avoid duplicate IDs)
  html += '<div class="progress" style="height:16px; margin-bottom:6px">';
  html += '<div class="progress-bar '+progClass+'" role="progressbar" aria-valuenow="'+pct+'" aria-valuemin="0" aria-valuemax="100" style="width:'+pct+'%;">'; // close left column only
  html += '</div></div>';
  html += '<div style="margin-top:4px">'+pct+'%</div>';
    html += '</div>'; // close left column
    // right: small table with values (inside same row)
    html += '<div class="col-xs-4">';
    html += '<table class="table table-condensed" style="margin:0">';
    html += '<tbody>';
    html += '<tr><th style="width:55%">Queued</th><td>'+queued+'</td></tr>';
    html += '<tr><th>Processing</th><td>'+processing+'</td></tr>';
    html += '<tr><th>Dropped</th><td>'+dropped+'</td></tr>';
    html += '<tr><th>Retries</th><td>'+retries+'</td></tr>';
    html += '<tr><th>Processed</th><td>'+processed+'</td></tr>';
    html += '<tr><th>Successes</th><td>'+successes+'</td></tr>';
    html += '</tbody></table>';
  html += '</div>'; // col
  html += '</div>'; // row
  html += '<div class="small-muted">Thresholds: warn='+q_warn+' &nbsp; crit='+q_crit+' &nbsp; dropped_warn='+d_warn+'</div>';
  html += '</div>'; // panel-body
  html += '</div>'; // panel

    container.innerHTML = html;

    // wire actions: refresh
    var ref = document.getElementById('fetch-stats-refresh');
    var refSpinner = document.getElementById('fetch-stats-refresh-spin');
    if (ref) ref.addEventListener('click', function(){
      try { ref.disabled = true; if (refSpinner) refSpinner.classList.add('rotate'); } catch(e){}
      fetch('/status/lite', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){ try { if (s.fetch_stats) populateFetchStats(s.fetch_stats); } catch(e){} }).catch(function(){
    safeFetchStatus({cache:'no-store'}).then(function(s){ try{ if (s.fetch_stats) populateFetchStats(s.fetch_stats); }catch(e){} });
      }).finally(function(){ try{ if (refSpinner) refSpinner.classList.remove('rotate'); ref.disabled=false; }catch(e){} });
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
    try {
  // (status-summary card removed; fetch stats now live in the Fetch Queue tab)
      var hostEl = document.getElementById('nav-host');
      if (hostEl) {
        var hostnameSpan = document.getElementById('hostname');
        var hostText = hostnameSpan ? hostnameSpan.textContent : hostEl.textContent;
        // build icon HTML
        var iconHtml = '';
        if (qcls === 'crit') {
          iconHtml = '<span class="text-danger" style="margin-right:6px;"><span class="glyphicon glyphicon-exclamation-sign" aria-hidden="true"></span></span>';
          hostEl.style.background = '#ffecec';
          hostEl.classList.add('fetch-crit-blink');
        } else if (qcls === 'warn') {
          iconHtml = '<span class="text-warning" style="margin-right:6px;"><span class="glyphicon glyphicon-exclamation-sign" aria-hidden="true"></span></span>';
          hostEl.style.background = '#fff6e5';
          hostEl.classList.remove('fetch-crit-blink');
        } else {
          // clear any previous highlight
          hostEl.style.background = '';
          hostEl.classList.remove('fetch-crit-blink');
        }
        // update hostname span and optional icon wrapper (do not clobber children)
        var prefix = '- ';
        var hostnameSpanEl = hostEl.querySelector('#hostname');
        if (!hostnameSpanEl) { hostnameSpanEl = document.createElement('span'); hostnameSpanEl.id = 'hostname'; hostEl.appendChild(hostnameSpanEl); }
        hostnameSpanEl.textContent = hostText || '';
        // manage small icon wrapper
        if (iconHtml) {
          var iconWrap = hostEl.querySelector('.fetch-icon');
          if (!iconWrap) { iconWrap = document.createElement('span'); iconWrap.className = 'fetch-icon'; hostEl.insertBefore(iconWrap, hostnameSpanEl); }
          iconWrap.innerHTML = iconHtml;
        } else {
          var iconWrap = hostEl.querySelector('.fetch-icon'); if (iconWrap) iconWrap.parentNode.removeChild(iconWrap);
        }
        hostEl.title = 'Fetch queue: queued=' + queued + ', dropped=' + dropped + ', retries=' + retries;
        // when crit, show toast with short message
        try {
          var toast = document.getElementById('fetch-toast');
          var toastMsg = document.getElementById('fetch-toast-msg');
          if (qcls === 'crit') {
            if (toast && toastMsg) {
              toastMsg.textContent = 'Queued=' + queued + ', Dropped=' + dropped + ', Retries=' + retries;
              toast.style.display = 'block';
            }
          } else {
            if (toast) toast.style.display = 'none';
          }
        } catch(e){}
  // Note: header-wide click removed to preserve other controls; use debug button instead
      }
    } catch(e) { /* ignore header update errors */ }

  } catch(e) { /* ignore UI errors */ }
}

function detectPlatformAndLoad() {
  try {
  // helper safe JSON parser
  function safeParse(label, text) {
    try { return JSON.parse(text); }
    catch(e) {
      try { console.error(label + ' JSON parse failed', e, text.slice(0,400)); } catch(_e) {}
      return null;
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
  // Fetch summary first for fast paint
    fetch('/status/lite',{cache:'no-store'}).then(function(r){return r.json();}).then(function(s){
        if (s.hostname) data.hostname = s.hostname;
        if (s.ip) data.ip = s.ip;
        if (s.uptime_linux) { data.uptime_linux = s.uptime_linux; }
        updateUI(data);
      }).catch(function(){});
  // Fetch remaining heavier status in background (full status still used for deep data)
  fetch('/status/lite', {cache: 'no-store'})
        .then(function(r) { return r.text(); })
        .then(function(statusText) {
          var status = safeParse('status', statusText);
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
              var spinner = document.getElementById('refresh-connections-spinner');
              try { refreshConnBtn.disabled = true; } catch(e){}
              if (spinner) spinner.classList.add('rotate');
              // call loadConnections which returns a Promise
              try {
                var p = loadConnections();
                if (p && typeof p.finally === 'function') {
                  p.finally(function(){ try{ refreshConnBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); });
                } else {
                  // Not a promise (nodedb not ready), just re-enable
                  try{ refreshConnBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate');
                }
              } catch(e){ try{ refreshConnBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); }
            });
          }
          var refreshVerBtn = document.getElementById('refresh-versions');
          if (refreshVerBtn) {
            refreshVerBtn.addEventListener('click', function(){
              var spinner = document.getElementById('refresh-versions-spinner');
              try { refreshVerBtn.disabled = true; } catch(e){}
              if (spinner) spinner.classList.add('rotate');
              try {
                var p = loadVersions();
                if (p && typeof p.finally === 'function') {
                  p.finally(function(){ try{ refreshVerBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); });
                } else {
                  try{ refreshVerBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate');
                }
              } catch(e){ try{ refreshVerBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); }
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
      fetch('/status', {cache: 'no-store'})
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
    if (tbody) {
      // Always clear table before populating
      while (tbody.firstChild) tbody.removeChild(tbody.firstChild);
    }
    fetch('/status', {cache:'no-store'})
      .then(function(r){ return r.text(); })
      .then(function(txt){
        // Log raw response to help diagnose non-JSON or wrapped responses
        try { console.log('ensureTraceroutePreloaded: /status raw length=', txt && txt.length); } catch(e){}
        var st = null;
        try {
          st = JSON.parse(txt);
        } catch(e) {
          try { console.error('ensureTraceroutePreloaded: failed to parse /status JSON', e); } catch(_){}
          // Attempt to salvage first JSON object from possibly wrapped text
          try {
            // If /status returns wrapped text or multiple JSON blobs concatenated,
            // prefer the last top-level JSON object (most recent /status payload).
            function extractLastJsonObject(str) {
              if (!str) return null;
              var lastCandidate = null;
              var depth = 0;
              var start = -1;
              for (var i = 0; i < str.length; i++) {
                var ch = str.charAt(i);
                if (ch === '{') {
                  if (depth === 0) start = i;
                  depth++;
                } else if (ch === '}') {
                  depth--;
                  if (depth === 0 && start !== -1) {
                    lastCandidate = str.substring(start, i + 1);
                    start = -1;
                  }
                }
              }
              return lastCandidate;
            }
            var candidate = extractLastJsonObject(txt);
            if (candidate) st = JSON.parse(candidate);
            try { console.log('ensureTraceroutePreloaded: salvaged JSON length=', candidate?candidate.length:0); } catch(e){}
          } catch(e2) { st = null; }
        }
        return st;
      })
      .then(function(st){
        var summaryEl = document.getElementById('traceroute-summary');
        try { console.log('ensureTraceroutePreloaded: /status returned, trace_to_uplink present=', !!(st && st.trace_to_uplink && st.trace_to_uplink.length)); } catch(e){}
        if (st && st.trace_to_uplink && Array.isArray(st.trace_to_uplink) && st.trace_to_uplink.length) {
          console.log('ensureTraceroutePreloaded: using precomputed hops count=', st.trace_to_uplink.length);
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
          try { console.log('ensureTraceroutePreloaded: no precomputed hops; trace_target=', st && st.trace_target); } catch(e){}
          // Show empty/error state
          if (tbody) {
            // Only show fallback 'no data' when we haven't already successfully populated
            // the traceroute table earlier in this page session.
            if (!window._traceroutePopulatedAt) {
              var tr = document.createElement('tr');
              var td = document.createElement('td');
              td.colSpan = 4;
              td.textContent = 'No traceroute data available.';
              tr.appendChild(td);
              tbody.appendChild(tr);
            }
          }
          if (summaryEl) {
            summaryEl.textContent = 'Traceroute data not available.';
            summaryEl.setAttribute('aria-label', 'Traceroute summary: no data');
          }
          if (window._uiDebug) console.debug('No traceroute data found in /status response');

          // If backend provided a traceroute target but did not include a precomputed
          // trace_to_uplink, populate the traceroute input and trigger a live
          // traceroute once on initial page load. Guard with a global flag to
          // avoid repeated runs during subsequent events.
          try {
            if (!window._tracerouteAutoRunDone && st && st.trace_target) {
              try { console.log('ensureTraceroutePreloaded: attempting auto-run traceroute for', st.trace_target); } catch(e){}
              var trInput = document.getElementById('tr-host');
              if (trInput) {
                trInput.value = st.trace_target;
                // mark as done before starting to avoid re-entrancy
                window._tracerouteAutoRunDone = true;
                // give the DOM a tick before running to ensure UI elements are ready
                setTimeout(function(){ try { runTraceroute(); } catch(e){} }, 10);
              } else {
                try { console.error('ensureTraceroutePreloaded: tr-host input not found when attempting auto-run'); } catch(e){}
              }
            }
          } catch(e) {}
        }
      })
      .catch(function(e){
        if (window._uiDebug) console.error('Error loading traceroute data:', e);
        var tbody = document.querySelector('#tracerouteTable tbody');
          if (tbody) {
            // Only show error placeholder if we don't already have traceroute data.
            if (!window._traceroutePopulatedAt) {
              var tr = document.createElement('tr');
              var td = document.createElement('td');
              td.colSpan = 4;
              td.textContent = 'Error loading traceroute data.';
              tr.appendChild(td);
              tbody.appendChild(tr);
            }
          }
        var summaryEl = document.getElementById('traceroute-summary');
        if (summaryEl) {
          summaryEl.textContent = 'Traceroute error.';
          summaryEl.setAttribute('aria-label', 'Traceroute summary: error');
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
      var statusEl = document.getElementById('links-status'); var spinner = document.getElementById('refresh-links-spinner');
      try { refreshLinksBtn.disabled = true; } catch(e){}
      if (statusEl) statusEl.textContent = 'Refreshing…';
      if (spinner) spinner.classList.add('rotate');
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
      }).catch(function(e){ if (statusEl) statusEl.textContent = 'ERR'; showActionToast('Error refreshing links', 4000); }).finally(function(){ try{ refreshLinksBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); });
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
            console.log('Fetching /status API...');
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
          // ensure any precomputed traceroute in /status is rendered
          safeFetchStatus({cache:'no-store'}).then(function(st){ try {
              if (st && st.trace_to_uplink && Array.isArray(st.trace_to_uplink) && st.trace_to_uplink.length) {
                var hops = st.trace_to_uplink.map(function(h){ return { hop: h.hop || '', ip: h.ip || h.host || '', hostname: h.host || h.hostname || h.ip || '', ping: h.ping || '' }; });
                populateTracerouteTable(hops);
                return;
              }
              // If server provided only a trace_target (no precomputed hops), trigger a live traceroute
              // when the user opens the Traceroute tab. Guard with flags so we don't run repeatedly.
              if (st && st.trace_target && !(st.trace_to_uplink && st.trace_to_uplink.length)) {
                try {
                  var trInput = document.getElementById('tr-host');
                  if (trInput) {
                    trInput.value = st.trace_target;
                    // Only auto-run once per session unless the table is empty and not populated earlier
                    if (!window._traceroutePopulatedAt && !window._tracerouteAutoRunDone) {
                      window._tracerouteAutoRunDone = true;
                      setTimeout(function(){ try { runTraceroute(); } catch(e){} }, 10);
                    }
                  }
                } catch(e) {}
              }
            } catch(e){}
          }).catch(function(){
            // Show fallback content when API fails
            var tbody = document.querySelector('#tracerouteTable tbody');
            if (tbody) {
              tbody.innerHTML = '<tr><td colspan="4" class="text-muted">Traceroute data not available. This tab requires backend API access.</td></tr>';
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
  var spinner = document.getElementById('refresh-log-spinner-2') || document.getElementById('refresh-log-spinner');
  var status = document.getElementById('log-status');
  try {
    if (btn) btn.disabled = true;
    if (classBtns && classBtns.length) classBtns.forEach(function(b){ b.disabled = true; });
    if (spinner) spinner.classList.add('rotate');
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
        if (spinner) spinner.classList.remove('rotate');
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
    var mainHostEl = document.getElementById('main-host');
    if (mainHostEl) {
      try { mainHostEl.textContent = displayHost || ''; } catch(e) { mainHostEl.innerText = displayHost || ''; }
    }
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
  if(!v) { wrap.textContent = 'No versions data'; return; }

  // Top-level header with hostname and small badges
  var header = document.createElement('div'); header.className = 'row';
  var hleft = document.createElement('div'); hleft.className = 'col-sm-8';
  var htitle = document.createElement('h4'); htitle.innerHTML = '<span class="glyphicon glyphicon-list-alt" aria-hidden="true"></span> Versions & System Info';
  hleft.appendChild(htitle);
  var hright = document.createElement('div'); hright.className = 'col-sm-4 text-right';
  var hostSpan = document.createElement('div'); hostSpan.className = 'small-muted'; hostSpan.style.marginTop='8px'; hostSpan.textContent = (v.hostname?('Host: '+v.hostname):'');
  hright.appendChild(hostSpan);
  header.appendChild(hleft); header.appendChild(hright);
  wrap.appendChild(header);

  // Create two columns: System / Software
  var row = document.createElement('div'); row.className = 'row';

  var col1 = document.createElement('div'); col1.className = 'col-sm-6';
  var panel1 = document.createElement('div'); panel1.className = 'panel panel-default';
  var panel1b = document.createElement('div'); panel1b.className = 'panel-body';
  panel1b.innerHTML = '<h5><span class="glyphicon glyphicon-hdd" aria-hidden="true"></span> System</h5>';
  var tbl1 = document.createElement('table'); tbl1.className = 'table table-condensed';
  function rowKV(icon, label, value) {
    var tr = document.createElement('tr');
    var th = document.createElement('th'); th.style.width='35%'; th.style.fontWeight='600'; th.innerHTML = (icon?('<span class="glyphicon '+icon+'" aria-hidden="true"></span> '):'') + label;
    var td = document.createElement('td'); td.innerHTML = value===undefined || value===null ? '<span class="text-muted">-</span>' : String(value);
    tr.appendChild(th); tr.appendChild(td); return tr;
  }
  tbl1.appendChild(rowKV('glyphicon-tag','System', v.system || '-'));
  tbl1.appendChild(rowKV('glyphicon-info-sign','Model', v.model || v.product || '-'));
  // Kernel: prefer common fields, fall back to nested/system hints
  var kernelVal = v.kernel || v.kernel_version || v.os_kernel || (v.system && v.system.kernel) || (v.system_info && v.system_info.kernel) || '-';
  tbl1.appendChild(rowKV('glyphicon-tasks','Kernel', kernelVal));
  if (v.local_ips) tbl1.appendChild(rowKV('glyphicon-globe','Local IPs', Array.isArray(v.local_ips)?v.local_ips.join(', '):String(v.local_ips)));
  panel1b.appendChild(tbl1); panel1.appendChild(panel1b); col1.appendChild(panel1);

  var col2 = document.createElement('div'); col2.className = 'col-sm-6';
  var panel2 = document.createElement('div'); panel2.className = 'panel panel-default';
  var panel2b = document.createElement('div'); panel2b.className = 'panel-body';
  panel2b.innerHTML = '<h5><span class="glyphicon glyphicon-cloud" aria-hidden="true"></span> Software</h5>';
  var tbl2 = document.createElement('table'); tbl2.className = 'table table-condensed';
  // prefer common fields
  function addSoftRow(key, label) {
    if (v[key] !== undefined) tbl2.appendChild(rowKV('glyphicon-cog', label||key, (typeof v[key] === 'object')?JSON.stringify(v[key]):v[key]));
  }
  addSoftRow('olsrd', 'OLSRd');
  addSoftRow('olsr2', 'OLSRv2');
  addSoftRow('firmware','Firmware');
  addSoftRow('bmk_webstatus','BMK Webstatus');
  addSoftRow('autoupdate','AutoUpdate');
  // detailed olsrd fields
  if (v.olsrd_details) {
    var d = v.olsrd_details;
    tbl2.appendChild(rowKV('glyphicon-screenshot','OLSRd Version', d.version || (v.olsrd||'-')));
    tbl2.appendChild(rowKV('glyphicon-list-alt','OLSRd Description', d.description || '-'));
    tbl2.appendChild(rowKV('glyphicon-phone','OLSRd Device', d.device || '-'));
    tbl2.appendChild(rowKV('glyphicon-calendar','OLSRd Build Date', d.date || '-'));
    tbl2.appendChild(rowKV('glyphicon-flag','OLSRd Release', d.release || '-'));
    tbl2.appendChild(rowKV('glyphicon-file','OLSRd Source', d.source || '-'));
  }
  else {
    // Fallbacks when detailed olsrd info isn't present: try common top-level keys
    if (v.olsrd !== undefined) tbl2.appendChild(rowKV('glyphicon-screenshot','OLSRd Version', v.olsrd));
    if (v.olsrd_release !== undefined) tbl2.appendChild(rowKV('glyphicon-flag','OLSRd Release', v.olsrd_release));
    if (v.olsrd_build_date !== undefined) tbl2.appendChild(rowKV('glyphicon-calendar','OLSRd Build Date', v.olsrd_build_date));
    if (v.olsrd_source !== undefined) tbl2.appendChild(rowKV('glyphicon-file','OLSRd Source', v.olsrd_source));
  }
  // Show where version info originated (edge/router/container/local) when available
  var srcHint = v.source || (v.is_edgerouter? 'EdgeRouter' : (v.is_linux_container? 'Linux/container' : (v.host_platform || 'local')));
  if (srcHint) tbl1.appendChild(rowKV('glyphicon-map-marker','Info source', srcHint));
  // wizards may be an object/array
  if (v.wizards) tbl2.appendChild(rowKV('glyphicon-education','Wizards', (typeof v.wizards==='object')?JSON.stringify(v.wizards):v.wizards));
  if (v.bootimage && v.bootimage.md5) tbl2.appendChild(rowKV('glyphicon-lock','Boot image MD5', v.bootimage.md5));
  panel2b.appendChild(tbl2); panel2.appendChild(panel2b); col2.appendChild(panel2);

  row.appendChild(col1); row.appendChild(col2); wrap.appendChild(row);

  // Small summary badges row
  var badgeRow = document.createElement('div'); badgeRow.className = 'row'; badgeRow.style.marginTop='6px';
  var brc = document.createElement('div'); brc.className = 'col-sm-12';
  var badges = document.createElement('div');
  function addBadge(label, val, cls) { var s = document.createElement('span'); s.className = 'label '+(cls||'label-default'); s.style.marginRight='6px'; s.textContent = label+': '+String(val); badges.appendChild(s); }
  if (v.system) addBadge('System', v.system, 'label-primary');
  if (v.olsrd_running !== undefined) addBadge('OLSRd', v.olsrd_running?'running':'stopped', v.olsrd_running?'label-success':'label-danger');
  if (v.olsr2_running !== undefined) addBadge('OLSRv2', v.olsr2_running?'running':'stopped', v.olsr2_running?'label-success':'label-danger');
  if (v.autoupdate_wizards_installed) addBadge('AutoUpdate wizards', v.autoupdate_wizards_installed, 'label-info');
  brc.appendChild(badges); badgeRow.appendChild(brc); wrap.appendChild(badgeRow);

  // Full JSON dump (collapsible)
  var pre = document.createElement('pre'); pre.style.maxHeight='260px'; pre.style.overflow='auto'; pre.style.marginTop='10px'; pre.textContent = JSON.stringify(v,null,2);
  wrap.appendChild(pre);
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
        var hn = window._nodedb_cache[ip];
        if (hn) hostnameCell.innerHTML = '<a href="https://' + hn + '" target="_blank">' + hn + '</a>';
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
    fetch('/status',{cache:'no-store'}).then(function(r){ return r.text(); }).then(function(statusText){
        var st = null;
        try {
          // Fast-path: try direct parse
          st = JSON.parse(statusText);
        } catch(e) {
          // Salvage attempt: extract the first balanced JSON object (handles
          // additional non-JSON text or concatenated JSON blobs).
          try {
            // Prefer the last top-level JSON object in case the response contains
            // wrapper text or multiple concatenated JSON blobs.
            function extractLastJsonObject(str) {
              if (!str) return null;
              var lastCandidate = null;
              var depth = 0;
              var start = -1;
              for (var i = 0; i < str.length; i++) {
                var ch = str.charAt(i);
                if (ch === '{') {
                  if (depth === 0) start = i;
                  depth++;
                } else if (ch === '}') {
                  depth--;
                  if (depth === 0 && start !== -1) {
                    lastCandidate = str.substring(start, i + 1);
                    start = -1;
                  }
                }
              }
              return lastCandidate;
            }
            var candidate = extractLastJsonObject(statusText);
            if (candidate) st = JSON.parse(candidate);
          } catch (e2) {
            if (window._uiDebug) console.debug('detectLegacyOlsrd: status parse failed', e2, statusText && statusText.slice ? statusText.slice(0,200) : statusText);
            st = null;
          }
        }
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