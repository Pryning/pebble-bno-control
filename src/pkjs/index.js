// B&O Control - phone side.
// Talks to Bang & Olufsen devices on the local network:
//   Mozart platform (Beosound A5, Level, Balance, ...) : http://IP/api/v1/...
//   ASE / BeoZone platform (Beoplay M3, M5, Stage ...) : http://IP:8080/BeoZone/...

var MAX_DEVICES = 16;
var CMD_LIST = 1, CMD_SCAN = 2, CMD_CLEAR = 3, CMD_TOGGLE = 4, CMD_JOIN = 5, CMD_FOCUS = 6;
var CMD_VOLUME = 7, CMD_PREV = 8, CMD_NEXT = 9, CMD_POWER_OFF = 10, CMD_UNLINK = 11;
var ST_IDLE = 0, ST_PLAYING = 1, ST_PAUSED = 2;
var TYPE_ASE = 0, TYPE_MOZART = 1;
var POLL_MS = 5000, FOCUS_POLL_MS = 2000;

var devices = [];   // {ip, type, name, online, state, source, title, artist, volume, volMax}
var focused = -1;
var scanning = false;
var pollTimer = null;

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
function loadDevices() {
  try {
    var raw = JSON.parse(localStorage.getItem('devices') || '[]');
    devices = raw.map(function (d) {
      return { ip: d.ip, type: d.type, name: d.name, jid: d.jid || '', online: 0, state: ST_IDLE,
               source: '', title: '', artist: '' };
    });
  } catch (e) { devices = []; }
}

function saveDevices() {
  localStorage.setItem('devices', JSON.stringify(devices.map(function (d) {
    return { ip: d.ip, type: d.type, name: d.name, jid: d.jid || '' };
  })));
}

function getSubnet() {
  var s = localStorage.getItem('subnet');
  if (s) return s;
  for (var i = 0; i < devices.length; i++) {
    var m = /^(\d+\.\d+\.\d+)\.\d+$/.exec(devices[i].ip);
    if (m) return m[1];
  }
  return '192.168.1';
}

// ---------------------------------------------------------------------------
// AppMessage queue (one in flight at a time)
// ---------------------------------------------------------------------------
var queue = [];
var sending = false;

function enqueue(msg, key) {
  if (key) {
    for (var i = 0; i < queue.length; i++) {
      if (queue[i].key === key && i > 0) { queue[i].msg = msg; return; }
    }
  }
  queue.push({ msg: msg, key: key, tries: 0 });
  pump();
}

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  var item = queue[0];
  Pebble.sendAppMessage(item.msg, function () {
    queue.shift();
    sending = false;
    pump();
  }, function () {
    item.tries++;
    if (item.tries > 3) queue.shift();
    sending = false;
    setTimeout(pump, 250);
  });
}

// Truncate a string so its UTF-8 encoding fits in maxBytes.
function fit(str, maxBytes) {
  str = String(str || '').replace(/\s+/g, ' ').trim();
  var bytes = 0, out = '';
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i), n;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
      n = 4;
      if (bytes + n > maxBytes) break;
      out += str[i] + str[i + 1]; i++; bytes += n; continue;
    }
    n = c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
    if (bytes + n > maxBytes) break;
    out += str[i]; bytes += n;
  }
  return out;
}

function deviceMsg(i) {
  var d = devices[i];
  return {
    IDX: i,
    NAME: fit(d.name, 31),
    TYPE: d.type,
    ONLINE: d.online ? 1 : 0,
    STATE: d.state,
    SOURCE: fit(d.source, 23),
    TITLE: fit(d.title, 63),
    ARTIST: fit(d.artist, 47),
    VOLUME: Math.max(0, Math.min(255, d.volume || 0)),
    VOLMAX: Math.max(0, Math.min(255, d.volMax || 0)),
    GROUP: d.group || 0,
    ROLE: d.role || 0
  };
}

function sendDevice(i) { enqueue(deviceMsg(i), 'dev' + i); }

function sendAll() {
  enqueue({ COUNT: Math.min(devices.length, MAX_DEVICES) });
  computeGroups();
  for (var i = 0; i < devices.length && i < MAX_DEVICES; i++) {
    devices[i].lastSig = JSON.stringify(deviceMsg(i));
    sendDevice(i);
  }
}

function toast(text) { enqueue({ TOAST: fit(text, 47) }); }

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
function http(method, url, body, timeout, cb) {
  var x = new XMLHttpRequest();
  var done = false;
  var timer = setTimeout(function () {
    try { x.abort(); } catch (e) {}
    finish(0, '');
  }, timeout);
  function finish(status, text) {
    if (done) return;
    done = true;
    clearTimeout(timer);
    cb(status, text);
  }
  x.onreadystatechange = function () {
    if (x.readyState === 4) finish(x.status, x.responseText);
  };
  x.onerror = function () { finish(0, ''); };
  try {
    x.open(method, url, true);
    if (body !== null && body !== undefined) {
      x.setRequestHeader('Content-Type', 'application/json');
      x.send(body);
    } else {
      x.send();
    }
  } catch (e) { finish(0, ''); }
}

function parse(text) { try { return JSON.parse(text); } catch (e) { return null; } }

// ASE's /BeoNotify/Notifications is a never-ending stream of JSON lines. On
// connect it dumps the current state, so we read for a short while and abort.
function readNotifications(ip, ms, cb) {
  var x = new XMLHttpRequest();
  var done = false;
  function text() { try { return x.responseText || ''; } catch (e) { return ''; } }
  function finish() {
    if (done) return;
    done = true;
    var t = text();
    try { x.abort(); } catch (e) {}
    cb(t);
  }
  function check() {
    if (x.readyState === 4) { finish(); return; }
    var t = text();
    if (t.indexOf('PROGRESS_INFORMATION') >= 0 && t.indexOf('SOURCE') >= 0) finish();
  }
  x.onreadystatechange = check;
  x.onprogress = check;
  x.onerror = finish;
  try {
    x.open('GET', 'http://' + ip + ':8080/BeoNotify/Notifications', true);
    x.send();
  } catch (e) { finish(); return; }
  setTimeout(finish, ms);
}

function aseStateFromString(s) {
  if (s === 'play') return ST_PLAYING;
  if (s === 'pause') return ST_PAUSED;
  return ST_IDLE;
}

function parseAseNotifications(text, st) {
  var lines = text.split(/\r?\n/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line) continue;
    var j = parse(line);
    if (!j || !j.notification) continue;
    var n = j.notification, data = n.data || {};
    switch (n.type) {
      case 'SOURCE':
      case 'SOURCE_EXPERIENCE_CHANGED':
        if (data.primaryExperience) {
          var pe = data.primaryExperience;
          if (pe.source && pe.source.friendlyName) st.source = pe.source.friendlyName;
          if (pe.state) st.state = aseStateFromString(pe.state);
        } else {
          st.source = '';
          st.state = ST_IDLE;
        }
        st.seen = true;
        break;
      case 'PROGRESS_INFORMATION':
        if (data.state) st.state = aseStateFromString(data.state);
        st.seen = true;
        break;
      case 'NOW_PLAYING_STORED_MUSIC':
        st.title = data.name || '';
        st.artist = data.artist || '';
        break;
      case 'NOW_PLAYING_NET_RADIO':
        st.title = data.liveDescription || data.name || '';
        st.artist = data.liveDescription ? (data.name || '') : '';
        break;
      case 'NOW_PLAYING_STORED_VIDEO':
        st.title = data.name || '';
        st.artist = '';
        break;
      case 'NUMBER_AND_NAME':
        if (!st.title) st.title = data.name || '';
        break;
      case 'NOW_PLAYING_ENDED':
        st.title = '';
        st.artist = '';
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Polling device state
// ---------------------------------------------------------------------------
function pollMozart(d, cb) {
  http('GET', 'http://' + d.ip + '/api/v1/playback/state', null, 3000, function (status, text) {
    var j = status === 200 ? parse(text) : null;
    if (!j || !j.state) { cb({ online: 0 }); return; }
    var v = j.state.value;
    var md = j.metadata || {};
    var state = (v === 'started' || v === 'buffering') ? ST_PLAYING :
                (v === 'paused' ? ST_PAUSED : ST_IDLE);
    var r = {
      online: 1,
      state: state,
      source: (j.source && j.source.name) || '',
      title: md.title || '',
      artist: md.artistName || md.organization || md.genre || '',
      leaderJid: (md.remoteLeader && md.remoteLeader.jid) || d.jid,
      leaderName: (md.remoteLeader && md.remoteLeader.friendlyName) || ''
    };
    http('GET', 'http://' + d.ip + '/api/v1/sound/volume', null, 3000, function (s2, t2) {
      var v2 = s2 === 200 ? parse(t2) : null;
      if (v2 && v2.level) {
        r.volume = v2.level.level;
        r.volMax = (v2.maximum && v2.maximum.level) || 100;
      }
      cb(r);
    });
  });
}

function pollAse(d, cb) {
  http('GET', 'http://' + d.ip + ':8080/BeoZone/Zone/ActiveSources', null, 3000, function (status, text) {
    var j = status === 200 ? parse(text) : null;
    if (!j) { cb({ online: 0 }); return; }
    var pe = j.primaryExperience || {};
    var src = pe.source || null;
    var st = {
      online: 1,
      state: src && src.inUse ? ST_PLAYING : ST_IDLE,
      source: src ? (src.friendlyName || '') : '',
      title: '', artist: '', seen: false,
      leaderJid: (src && src.product && src.product.jid) || d.jid,
      leaderName: (src && src.product && src.product.friendlyName) || ''
    };
    readNotifications(d.ip, 1500, function (t) {
      var fallbackState = st.state;
      parseAseNotifications(t, st);
      if (!st.seen) st.state = fallbackState;
      if (!src) st.state = ST_IDLE;
      delete st.seen;
      http('GET', 'http://' + d.ip + ':8080/BeoZone/Zone/Sound/Volume/Speaker', null, 3000, function (s2, t2) {
        var v2 = s2 === 200 ? parse(t2) : null;
        if (v2 && v2.speaker) {
          st.volume = v2.speaker.level;
          st.volMax = (v2.speaker.range && v2.speaker.range.maximum) || 90;
        }
        cb(st);
      });
    });
  });
}

// Each device's Beolink JID identifies it as a multiroom leader.
function fetchJid(d, cb) {
  if (d.jid) { cb(); return; }
  if (d.type === TYPE_MOZART) {
    http('GET', 'http://' + d.ip + '/api/v1/beolink/self', null, 3000, function (s, t) {
      var j = s === 200 ? parse(t) : null;
      if (j && j.jid) { d.jid = j.jid; saveDevices(); }
      cb();
    });
  } else {
    http('GET', 'http://' + d.ip + ':8080/BeoDevice', null, 3000, function (s, t) {
      var j = s === 200 ? parse(t) : null;
      var jid = j && j.beoDevice ? aseJid(j.beoDevice) : '';
      if (jid) { d.jid = jid; saveDevices(); }
      cb();
    });
  }
}

function aseJid(bd) {
  var p = bd.productId || {};
  if (!p.typeNumber || !p.itemNumber || !p.serialNumber) return '';
  return p.typeNumber + '.' + p.itemNumber + '.' + p.serialNumber + '@products.bang-olufsen.com';
}

function pollDevice(i) {
  var d = devices[i];
  if (!d || d.busy) return;
  d.busy = true;
  fetchJid(d, function () {
    var fn = d.type === TYPE_MOZART ? pollMozart : pollAse;
    fn(d, function (r) {
      d.busy = false;
      if (devices[i] !== d) return;  // list changed meanwhile
      d.online = r.online;
      if (r.online) {
        d.state = r.state; d.source = r.source; d.title = r.title; d.artist = r.artist;
        d.leaderJid = r.leaderJid || d.jid;
        if (d.offUntil && Date.now() < d.offUntil) {
          // just sent to standby: the speaker may still report the old state
          d.state = ST_IDLE; d.title = ''; d.artist = '';
        }
        if (r.volMax) {
          // don't overwrite a volume the user just set (the speaker may lag)
          if (!d.volUntil || Date.now() > d.volUntil) d.volume = r.volume;
          d.volMax = r.volMax;
        }
      } else {
        d.state = ST_IDLE; d.source = ''; d.title = ''; d.artist = ''; d.leaderJid = '';
      }
      flush();
    });
  });
}

// Work out Beolink multiroom groups: active devices that share a leader.
function computeGroups() {
  var byLeader = {};
  devices.forEach(function (d, i) {
    d.group = 0; d.role = 0;
    if (!d.online || d.state === ST_IDLE || !d.leaderJid) return;
    (byLeader[d.leaderJid] = byLeader[d.leaderJid] || []).push(i);
  });
  var nextId = 1;
  Object.keys(byLeader).forEach(function (jid) {
    var members = byLeader[jid];
    if (members.length < 2) return;
    var id = nextId++;
    var leader = null;
    members.forEach(function (i) {
      var d = devices[i];
      d.group = id;
      d.role = d.jid && d.jid === jid ? 1 : 2;
      if (d.role === 1) leader = d;
    });
    // followers often don't report the track: show the leader's
    if (leader) {
      members.forEach(function (i) {
        var d = devices[i];
        if (d !== leader && !d.title) { d.title = leader.title; d.artist = leader.artist; }
      });
    }
  });
}

// Send every device whose watch-visible data changed.
function flush() {
  computeGroups();
  for (var i = 0; i < devices.length && i < MAX_DEVICES; i++) {
    var sig = JSON.stringify(deviceMsg(i));
    if (sig !== devices[i].lastSig) {
      devices[i].lastSig = sig;
      sendDevice(i);
    }
  }
}

var lastFullPoll = 0;
function pollLoop() {
  clearTimeout(pollTimer);
  var now = Date.now();
  if (!scanning) {
    if (now - lastFullPoll >= POLL_MS) {
      lastFullPoll = now;
      for (var i = 0; i < devices.length && i < MAX_DEVICES; i++) pollDevice(i);
    } else if (focused >= 0 && focused < devices.length) {
      pollDevice(focused);
    }
  }
  pollTimer = setTimeout(pollLoop, focused >= 0 ? FOCUS_POLL_MS : POLL_MS);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
function toggle(i) {
  var d = devices[i];
  if (!d) return;
  var play = d.state !== ST_PLAYING;
  var done = function (status) {
    if (status < 200 || status >= 300) toast((play ? 'Play' : 'Pause') + ' failed (' + status + ')');
    setTimeout(function () { pollDevice(i); }, 800);
  };
  if (d.type === TYPE_MOZART) {
    http('POST', 'http://' + d.ip + '/api/v1/playback/command/' + (play ? 'play' : 'pause'),
         null, 4000, done);
  } else {
    http('POST', 'http://' + d.ip + ':8080/BeoZone/Zone/Stream/' + (play ? 'Play' : 'Pause'),
         '{}', 4000, done);
  }
}

function join(i) {
  var d = devices[i];
  if (!d) return;
  var done = function (status, text) {
    if (status >= 200 && status < 300) {
      toast('Join requested');
    } else if (status === 400 && d.type === TYPE_MOZART) {
      toast('Beolink not enabled');
    } else if (status === 403) {
      toast('Join not allowed');
    } else {
      toast('Join failed (' + status + ')');
    }
    setTimeout(function () { pollDevice(i); }, 2500);
  };
  if (d.type === TYPE_MOZART) {
    http('POST', 'http://' + d.ip + '/api/v1/beolink/join', null, 5000, done);
  } else {
    http('POST', 'http://' + d.ip + ':8080/BeoZone/Zone/Device/OneWayJoin', '{}', 5000, done);
  }
}

function setVolume(i, level) {
  var d = devices[i];
  if (!d) return;
  level = Math.max(0, Math.min(d.volMax || 100, level | 0));
  d.volume = level;
  d.volUntil = Date.now() + 3000;
  var done = function (status) {
    if (status < 200 || status >= 300) toast('Volume failed (' + status + ')');
  };
  var body = JSON.stringify({ level: level });
  if (d.type === TYPE_MOZART) {
    http('PUT', 'http://' + d.ip + '/api/v1/sound/volume/level', body, 4000, done);
  } else {
    http('PUT', 'http://' + d.ip + ':8080/BeoZone/Zone/Sound/Volume/Speaker/Level', body, 4000, done);
  }
}

function skip(i, forward) {
  var d = devices[i];
  if (!d) return;
  var done = function (status) {
    if (status < 200 || status >= 300) toast((forward ? 'Next' : 'Previous') + ' failed (' + status + ')');
    setTimeout(function () { pollDevice(i); }, 1200);
  };
  if (d.type === TYPE_MOZART) {
    http('POST', 'http://' + d.ip + '/api/v1/playback/command/' + (forward ? 'skip' : 'prev'),
         null, 4000, done);
  } else {
    http('POST', 'http://' + d.ip + ':8080/BeoZone/Zone/Stream/' + (forward ? 'Forward' : 'Backward'),
         '{}', 4000, done);
  }
}

function standby(d, cb) {
  d.offUntil = Date.now() + 6000;
  d.state = ST_IDLE; d.title = ''; d.artist = '';
  if (d.type === TYPE_MOZART) {
    http('PUT', 'http://' + d.ip + '/api/v1/state/standby', null, 4000, cb);
  } else {
    http('PUT', 'http://' + d.ip + ':8080/BeoDevice/powerManagement/standby',
         JSON.stringify({ standby: { powerState: 'standby' } }), 4000, cb);
  }
}

// Power off a speaker; if it leads a multiroom group, the whole group.
function powerOff(i) {
  var d = devices[i];
  if (!d) return;
  var targets = [d];
  if (d.group && d.role === 1) {
    targets = devices.filter(function (o) { return o.group === d.group; });
  }
  var pending = targets.length, failed = 0;
  targets.forEach(function (t) {
    standby(t, function (status) {
      if (status < 200 || status >= 300) failed++;
      if (--pending === 0) {
        if (failed) toast('Power off failed on ' + failed + ' of ' + targets.length);
        flush();
      }
    });
  });
  flush();
}

// Make one speaker leave the multiroom experience it is following.
function leave(d, cb) {
  if (d.type === TYPE_MOZART) {
    http('POST', 'http://' + d.ip + '/api/v1/beolink/leave', null, 4000, cb);
  } else {
    http('DELETE', 'http://' + d.ip + ':8080/BeoZone/Zone/ActiveSources/primaryExperience', null, 4000, cb);
  }
}

// Unlink: a follower leaves its group; a leader releases all its followers
// (and keeps playing itself).
function unlink(i) {
  var d = devices[i];
  if (!d || !d.group) { toast('Not in a group'); return; }
  var targets = [d];
  if (d.role === 1) {
    targets = devices.filter(function (o) { return o.group === d.group && o !== d; });
  }
  var pending = targets.length, failed = 0;
  targets.forEach(function (t) {
    leave(t, function (status) {
      if (status < 200 || status >= 300) failed++;
      if (--pending === 0) {
        if (failed) toast('Unlink failed on ' + failed + ' of ' + targets.length);
        else toast(d.role === 1 ? 'Group released' : 'Left group');
        setTimeout(function () {
          devices.forEach(function (o, k) { if (o.group === d.group) pollDevice(k); });
        }, 1500);
      }
    });
  });
}

// ---------------------------------------------------------------------------
// Network discovery: probe every host on the /24 for both APIs.
// ---------------------------------------------------------------------------
function probe(ip, cb) {
  var pending = 2, result = null;
  function done() { if (--pending === 0) cb(result); }
  http('GET', 'http://' + ip + '/api/v1/beolink/self', null, 1500, function (s, t) {
    var j = s === 200 ? parse(t) : null;
    if (j && j.friendlyName && /bang-olufsen/.test(j.jid || '')) {
      result = { ip: ip, type: TYPE_MOZART, name: j.friendlyName, jid: j.jid };
    }
    done();
  });
  http('GET', 'http://' + ip + ':8080/BeoDevice', null, 1500, function (s, t) {
    var j = s === 200 ? parse(t) : null;
    if (j && j.beoDevice && !result) {
      var bd = j.beoDevice;
      var name = (bd.productFriendlyName && bd.productFriendlyName.productFriendlyName) ||
                 (bd.productId && bd.productId.productType) || ip;
      result = { ip: ip, type: TYPE_ASE, name: name, jid: aseJid(bd) };
    }
    done();
  });
}

function scan() {
  if (scanning) return;
  scanning = true;
  var subnet = getSubnet();
  var next = 1, finished = 0, total = 254, found = [], lastPct = -1;
  var CONCURRENCY = 24;
  console.log('Scanning ' + subnet + '.0/24');
  enqueue({ SCAN: 0, FOUND: 0 }, 'scan');

  function worker() {
    if (next > total) return;
    var ip = subnet + '.' + (next++);
    probe(ip, function (r) {
      if (r) { found.push(r); console.log('Found ' + r.name + ' @ ' + r.ip); }
      finished++;
      var pct = Math.floor(finished * 100 / total);
      if (pct >= lastPct + 5 || finished === total) {
        lastPct = pct;
        enqueue({ SCAN: Math.min(pct, 100), FOUND: found.length }, 'scan');
      }
      if (finished === total) complete();
      else worker();
    });
  }

  function complete() {
    // merge with known devices (by IP)
    found.forEach(function (f) {
      var existing = null;
      devices.forEach(function (d) { if (d.ip === f.ip) existing = d; });
      if (existing) { existing.name = f.name; existing.type = f.type; existing.jid = f.jid; }
      else devices.push({ ip: f.ip, type: f.type, name: f.name, jid: f.jid, online: 1, state: ST_IDLE,
                          source: '', title: '', artist: '' });
    });
    devices.sort(function (a, b) { return a.name.localeCompare(b.name); });
    if (devices.length > MAX_DEVICES) devices.length = MAX_DEVICES;
    saveDevices();
    scanning = false;
    enqueue({ SCAN: 101, FOUND: found.length }, 'scan');
    sendAll();
    lastFullPoll = 0;
    pollLoop();
  }

  for (var i = 0; i < CONCURRENCY; i++) worker();
}

function clearDevices() {
  devices = [];
  saveDevices();
  sendAll();
  toast('All devices deleted');
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
Pebble.addEventListener('ready', function () {
  loadDevices();
  sendAll();
  lastFullPoll = 0;
  pollLoop();
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  var cmd = p.CMD, idx = p.IDX;
  if (cmd === CMD_LIST) sendAll();
  else if (cmd === CMD_SCAN) scan();
  else if (cmd === CMD_CLEAR) clearDevices();
  else if (cmd === CMD_TOGGLE) toggle(idx);
  else if (cmd === CMD_JOIN) join(idx);
  else if (cmd === CMD_VOLUME) setVolume(idx, p.VALUE);
  else if (cmd === CMD_PREV) skip(idx, false);
  else if (cmd === CMD_NEXT) skip(idx, true);
  else if (cmd === CMD_POWER_OFF) powerOff(idx);
  else if (cmd === CMD_UNLINK) unlink(idx);
  else if (cmd === CMD_FOCUS) {
    focused = (typeof idx === 'number' && idx >= 0) ? idx : -1;
    if (focused >= 0) pollDevice(focused);
    pollLoop();
  }
});

// Phone-side settings page: lets you change the subnet that gets scanned.
Pebble.addEventListener('showConfiguration', function () {
  var subnet = getSubnet();
  var html = '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>B&amp;O Control</title><style>body{font-family:sans-serif;background:#111;color:#eee;padding:20px}' +
    'input{font-size:18px;padding:8px;width:100%;box-sizing:border-box;margin:8px 0 16px}' +
    'button{font-size:18px;padding:10px 20px;background:#fff;color:#000;border:0;border-radius:4px}</style></head>' +
    '<body><h2>B&amp;O Control</h2><label>Network to scan (first three parts of the IP)</label>' +
    '<input id="s" value="' + subnet + '" placeholder="192.168.1">' +
    '<button onclick="location.href=\'pebblejs://close#\'+encodeURIComponent(JSON.stringify({subnet:document.getElementById(\'s\').value}))">Save</button>' +
    '</body></html>';
  Pebble.openURL('data:text/html,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var cfg = parse(decodeURIComponent(e.response));
  if (cfg && /^\d+\.\d+\.\d+$/.test((cfg.subnet || '').trim())) {
    localStorage.setItem('subnet', cfg.subnet.trim());
    toast('Subnet ' + cfg.subnet.trim());
  }
});
