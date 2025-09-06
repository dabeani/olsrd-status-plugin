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
  }
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
    if (l.is_default) { tr.style.backgroundColor = '#fff8d5'; }
    function td(val){ var td = document.createElement('td'); td.innerHTML = val || ''; return td; }
    tr.appendChild(td(l.intf));
    tr.appendChild(td(l.local));
    tr.appendChild(td(l.remote));
    if (l.remote_host) {
      var linkHtml = '<a target="_blank" href="https://' + l.remote_host + '">' + l.remote_host + '</a>';
      tr.appendChild(td(linkHtml));
    } else { tr.appendChild(td(l.remote_host)); }
    tr.appendChild(td(l.lq));
    tr.appendChild(td(l.nlq));
    tr.appendChild(td(l.cost));
    var routesCell = td(l.routes || '');
    if (l.routes && parseInt(l.routes,10) > 0) {
      routesCell.style.cursor='pointer'; routesCell.title='Click to view routes via this neighbor';
      routesCell.addEventListener('click', function(){ showRoutesFor(l.remote); });
    }
    tr.appendChild(routesCell);
    var nodesDisplay = l.nodes || '';
    if (l.node_names) {
      var names = l.node_names.split(',').slice(0,10); // cap display
      nodesDisplay += '<div style="font-size:60%;color:#555;max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;" title="' + l.node_names + '">' + names.join(', ') + (l.node_names.split(',').length>10?' …':'') + '</div>';
    }
    var nodesCell = td(nodesDisplay);
    // make nodes clickable like routes when there are nodes
    if (l.nodes && parseInt(l.nodes,10) > 0) {
      nodesCell.style.cursor = 'pointer'; nodesCell.title = 'Click to view nodes behind this neighbor';
      nodesCell.addEventListener('click', function(){ showNodesFor(l.remote, l.node_names); });
    }
    tr.appendChild(nodesCell);
    tbody.appendChild(tr);
  });
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
    modal.style.display='block';
    // wire header sort clicks
    try {
      var headers = document.querySelectorAll('#node-modal-table thead th[data-key]');
      headers.forEach(function(h){ h.onclick = function(){ var k = h.getAttribute('data-key'); if (_nodeSort.key === k) _nodeSort.asc = !_nodeSort.asc; else { _nodeSort.key = k; _nodeSort.asc = true; } renderRows(window._nodedb_cache_list || found); }; });
    } catch(e) {}
    return;
  }

  fetch('/nodedb.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(nb){ try{ window._nodedb_cache = nb || {}; var found = findNodes(nb || {}); window._nodedb_cache_list = found; if (countBadge) { countBadge.style.display='inline-block'; countBadge.textContent = found.length; } renderRows(found); if (bodyPre) bodyPre.textContent = JSON.stringify(found, null, 2); modal.style.display='block'; }catch(e){ if(tbody) tbody.innerHTML='<tr><td colspan="6" class="text-danger">Error rendering nodes</td></tr>'; if(bodyPre) bodyPre.textContent='Error'; modal.style.display='block'; } }).catch(function(){ if(tbody) tbody.innerHTML='<tr><td colspan="6" class="text-danger">Error loading nodedb.json</td></tr>'; if(bodyPre) bodyPre.textContent='Error loading nodedb.json'; modal.style.display='block'; });
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
          // could be either "destination device" or "destination metric"
          var maybe = parts[1];
          if (!isNaN(Number(maybe))) { metric = maybe; }
          else { device = maybe; }
        } else if (parts.length >= 3) {
          // assume last token is metric if numeric, rest between dest and last are device
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
  modal.style.display='block';
}

window.addEventListener('load', function(){
  var c = document.getElementById('route-modal-close');
  if (c) c.addEventListener('click', function(){ var m=document.getElementById('route-modal'); if (m) m.style.display='none'; });
  var m = document.getElementById('route-modal');
  if (m) m.addEventListener('click', function(e){ if (e.target === m) m.style.display='none'; });

  var nc = document.getElementById('node-modal-close');
  if (nc) nc.addEventListener('click', function(){ var m=document.getElementById('node-modal'); if (m) m.style.display='none'; window._nodeModal_state = null; if (_nodeModalKeyHandler) { document.removeEventListener('keydown', _nodeModalKeyHandler); _nodeModalKeyHandler = null; } });
  var nm = document.getElementById('node-modal');
  if (nm) nm.addEventListener('click', function(e){ if (e.target === nm) { nm.style.display='none'; window._nodeModal_state = null; if (_nodeModalKeyHandler) { document.removeEventListener('keydown', _nodeModalKeyHandler); _nodeModalKeyHandler = null; } } });
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
}

function updateUI(data) {
  try {
  setText('hostname', data.hostname || 'Unknown');
  setText('ip', data.ip || '');
  // prefer human-friendly uptime string if provided by backend
  setText('uptime', data.uptime_linux || data.uptime_str || data.uptime || '');
  setText('dl-uptime', data.uptime_linux || data.uptime_str || data.uptime || '');
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
    var li = document.getElementById('tab-olsrd2-links'); if (li) li.style.display='';
    setText('olsr2info', data.olsr2info || '');
  } else {
    showTab('tab-olsr2', false);
    var li = document.getElementById('tab-olsrd2-links'); if (li) li.style.display='none';
  }
  // legacy OLSR tab visibility (if backend set olsrd_on or provided links while not olsr2)
  try {
    if (!data.olsr2_on && (data.olsrd_on || (data.links && data.links.length))) {
      var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
      if (linkTab) linkTab.parentElement.style.display = '';
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
      // Fetch summary first for fast paint
      fetch('/status/summary',{cache:'no-store'}).then(function(r){return r.json();}).then(function(s){
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
                // Keep tab visible; it will lazy-load on click
                var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
                if (linkTab) linkTab.parentElement.style.display = '';
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
  function ensureTraceroutePreloaded(){
    var tbody = document.querySelector('#tracerouteTable tbody');
    if (tbody && tbody.children.length) return; // already populated
    fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(st){
      if (st && st.trace_to_uplink && Array.isArray(st.trace_to_uplink) && st.trace_to_uplink.length){
        var trTab = document.querySelector('#mainTabs a[href="#tab-traceroute"]');
        if (trTab) trTab.parentElement.style.display='';
        var hops = st.trace_to_uplink.map(function(h){ return { hop: h.hop || '', ip: h.ip || h.host || '', hostname: h.host || h.hostname || h.ip || '', ping: h.ping || '' }; });
        populateTracerouteTable(hops);
        var summaryEl = document.getElementById('traceroute-summary');
        if (summaryEl) summaryEl.textContent = 'Traceroute to ' + (st.trace_target || '') + ': ' + hops.length + ' hop(s)';
      }
    }).catch(function(){});
  }
  mt.addEventListener('click', function(e){
    var a = e.target.closest('a'); if(!a) return;
    if (a.getAttribute('href') === '#tab-olsr' && !_olsrLoaded) {
      fetch('/olsr/links',{cache:'no-store'}).then(function(r){return r.json();}).then(function(o){
        if (o.links && o.links.length) { populateOlsrLinksTable(o.links); }
        _olsrLoaded = true;
      }).catch(function(){});
    } else if (a.getAttribute('href') === '#tab-traceroute') {
      ensureTraceroutePreloaded();
    }
  });
  // Refresh button support for OLSR Links
  var refreshLinksBtn = document.getElementById('refresh-links');
  if (refreshLinksBtn) {
    refreshLinksBtn.addEventListener('click', function(){
      var statusEl = document.getElementById('links-status'); var spinner = document.getElementById('refresh-links-spinner');
      try { refreshLinksBtn.disabled = true; } catch(e){}
      if (statusEl) statusEl.textContent = 'Refreshing…';
      if (spinner) spinner.classList.add('rotate');
      // First force-update node_db
      fetch('/nodedb/refresh',{cache:'no-store'}).then(function(r){ return r.json(); }).then(function(res){
        // ignore res contents; continue to fetch latest links
        return fetch('/olsr/links',{cache:'no-store'});
      }).then(function(r2){ return r2.json(); }).then(function(o){
        if (o.links && o.links.length) { populateOlsrLinksTable(o.links); }
        if (statusEl) statusEl.textContent = '';
        _olsrLoaded = true;
      }).catch(function(e){ if (statusEl) statusEl.textContent = 'ERR'; }).finally(function(){ try{ refreshLinksBtn.disabled=false; }catch(e){} if (spinner) spinner.classList.remove('rotate'); });
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
    // Build full hostname: resolved hostname + ".<node>.funkfeuer.at" when node present
    var fullHostname = (hostnameVal || '').toString();
    var primaryNode = (nodeNames && nodeNames.length) ? nodeNames[0] : '';
    // sanitize trailing dot
    if (fullHostname && fullHostname.slice(-1) === '.') fullHostname = fullHostname.slice(0, -1);
    if (primaryNode) {
      if (fullHostname) fullHostname = fullHostname + '.' + primaryNode + '.funkfeuer.at';
      else fullHostname = primaryNode + '.funkfeuer.at';
    }
    // Append Hostname first (constructed fullHostname or fallback), then Node column (node names)
    tr.appendChild(td(fullHostname || hostnameVal || ''));
    tr.appendChild(td(nodeNames.join('<br>')));
    tbody.appendChild(tr);
  });
  var headers = document.querySelectorAll('#connectionsTable th');
  headers.forEach(function(h){ h.style.cursor='pointer'; h.onclick = function(){ sortTableByColumn(h.getAttribute('data-key')); }; });
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
  tbl1.appendChild(rowKV('glyphicon-tasks','Kernel', v.kernel || '-'));
  tbl1.appendChild(rowKV('glyphicon-time','Uptime', v.uptime || v.uptime_str || '-'));
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
}
          // expose nodedb for traceroute hostname enrichment
          try { window._nodedb_cache = nodedb; } catch(e){}

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
    // Reuse existing status endpoint: if olsr2_on true we consider olsrd2; need explicit legacy check
    fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(st){
      // If olsr2_on is true assume olsrd2; hide legacy links tab unless links array exists AND st.olsr2_on is false
      var show = false;
      if (!st.olsr2_on && st.links && Array.isArray(st.links) && st.links.length) show = true;
      var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]');
      if (linkTab) linkTab.parentElement.style.display = show? '' : 'none';
      if (cb) cb(show);
    }).catch(function(){ var linkTab = document.querySelector('#mainTabs a[href="#tab-olsr"]'); if (linkTab) linkTab.parentElement.style.display='none'; if(cb) cb(false); });
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