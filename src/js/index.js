(function() {
  var html = '<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>CountUpVibe</title></head><body style="font-family:sans-serif;margin:24px;background:#081820;color:#d8ffe8;">' +
    '<h1>CountUpVibe</h1>' +
    '<p>Developer: SalmonTech</p>' +
    '<p>This build has no phone-side settings yet. Set vibe interval on the watch with UP.</p>' +
    '</body></html>';

  Pebble.addEventListener('ready', function() {
    console.log('CountUpVibe JS ready');
  });

  Pebble.addEventListener('showConfiguration', function() {
    Pebble.openURL('data:text/html,' + encodeURIComponent(html));
  });

  Pebble.addEventListener('webviewclosed', function() {
    console.log('CountUpVibe config closed');
  });
}());
