// JSON polyfill for older browsers
if (!window.JSON) {
  window.JSON = {
    parse: function(s) {
      return eval('(' + s + ')');
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
        if (obj.hasOwnProperty(k)) {
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
    var keys = [];
    for (var key in obj) {
      if (obj.hasOwnProperty(key)) {
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
        self.onFulfilled.forEach(function(fn) { fn(value); });
      }
    }
    
    function reject(reason) {
      if (self.status === 'pending') {
        self.status = 'rejected';
        self.reason = reason;
        self.onRejected.forEach(function(fn) { fn(reason); });
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
        } catch (e) {
          reject(e);
        }
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
        } catch (e) {
          reject(e);
        }
      }
      
      if (self.status === 'fulfilled') {
        handleFulfilled(self.value);
      } else if (self.status === 'rejected') {
        handleRejected(self.reason);
      } else {
        self.onFulfilled.push(handleFulfilled);
        self.onRejected.push(handleRejected);
      }
    });
  };
  
  window.Promise.prototype.catch = function(onRejected) {
    return this.then(null, onRejected);
  };
  
  window.Promise.resolve = function(value) {
    return new window.Promise(function(resolve) { resolve(value); });
  };
  
  window.Promise.all = function(promises) {
    return new window.Promise(function(resolve, reject) {
      var results = [];
      var completed = 0;
      var total = promises.length;
      if (total === 0) {
        resolve(results);
        return;
      }
      promises.forEach(function(promise, index) {
        promise.then(function(value) {
          results[index] = value;
          completed++;
          if (completed === total) {
            resolve(results);
          }
        }, reject);
      });
    });
  };

// Fetch polyfill for older browsers
if (!window.fetch) {
  window.fetch = function(url, options) {
    return new Promise(function(resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhr.open(options && options.method || 'GET', url, true);
      if (options && options.headers) {
        for (var header in options.headers) {
          xhr.setRequestHeader(header, options.headers[header]);
        }
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
      xhr.send(options && options.body);
    });
  };
}

window.refreshTab = function(id, url) {
  var el = document.getElementById(id);
  if (el) el.textContent = 'Loading…';
  if (id === 'p-json') {
    fetch(url, {cache:"no-store"}).then(function(r){ return r.text(); }).then(function(t){
      try{ el.textContent = JSON.stringify(JSON.parse(t), null, 2); }
      catch(e){ el.textContent = t; }
    }).catch(function(e){ el.textContent = "ERR: "+e; });
    return;
  }
  fetch(url, {cache:"no-store"}).then(function(r){
    if(!r.ok) return r.text().then(function(t){ el.textContent="HTTP "+r.status+"\n"+t; });
    return r.text().then(function(t){ el.textContent = t; });
  }).catch(function(e){ el.textContent = "ERR: "+e; });
};

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
  }
}

function setHTML(id, html) {
  var el = document.getElementById(id);
  if (el) el.innerHTML = html;
}

function showTab(tabId, show) {
  var el = document.getElementById(tabId);
  if (el) el.style.display = show ? '' : 'none';
}

function populateDevicesTable(devices, airos) {
  var tbody = document.querySelector('#devicesTable tbody');
  tbody.innerHTML = '';
  if (!devices || !Array.isArray(devices) || devices.length === 0) {
    var tbody = document.querySelector('#devicesTable tbody');
    if (tbody) tbody.innerHTML = '<tr><td colspan="8" class="text-muted">No devices found</td></tr>';
    return;
  }
  var warn_frequency = 0;
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
}

function populateOlsrLinksTable(links) {
  var tbody = document.querySelector('#olsrLinksTable tbody');
  if (!tbody) return; tbody.innerHTML = '';
  if (!links || !Array.isArray(links) || links.length === 0) {
    var tbody = document.querySelector('#olsrLinksTable tbody');
    if (tbody) tbody.innerHTML = '<tr><td colspan="9" class="text-muted">No links found</td></tr>';
    return;
  }
  links.forEach(function(l){
    var tr = document.createElement('tr');
    function td(val){ var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(l.intf));
    tr.appendChild(td(l.local));
    tr.appendChild(td(l.remote));
    tr.appendChild(td(l.remote_host));
    tr.appendChild(td(l.lq));
    tr.appendChild(td(l.nlq));
    tr.appendChild(td(l.cost));
    tr.appendChild(td(l.routes || ''));
    tr.appendChild(td(l.nodes || ''));
    tbody.appendChild(tr);
  });
}

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
}

function updateUI(data) {
  try {
  setText('hostname', data.hostname || 'Unknown');
  setText('ip', data.ip || '');
  // prefer human-friendly uptime string if provided by backend
  setText('uptime', data.uptime_str || data.uptime || '');
  setText('dl-uptime', data.uptime_str || data.uptime || '');
  try { if (data.hostname) document.title = data.hostname; } catch(e) {}
  try { setText('main-host', data.hostname || ''); } catch(e) {}
  try { populateNavHost(data.hostname || '', data.ip || ''); } catch(e) {}
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
  if (data.olsr2_on) {
    showTab('tab-olsr2', true);
    setText('olsr2info', data.olsr2info || '');
  } else {
    showTab('tab-olsr2', false);
  }
  if (data.admin && data.admin.url) {
    showTab('tab-admin', true);
    var adminLink = document.getElementById('adminLink');
    if (adminLink) { adminLink.href = data.admin.url; adminLink.textContent = 'Login'; }
  } else {
    showTab('tab-admin', false);
  }
  } catch(e) {}
}

function detectPlatformAndLoad() {
  try {
  fetch('/capabilities', {cache: 'no-store'})
    .then(function(r) { return r.json(); })
    .then(function(caps) {
      // show/hide traceroute tab
      try {
        var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
        if (trTab) trTab.parentElement.style.display = (caps.traceroute? '': 'none');
        var adminTabLink = document.getElementById('tab-admin-link');
        if (adminTabLink) adminTabLink.style.display = (caps.show_admin_link? '' : 'none');
      } catch(e){}
      var data = { hostname: '', ip: '', uptime: '', devices: [], airos: {}, olsr2_on: false, olsr2info: '', admin: null };
      fetch('/status', {cache: 'no-store'})
        .then(function(r) { return r.json(); })
        .then(function(status) {
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
                if (linkTab) linkTab.parentElement.style.display = '';
                populateOlsrLinksTable(data.links);
              } else {
                var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
                if (linkTab) linkTab.parentElement.style.display = 'none';
              }
              // Neighbors
              if (status.neighbors && Array.isArray(status.neighbors) && status.neighbors.length) {
                var nTab = document.querySelector('#mainTabs a[href="#tab-neighbors"]'); if (nTab) nTab.parentElement.style.display = '';
                populateNeighborsTable(status.neighbors);
              } else {
                var nTab = document.querySelector('#mainTabs a[href="#tab-neighbors"]'); if (nTab) nTab.parentElement.style.display = 'none';
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
          fetch('/nodedb.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ nodedb = nb || {}; }).catch(function(){ nodedb = {}; });
          function loadConnections() {
            var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = 'Loading...';
            fetch('/connections.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(c){
              renderConnectionsTable(c, nodedb);
              if(statusEl) statusEl.textContent = '';
            }).catch(function(e){ var el=document.getElementById('connections-status'); if(el) el.textContent='ERR: '+e; });
          }
          loadConnections();
          function loadVersions() {
            var statusEl = document.getElementById('versions-status'); if(statusEl) statusEl.textContent = 'Loading...';
            fetch('/versions.json',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(v){
              renderVersionsPanel(v);
              if(statusEl) statusEl.textContent = '';
            }).catch(function(e){ var el=document.getElementById('versions-status'); if(el) el.textContent='ERR: '+e; });
          }
          loadVersions();
          document.getElementById('tr-run').addEventListener('click', function(){ runTraceroute(); });
          document.getElementById('refresh-connections').addEventListener('click', loadConnections);
          document.getElementById('refresh-versions').addEventListener('click', loadVersions);
          // render fixed traceroute-to-uplink results when provided by /status
          try {
            if (status.trace_to_uplink && Array.isArray(status.trace_to_uplink) && status.trace_to_uplink.length) {
              // show traceroute tab and populate table
              var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
              if (trTab) trTab.parentElement.style.display = '';
              var hops = status.trace_to_uplink.map(function(h){ return { hop: h.hop || h.hop || '', ip: h.ip || h.ip || '', hostname: h.host || h.hostname || h.host || '', ping: h.ping || h.ping || '' }; });
              populateTracerouteTable(hops);
              // also show a short textual summary
              var summaryEl = document.getElementById('traceroute-summary');
              if (summaryEl) summaryEl.textContent = 'Traceroute to ' + (status.trace_target || '') + ': ' + hops.length + ' hop(s)';
            }
          } catch(e) { /* ignore */ }
        });
    });
  } catch(e) {
    // Fallback: try to load data without capabilities
    try {
      fetch('/status', {cache: 'no-store'})
        .then(function(r) { return r.json(); })
        .then(function(status) {
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
        });
    } catch(e2) {}
  }
}

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

// classList polyfill for older browsers
if (!HTMLElement.prototype.classList) {
  HTMLElement.prototype.classList = {
    add: function(className) {
      if (!new RegExp('(^|\\s)' + className + '(\\s|$)').test(this.className)) {
        this.className += (this.className ? ' ' : '') + className;
      }
    },
    remove: function(className) {
      this.className = this.className.replace(new RegExp('(^|\\s)' + className + '(\\s|$)', 'g'), ' ');
    },
    contains: function(className) {
      return new RegExp('(^|\\s)' + className + '(\\s|$)').test(this.className);
    }
  };
}

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
        });
      }
    } else {
      document.attachEvent('on' + event, callback);
    }
  };
}

document.addEventListener('DOMContentLoaded', function() {
  // Initialize tab functionality
  var tabLinks = document.querySelectorAll('#mainTabs a');
  var tabPanes = document.querySelectorAll('.tab-pane');

  function switchTab(targetId) {
    // Hide all tab panes
    tabPanes.forEach(function(pane) {
      pane.classList.remove('active');
    });

    // Remove active class from all tab links
    tabLinks.forEach(function(link) {
      link.parentElement.classList.remove('active');
    });

    // Show target tab pane
    var targetPane = document.querySelector(targetId);
    if (targetPane) {
      targetPane.classList.add('active');
    }

    // Add active class to clicked tab link
    var activeLink = document.querySelector('#mainTabs a[href="' + targetId + '"]');
    if (activeLink) {
      activeLink.parentElement.classList.add('active');
    }
  }

  // Add click handlers to tab links
  tabLinks.forEach(function(link) {
    link.addEventListener('click', function(e) {
      e.preventDefault();
      var targetId = this.getAttribute('href');
      switchTab(targetId);
    });
  });

  detectPlatformAndLoad();
});

// Populate dynamic nav entries (host + login) after status load
function populateNavHost(host, ip) {
  var el = document.getElementById('nav-host');
  if (!el) return;
  el.innerHTML = ip + ' - ' + host;
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
  c.ports.forEach(function(p){
    var tr = document.createElement('tr');
    function td(val){ var td=document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(p.port));
    tr.appendChild(td(p.bridge || ''));
    tr.appendChild(td((p.macs || []).join('<br>')));
    tr.appendChild(td((p.ips || []).join('<br>')));
    var hostnames = [];
    (p.ips || []).forEach(function(ip){ if(nodedb[ip] && nodedb[ip].name) hostnames.push(nodedb[ip].name); });
    tr.appendChild(td(hostnames.join('<br>')));
    tr.appendChild(td(p.notes || ''));
    tbody.appendChild(tr);
  });
  var headers = document.querySelectorAll('#connectionsTable th');
  headers.forEach(function(h){ h.style.cursor='pointer'; h.onclick = function(){ sortTableByColumn(h.getAttribute('data-key')); }; });
}

function renderVersionsPanel(v) {
  var wrap = document.getElementById('versions-wrap'); if(!wrap) return; wrap.innerHTML='';
  if(!v) { wrap.textContent = 'No versions data'; return; }
  var dl = document.createElement('dl'); dl.className='dl-horizontal';
  function add(k,label){ var dt=document.createElement('dt'); dt.textContent=label||k; var dd=document.createElement('dd');
    var val = v[k];
    if (val === undefined) dd.textContent = '-';
    else if (typeof val === 'object') dd.textContent = JSON.stringify(val);
    else dd.textContent = String(val);
    dl.appendChild(dt); dl.appendChild(dd); }

  // preferred ordering to match bmk-webstatus.py output when present
  var preferred = ['hostname','firmware','kernel','model','autoupdate','wizards','local_ips'];
  var used = {};
  preferred.forEach(function(k){ if (v[k] !== undefined) { add(k, k==='local_ips' ? 'Local IPs' : (k==='wizards' ? 'Wizards' : (k==='autoupdate' ? 'AutoUpdate' : k.charAt(0).toUpperCase()+k.slice(1)))); used[k]=1; } });
  // add remaining keys
  Object.keys(v).sort().forEach(function(k){ if (!used[k]) add(k); });
  var pre = document.createElement('pre'); pre.style.maxHeight='240px'; pre.style.overflow='auto'; pre.textContent = JSON.stringify(v,null,2);
  wrap.appendChild(dl); wrap.appendChild(pre);
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
  if (!tracerouteData || !Array.isArray(tracerouteData)) return;

  tracerouteData.forEach(function(hop) {
    var tr = document.createElement('tr');
    function td(val) { var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(hop.hop || ''));
    tr.appendChild(td('<a href="https://' + (hop.ip || '') + '" target="_blank">' + (hop.ip || '') + '</a>'));
    tr.appendChild(td('<a href="https://' + (hop.hostname || '') + '" target="_blank">' + (hop.hostname || '') + '</a>'));
    tr.appendChild(td((hop.ping || '') + 'ms'));
    tbody.appendChild(tr);
  });
}

function runTraceroute(){
  var target = document.getElementById('tr-host').value.trim();
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
          // hostname is the part before the parentheses
          hostname = rest.replace(/\([^)]*\)/, '').trim().split(/\s+/)[0] || '';
        } else {
          // no parentheses, split tokens
          var tokens = rest.split(/\s+/);
          if (tokens.length > 0) {
            var tok0 = tokens[0];
            // crude IP detection
            var isIpv4 = /^\d{1,3}(?:\.\d{1,3}){3}$/.test(tok0);
            var isIpv6 = tok0.indexOf(':') >= 0;
            if (isIpv4 || isIpv6) {
              ip = tok0;
            } else {
              hostname = tok0;
              if (tokens.length > 1) {
                var maybeIp = tokens[1].replace(/^[()]+|[()]+$/g, '');
                if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(maybeIp) || maybeIp.indexOf(':') >= 0) ip = maybeIp;
              }
            }
          }
        }
        // ping: look for number followed by ms
        var pingMatch = rest.match(/(\d+(?:\.\d+)?)\s*ms/);
        if (pingMatch) ping = pingMatch[1];
        else {
          // fallback: pick last numeric token
          var numMatch = rest.match(/(\d+(?:\.\d+)?)(?!.*\d)/);
          if (numMatch) ping = numMatch[1];
        }
        hops.push({ hop: hopnum, ip: ip, hostname: hostname, ping: ping });
      }
      if (hops.length > 0) {
        populateTracerouteTable(hops);
        if (pre) { pre.style.display='none'; }
        if (summaryEl) summaryEl.textContent = 'Traceroute: ' + hops.length + ' hop(s)';
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

