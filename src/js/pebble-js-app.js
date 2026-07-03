/* CountUpVibe phone configuration (PebbleKit JS).
 *
 * Self-contained config page served as a data: URI -- no external hosting and no
 * Clay/npm dependency, so it builds on CloudPebble as-is. The page collects the
 * three settings, returns them as JSON via pebblejs://close, and we forward them
 * to the watch over AppMessage. The phone copy is mirrored in localStorage so the
 * page can pre-fill the user's current values next time.
 *
 * Order: (1) minimum interval step, (2) default vibration interval -- minutes by
 * typing, seconds as a dropdown of step-multiples (free typing only when step is
 * 1 s), (3) water colour. Keys sent must match appinfo.json "appKeys".
 */

var DEFAULTS = { interval: 300, step: 15, color: 0x55AAFF };
var STEPS = [1, 5, 10, 15, 20, 30];
/* The full Pebble 64-colour palette laid out so that grid-adjacent cells are
 * perceptual shades of one another (a smooth colour map). Rendered below as a
 * hexagonal honeycomb. Layout is the standard Pebble/Clay COLOR picker map
 * (from KiserDesigns/Pebble_RomanShadow's custom-color.js). 0 = empty cell. */
var COLOR_LAYOUT = [
  [0,        0,        '55ff00', 'aaff55', 0,        'ffff55', 'ffffaa', 0,        0       ],
  [0,        'aaffaa', '55ff55', '00ff00', 'aaff00', 'ffff00', 'ffaa55', 'ffaaaa', 0       ],
  ['55ffaa', '00ff55', '00aa00', '55aa00', 'aaaa55', 'aaaa00', 'ffaa00', 'ff5500', 'ff5555'],
  ['aaffff', '00ffaa', '00aa55', '55aa55', '005500', '555500', 'aa5500', 'ff0000', 'ff0055'],
  [0,        '55aaaa', '00aaaa', '005555', 'ffffff', '000000', 'aa5555', 'aa0000', 0       ],
  ['55ffff', '00ffff', '00aaff', '0055aa', 'aaaaaa', '555555', '550000', 'aa0055', 'ff55aa'],
  ['55aaff', '0055ff', '0000ff', '0000aa', '000055', '550055', 'aa00aa', 'ff00aa', 'ffaaff'],
  [0,        '5555aa', '5555ff', '5500ff', '5500aa', 'aa00ff', 'ff00ff', 'ff55ff', 0       ],
  [0,        0,        0,        'aaaaff', 'aa55ff', 'aa55aa', 0,        0,        0       ]
];

var STYLE =
  'body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;background:#f2f2f2;color:#222}' +
  '.bar{background:#0a84ff;color:#fff;padding:14px 16px;font-size:18px;font-weight:600}' +
  '.card{background:#fff;margin:12px;border-radius:10px;padding:14px}' +
  'h2{font-size:13px;text-transform:uppercase;color:#888;margin:0 0 10px}' +
  'input[type=number],select{font-size:18px;padding:6px;border:1px solid #ccc;border-radius:6px}' +
  'input[type=number]{width:64px;text-align:center}' +
  '#step{width:100%}' +
  '.row{display:flex;align-items:center;gap:8px}' +
  /* Hexagonal honeycomb colour picker. Cell/row sizes are set by JS so the
   * comb scales to the phone width; here we only define shape and behaviour. */
  '.pal{display:block;margin:2px auto 0;position:relative}' +
  '.hrow{display:flex;justify-content:flex-start}' +
  '.hx{flex:0 0 auto;position:relative;box-sizing:border-box;' +
  '-webkit-clip-path:polygon(50% 0,100% 25%,100% 75%,50% 100%,0 75%,0 25%);' +
  'clip-path:polygon(50% 0,100% 25%,100% 75%,50% 100%,0 75%,0 25%);' +
  'display:flex;align-items:center;justify-content:center;font-weight:700;' +
  'line-height:1;cursor:pointer;-webkit-user-select:none;user-select:none}' +
  '.hx.sp{visibility:hidden;cursor:default}' +
  '.hx .ck{opacity:0}' +
  '.hx.sel .ck{opacity:1}' +
  '.hx.sel{box-shadow:inset 0 0 0 3px #fff,inset 0 0 0 5px rgba(0,0,0,.45)}' +
  'button{font-size:17px;padding:12px;border:0;border-radius:8px;width:100%;margin-top:6px}' +
  '#save{background:#0a84ff;color:#fff}#reset{background:#e5e5ea;color:#111}' +
  '.hint{font-size:12px;color:#999;margin-top:8px}' +
  /* Follow the phone's dark/light theme. */
  '@media (prefers-color-scheme:dark){' +
  'body{background:#000;color:#eee}' +
  '.card{background:#1c1c1e}' +
  'h2{color:#9a9a9e}' +
  'input[type=number],select{background:#2c2c2e;color:#eee;border-color:#48484a}' +
  '#reset{background:#2c2c2e;color:#eee}' +
  '.hint{color:#8e8e93}' +
  '}';

function loadSettings() {
  var s = {};
  try { s = JSON.parse(localStorage.getItem('cuv_settings')) || {}; } catch (e) {}
  return {
    interval: typeof s.interval === 'number' ? s.interval : DEFAULTS.interval,
    step: typeof s.step === 'number' ? s.step : DEFAULTS.step,
    color: typeof s.color === 'number' ? s.color : DEFAULTS.color
  };
}

function sendSettings(s) {
  Pebble.sendAppMessage(
    { 'INTERVAL_DEFAULT': s.interval, 'MIN_STEP': s.step, 'WATER_COLOR': s.color },
    function () {}, function () {}
  );
}

function buildHtml(cur) {
  var mm = Math.floor(cur.interval / 60), ss = cur.interval % 60;

  var stepOpts = STEPS.map(function (v) {
    return '<option value="' + v + '"' + (v === cur.step ? ' selected' : '') + '>' + v + ' s</option>';
  }).join('');

  // Honeycomb: one .hrow per layout row, one .hx (hexagon) per cell. Empty
  // cells become invisible spacers so the offset rows stay aligned.
  var comb = COLOR_LAYOUT.map(function (row) {
    var cells = row.map(function (c) {
      if (!c) { return '<div class="hx sp"></div>'; }
      var v = parseInt(c, 16);
      var light = (parseInt(c.slice(0, 2), 16) * 0.299 +
                   parseInt(c.slice(2, 4), 16) * 0.587 +
                   parseInt(c.slice(4), 16) * 0.114) > 140;
      return '<div class="hx" data-c="' + v + '" style="background:#' + c + '">' +
             '<span class="ck" style="color:' + (light ? '#000' : '#fff') + '">✓</span></div>';
    }).join('');
    return '<div class="hrow">' + cells + '</div>';
  }).join('');

  return '<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<style>' + STYLE + '</style></head><body>' +
    '<div class="bar">CountUpVibe Settings</div>' +
    '<div class="card"><h2>Minimum interval step</h2><select id="step">' + stepOpts + '</select>' +
    '<div class="hint">On the watch the seconds toggle in multiples of this. It also sets the seconds options below.</div></div>' +
    '<div class="card"><h2>Default vibration interval</h2>' +
    '<div class="row"><input id="mm" type="number" min="0" max="99" value="' + mm + '"><span>min</span>' +
    '<span id="secwrap"></span><span>sec</span></div>' +
    '<div class="hint">00:00 to 99:59 (default 05:00). Seconds step in multiples of the minimum; free typing only when the step is 1 s.</div></div>' +
    '<div class="card"><h2>Water colour</h2><div class="pal" id="pal">' + comb + '</div>' +
    '<div class="hint">Tap a cell. Neighbouring cells are shades of the same colour.</div></div>' +
    '<div class="card"><button id="reset">Reset to defaults</button><button id="save">Save</button></div>' +
    '<script>' +
    'var DEF={interval:' + DEFAULTS.interval + ',step:' + DEFAULTS.step + ',color:' + DEFAULTS.color + '};' +
    'var sel=' + cur.color + ';' +
    'function pad(n){return (n<10?"0":"")+n;}' +
    'function cleanSec(r){r=(""+r).trim();if(!/^\\d+$/.test(r))return 0;var n=parseInt(r,10);return n>59?59:n;}' +
    'function mults(s){var a=[];for(var i=0;i<60;i+=s){a.push(i);}return a;}' +
    'function renderSec(step,curv){var w=document.getElementById("secwrap");' +
    'if(step===1){var v=cleanSec(curv);' +
    'w.innerHTML="<input id=\\"ss\\" type=\\"number\\" min=\\"0\\" max=\\"59\\" value=\\""+v+"\\">";}' +
    'else{var m=mults(step);if(m.indexOf(curv)<0){curv=0;}var o="";' +
    'for(var i=0;i<m.length;i++){o+="<option value=\\""+m[i]+"\\""+(m[i]===curv?" selected":"")+">"+pad(m[i])+"</option>";}' +
    'w.innerHTML="<select id=\\"ss\\">"+o+"</select>";}}' +
    'function readSec(step){var el=document.getElementById("ss");if(!el){return 0;}' +
    'if(step===1){return cleanSec(el.value);}return parseInt(el.value,10)||0;}' +
    'function mark(){var n=document.querySelectorAll(".hx");for(var i=0;i<n.length;i++){' +
    'var d=n[i].getAttribute("data-c");if(d===null){continue;}' +
    'var c=parseInt(d,10);if(c===sel){n[i].className="hx sel";}else{n[i].className="hx";}}}' +
    /* Size the comb to the phone width: pointy-top hexagons, odd rows shifted
       half a cell, rows overlapping vertically by a quarter of the hex height. */
    'function layoutPal(){var pal=document.getElementById("pal");pal.style.width="auto";' +
    'var avail=pal.clientWidth||300;var hw=Math.floor(avail/9.5);if(hw>46){hw=46;}if(hw<18){hw=18;}' +
    'var hh=Math.round(hw/0.866);pal.style.width=Math.round(hw*9.5)+"px";' +
    'var rows=pal.getElementsByClassName("hrow");' +
    'for(var r=0;r<rows.length;r++){rows[r].style.height=hh+"px";' +
    'rows[r].style.marginTop=(r?(-Math.round(hh*0.25))+"px":"0");' +
    'rows[r].style.paddingLeft=((r%2)?Math.round(hw/2):0)+"px";' +
    'var hx=rows[r].getElementsByClassName("hx");' +
    'for(var k=0;k<hx.length;k++){hx[k].style.width=hw+"px";hx[k].style.height=hh+"px";' +
    'hx[k].style.fontSize=Math.round(hw*0.5)+"px";}}' +
    'pal.style.height=(hh+(rows.length-1)*Math.round(hh*0.75))+"px";}' +
    'window.addEventListener("resize",layoutPal);' +
    'document.getElementById("step").addEventListener("change",function(){' +
    'var st=parseInt(this.value,10);' +
    'var cv=document.getElementById("ss")?(parseInt(document.getElementById("ss").value,10)||0):0;' +
    'renderSec(st,cv);});' +
    'document.getElementById("pal").addEventListener("click",function(e){var t=e.target;' +
    'while(t&&(!t.className||t.className.indexOf("hx")<0)){t=t.parentNode;}' +
    'if(!t||t.className.indexOf("sp")>-1){return;}var d=t.getAttribute("data-c");' +
    'if(d===null){return;}sel=parseInt(d,10);mark();});' +
    'document.getElementById("reset").addEventListener("click",function(){' +
    'document.getElementById("step").value=DEF.step;document.getElementById("mm").value=Math.floor(DEF.interval/60);' +
    'renderSec(DEF.step,DEF.interval%60);sel=DEF.color;mark();});' +
    'document.getElementById("save").addEventListener("click",function(){' +
    'var mm=parseInt(document.getElementById("mm").value,10)||0;if(mm<0){mm=0;}if(mm>99){mm=99;}' +
    'var step=parseInt(document.getElementById("step").value,10)||15;' +
    'var ss=readSec(step);' +
    'var out={interval:mm*60+ss,step:step,color:sel};' +
    /* Phone webview uses pebblejs://close; the emulator passes a return_to
       query param it intercepts -- honour it so config works in both. */
    'var rt=(location.search.match(/[?&]return_to=([^&]+)/)||[])[1];' +
    'rt=rt?decodeURIComponent(rt):"pebblejs://close#";' +
    'location.href=rt+encodeURIComponent(JSON.stringify(out));});' +
    'renderSec(' + cur.step + ',' + ss + ');layoutPal();mark();' +
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

  sendSettings({ interval: interval, step: step, color: color });
});

Pebble.addEventListener('ready', function () {
  /* Push the phone's retained config to the watch on every launch. A watch app
   * update/reinstall wipes the watch's persisted settings, so without this the
   * watch would fall back to factory defaults; the phone's localStorage copy
   * survives, so this restores the user's saved preferences automatically. */
  sendSettings(loadSettings());
});
