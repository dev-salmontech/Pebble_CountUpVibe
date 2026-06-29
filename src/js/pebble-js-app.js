/* CountUpVibe phone configuration (PebbleKit JS).
 *
 * Self-contained config page served as a data: URI -- no external hosting and no
 * Clay/npm dependency, so it builds on CloudPebble as-is. The page collects the
 * three settings, returns them as JSON via pebblejs://close, and we forward them
 * to the watch over AppMessage. The phone copy is mirrored in localStorage so the
 * page can pre-fill the user's current values next time.
 *
 * Keys sent to the watch must match appinfo.json "appKeys".
 */

var DEFAULTS = { interval: 300, step: 15, color: 0x55AAFF };
var STEPS = [1, 5, 10, 15, 20, 30];
var PALETTE = [
  0xFFFFFF, 0x000000, 0xFF0000, 0xFF5500, 0xFFAA00, 0xFFFF00,
  0xAAFF00, 0x00FF00, 0x00FFAA, 0x00FFFF, 0x55AAFF, 0x0055FF,
  0x0000FF, 0xAA00FF, 0xFF00FF, 0xFF55AA, 0xAAAAAA, 0x555555
];

function loadSettings() {
  var s = {};
  try { s = JSON.parse(localStorage.getItem('cuv_settings')) || {}; } catch (e) {}
  return {
    interval: typeof s.interval === 'number' ? s.interval : DEFAULTS.interval,
    step: typeof s.step === 'number' ? s.step : DEFAULTS.step,
    color: typeof s.color === 'number' ? s.color : DEFAULTS.color
  };
}

function hex(v) { return '#' + ('000000' + (v >>> 0).toString(16)).slice(-6); }

function buildHtml(cur) {
  var pal = PALETTE.slice();
  if (pal.indexOf(cur.color) < 0) { pal.unshift(cur.color); }
  var mm = Math.floor(cur.interval / 60), ss = cur.interval % 60;

  var opts = STEPS.map(function (v) {
    return '<option value="' + v + '"' + (v === cur.step ? ' selected' : '') + '>' + v + ' s</option>';
  }).join('');

  var swatches = pal.map(function (v) {
    var light = ((v >> 16 & 255) * 0.299 + (v >> 8 & 255) * 0.587 + (v & 255) * 0.114) > 140;
    return '<div class="sw" data-c="' + v + '" style="background:' + hex(v) +
           ';color:' + (light ? '#000' : '#fff') + '"></div>';
  }).join('');

  return '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<style>' +
    'body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;background:#f2f2f2;color:#222}' +
    '.bar{background:#0a84ff;color:#fff;padding:14px 16px;font-size:18px;font-weight:600}' +
    '.card{background:#fff;margin:12px;border-radius:10px;padding:14px}' +
    'h2{font-size:13px;text-transform:uppercase;color:#888;margin:0 0 10px}' +
    'input[type=number]{width:64px;font-size:18px;padding:6px;text-align:center;border:1px solid #ccc;border-radius:6px}' +
    'select{font-size:16px;padding:8px;border:1px solid #ccc;border-radius:6px;width:100%}' +
    '.row{display:flex;align-items:center;gap:8px}' +
    '.pal{display:flex;flex-wrap:wrap;gap:8px}' +
    '.sw{width:42px;height:42px;border-radius:8px;border:1px solid #00000022;' +
    'display:flex;align-items:center;justify-content:center;font-size:22px;cursor:pointer}' +
    '.sw.sel{outline:3px solid #0a84ff}' +
    'button{font-size:17px;padding:12px;border:0;border-radius:8px;width:100%;margin-top:6px}' +
    '#save{background:#0a84ff;color:#fff}#reset{background:#e5e5ea;color:#111}' +
    '.hint{font-size:12px;color:#999;margin-top:8px}' +
    '</style></head><body>' +
    '<div class="bar">CountUpVibe Settings</div>' +
    '<div class="card"><h2>Default vibration interval</h2>' +
    '<div class="row"><input id="mm" type="number" min="0" max="99" value="' + mm + '"><span>min</span>' +
    '<input id="ss" type="number" min="0" max="59" value="' + ss + '"><span>sec</span></div>' +
    '<div class="hint">00:00 to 99:59 (default 05:00)</div></div>' +
    '<div class="card"><h2>Minimum interval step</h2><select id="step">' + opts + '</select>' +
    '<div class="hint">On the watch the seconds toggle in multiples of this.</div></div>' +
    '<div class="card"><h2>Water colour</h2><div class="pal" id="pal">' + swatches + '</div></div>' +
    '<div class="card"><button id="reset">Reset to defaults</button><button id="save">Save</button></div>' +
    '<script>' +
    'var DEF={interval:' + DEFAULTS.interval + ',step:' + DEFAULTS.step + ',color:' + DEFAULTS.color + '};' +
    'var sel=' + cur.color + ';' +
    'function mark(){var n=document.querySelectorAll(".sw");for(var i=0;i<n.length;i++){' +
    'var c=parseInt(n[i].getAttribute("data-c"),10);n[i].className="sw"+(c===sel?" sel":"");' +
    'n[i].innerHTML=(c===sel?"\\u2713":"");}}' +
    'document.getElementById("pal").addEventListener("click",function(e){var t=e.target;' +
    'if(t.className.indexOf("sw")<0)return;sel=parseInt(t.getAttribute("data-c"),10);mark();});' +
    'document.getElementById("reset").addEventListener("click",function(){' +
    'document.getElementById("mm").value=Math.floor(DEF.interval/60);' +
    'document.getElementById("ss").value=DEF.interval%60;' +
    'document.getElementById("step").value=DEF.step;sel=DEF.color;mark();});' +
    'document.getElementById("save").addEventListener("click",function(){' +
    'var mm=parseInt(document.getElementById("mm").value,10)||0;' +
    'var ss=parseInt(document.getElementById("ss").value,10)||0;' +
    'if(mm<0)mm=0;if(mm>99)mm=99;if(ss<0)ss=0;if(ss>59)ss=59;' +
    'var step=parseInt(document.getElementById("step").value,10)||15;' +
    'var out={interval:mm*60+ss,step:step,color:sel};' +
    'location.href="pebblejs://close#"+encodeURIComponent(JSON.stringify(out));});' +
    'mark();' +
    '<\/script></body></html>';
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL('data:text/html,' + encodeURIComponent(buildHtml(loadSettings())));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) { return; }
  var d;
  try { d = JSON.parse(decodeURIComponent(e.response)); } catch (err) { return; }

  var interval = parseInt(d.interval, 10);
  if (isNaN(interval) || interval < 0) { interval = DEFAULTS.interval; }
  if (interval > 5999) { interval = 5999; }

  var step = parseInt(d.step, 10);
  if (STEPS.indexOf(step) < 0) { step = DEFAULTS.step; }

  var color = parseInt(d.color, 10);
  if (isNaN(color)) { color = DEFAULTS.color; }

  localStorage.setItem('cuv_settings', JSON.stringify({ interval: interval, step: step, color: color }));

  Pebble.sendAppMessage(
    { 'INTERVAL_DEFAULT': interval, 'MIN_STEP': step, 'WATER_COLOR': color },
    function () {}, function () {}
  );
});
