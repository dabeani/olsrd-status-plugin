window.refreshTab = function(id, url) {
  var el = document.getElementById(id);
  if (el) el.textContent = 'Loading…';
  if (id === 'p-json') {
    fetch(url, {cache:"no-store"}).then(r=>r.text()).then(t=>{
      try{ el.textContent = JSON.stringify(JSON.parse(t), null, 2); }
      catch(e){ el.textContent = t; }
    }).catch(e=>{ el.textContent = "ERR: "+e; });
    return;
  }
  fetch(url, {cache:"no-store"}).then(r=>{
    if(!r.ok) return r.text().then(t=>{ el.textContent="HTTP "+r.status+"\n"+t; });
    return r.text().then(t=>{ el.textContent = t; });
  }).catch(e=>{ el.textContent = "ERR: "+e; });
};

function setText(id, text) {
  var el = document.getElementById(id);
  if (el) el.textContent = text;
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
  if (!devices || !Array.isArray(devices)) return;
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
  if (!links || !Array.isArray(links)) return;
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

function updateUI(data) {
  setText('hostname', data.hostname || 'Unknown');
  setText('ip', data.ip || '');
  setText('uptime', data.uptime || '');
  try { if (data.hostname) document.title = data.hostname; } catch(e) {}
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
}

function detectPlatformAndLoad() {
  fetch('/capabilities', {cache: 'no-store'})
    .then(r => r.json())
    .then(caps => {
      var data = { hostname: '', ip: '', uptime: '', devices: [], airos: {}, olsr2_on: false, olsr2info: '', admin: null };
      fetch('/status', {cache: 'no-store'})
        .then(r => r.json())
        .then(status => {
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
              .then(r => r.text())
              .then(t => { data.olsr2info = t; updateUI(data); try { if (data.links && data.links.length) populateOlsrLinksTable(data.links); } catch(e){} });
          } else {
            updateUI(data);
            try { if (data.links && data.links.length) populateOlsrLinksTable(data.links); } catch(e){}
          }
          if (status.admin_url) {
            data.admin = { url: status.admin_url };
            updateUI(data);
          }
          var nodedb = {};
          fetch('/nodedb.json',{cache:'no-store'}).then(r=>r.json()).then(nb=>{ nodedb = nb || {}; }).catch(()=>{ nodedb = {}; });
          function loadConnections() {
            var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = 'Loading...';
            fetch('/connections.json',{cache:'no-store'}).then(r=>r.json()).then(c=>{
              renderConnectionsTable(c, nodedb);
              if(statusEl) statusEl.textContent = '';
            }).catch(e=>{ var el=document.getElementById('connections-status'); if(el) el.textContent='ERR: '+e; });
          }
          loadConnections();
          function loadVersions() {
            var statusEl = document.getElementById('versions-status'); if(statusEl) statusEl.textContent = 'Loading...';
            fetch('/versions.json',{cache:'no-store'}).then(r=>r.json()).then(v=>{
              renderVersionsPanel(v);
              if(statusEl) statusEl.textContent = '';
            }).catch(e=>{ var el=document.getElementById('versions-status'); if(el) el.textContent='ERR: '+e; });
          }
          loadVersions();
          document.getElementById('tr-run').addEventListener('click', function(){ runTraceroute(); });
          document.getElementById('refresh-connections').addEventListener('click', loadConnections);
          document.getElementById('refresh-versions').addEventListener('click', loadVersions);
        });
    });
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
  function add(k,label){ var dt=document.createElement('dt'); dt.textContent=label||k; var dd=document.createElement('dd'); dd.textContent=(v[k]!==undefined?v[k]:'-'); dl.appendChild(dt); dl.appendChild(dd); }
  add('hostname','Hostname');
  add('firmware','Firmware');
  add('kernel','Kernel');
  add('model','Model');
  add('autoupdate','AutoUpdate');
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
    return va.localeCompare(vb, undefined, {numeric:true});
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
  fetch('/traceroute?target='+encodeURIComponent(target),{cache:'no-store'}).then(r=>r.text()).then(t=>{
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
  }).catch(e=>{ if (pre) pre.textContent = 'ERR: '+e; });
}

