#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ===== 1. USER SETTINGS =====
const char* WIFI_SSID     = "shre._.noy";
const char* WIFI_PASSWORD = "shreyay22";
const char* TS_API_KEY    = "R0POSTQP95U9VLLK";

const bool AUTO_BASELINE = true;
int  MARGIN      = 400;
int  THRESHOLD   = 1800;
const int HYSTERESIS = 150;

const unsigned long WARMUP_MS = 180000UL;  // use 30000UL for a quick bench test
const unsigned long READ_MS   = 1000;
const unsigned long UPLOAD_MS = 20000;

const int PIN_MQ2 = 34;

// ===== 2. WEB DASHBOARD (stored in flash) =====
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gas Leak Monitor</title>
<style>
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:20px;text-align:center}
h1{font-size:1.3rem;margin:0 0 4px}
.sub{color:#94a3b8;font-size:.85rem;margin-bottom:18px}
#banner{padding:14px;border-radius:12px;font-size:1.3rem;font-weight:700;margin:0 auto 20px;max-width:520px;background:#334155}
.row{display:flex;justify-content:center;gap:28px;flex-wrap:wrap;margin-bottom:20px}
.dev{width:110px}
.led{width:70px;height:70px;border-radius:50%;margin:0 auto 8px;background:#334155;transition:.15s}
.green.on{background:#22c55e;box-shadow:0 0 28px #22c55e}
.red.on{background:#ef4444;box-shadow:0 0 28px #ef4444}
.buzz{width:70px;height:70px;border-radius:14px;margin:0 auto 8px;background:#334155;line-height:70px;font-size:2rem}
.buzz.on{background:#f59e0b;animation:p .25s infinite alternate}
@keyframes p{to{transform:scale(1.12)}}
.card{max-width:520px;margin:0 auto 14px;background:#1e293b;border-radius:12px;padding:14px}
.big{font-size:2rem;font-weight:700}
.bar{height:14px;background:#334155;border-radius:7px;position:relative;overflow:hidden;margin-top:10px}
#fill{height:100%;width:0;background:#22c55e;transition:width .3s}
#mark{position:absolute;top:0;bottom:0;width:2px;background:#fff}
button{background:#3b82f6;color:#fff;border:0;border-radius:8px;padding:10px 16px;font-size:1rem;margin:4px;cursor:pointer}
button.alt{background:#475569}
small{color:#94a3b8}
</style></head><body>
<h1>Gas &amp; Smoke Leakage Monitor</h1>
<div class="sub" id="conn">Connecting...</div>
<div id="banner">...</div>
<div class="row">
  <div class="dev"><div class="led green" id="g"></div>Green LED<br><small>Normal</small></div>
  <div class="dev"><div class="led red" id="r"></div>Red LED<br><small>Warning</small></div>
  <div class="dev"><div class="buzz" id="b">&#128276;</div>Buzzer</div>
</div>
<div class="card">
  <div class="big" id="raw">--</div>
  <small>MQ-2 reading (raw ADC counts) &middot; threshold <span id="thr">--</span></small>
  <div class="bar"><div id="fill"></div><div id="mark"></div></div>
</div>
<div>
  <button id="snd">Enable sound</button>
  <button class="alt" id="sim">Test alarm: OFF</button>
</div>
<script>
const $=id=>document.getElementById(id);
let ctx=null,gain=null,beepTimer=null,soundOn=false,simOn=false,alertNow=false,bl=false;
function startAudio(){
  ctx=new (window.AudioContext||window.webkitAudioContext)();
  const osc=ctx.createOscillator();gain=ctx.createGain();
  osc.type='square';osc.frequency.value=880;gain.gain.value=0;
  osc.connect(gain);gain.connect(ctx.destination);osc.start();
}
function setBeep(on){
  clearInterval(beepTimer);beepTimer=null;
  if(gain)gain.gain.value=0;
  if(on&&soundOn){let t=false;beepTimer=setInterval(()=>{t=!t;gain.gain.value=t?0.12:0;},250);}
}
$('snd').onclick=()=>{
  if(!ctx)startAudio();
  if(ctx.state==='suspended')ctx.resume();
  soundOn=!soundOn;
  $('snd').textContent=soundOn?'Sound: ON':'Enable sound';
  setBeep(alertNow);
};
$('sim').onclick=async()=>{
  try{await fetch('/sim?on='+(simOn?0:1));}catch(e){}
};
async function poll(){
  try{
    const d=await (await fetch('/data',{cache:'no-store'})).json();
    $('conn').textContent='Live - updated '+new Date().toLocaleTimeString();
    bl=!bl;
    simOn=d.sim;
    $('sim').textContent='Test alarm: '+(simOn?'ON':'OFF');
    $('raw').textContent=d.raw;
    $('thr').textContent=d.threshold;
    const warming=d.warming&&!d.sim;
    $('g').classList.toggle('on',!d.alert&&(!warming||bl));
    $('r').classList.toggle('on',d.alert);
    $('b').classList.toggle('on',d.alert);
    const bn=$('banner');
    if(d.alert){bn.textContent='WARNING - gas/smoke level high';bn.style.background='#991b1b';}
    else if(warming){bn.textContent='WARMING UP ('+d.left+' s left)';bn.style.background='#854d0e';}
    else{bn.textContent='NORMAL';bn.style.background='#14532d';}
    $('fill').style.width=Math.min(100,d.raw/3100*100)+'%';
    $('fill').style.background=d.alert?'#ef4444':'#22c55e';
    $('mark').style.left=Math.min(100,d.threshold/3100*100)+'%';
    if(d.alert!==alertNow){alertNow=d.alert;setBeep(alertNow);}
  }catch(e){
    $('conn').textContent='Disconnected - retrying...';
    $('banner').textContent='NO DATA';$('banner').style.background='#334155';
  }
}
setInterval(poll,1000);poll();
</script></body></html>
)rawliteral";

// ===== 3. PROGRAM =====
WebServer server(80);

bool alarmOn = false, simulate = false, warming = true, ipShown = false;
int lastRaw = 0, baseline = 0;
unsigned long lastRead = 0, lastUpload = 0, warmEnd = 0;

int readMQ2() {
  long sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogRead(PIN_MQ2); delay(2); }
  return sum / 16;
}

void connectWiFi(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(500); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWi-Fi connected"
                                               : "\nWi-Fi NOT connected (running offline)");
}

void showIP() {
  if (!ipShown && WiFi.status() == WL_CONNECTED) {
    ipShown = true;
    Serial.print("Open the dashboard at: http://");
    Serial.println(WiFi.localIP());
  }
}

void uploadToThingSpeak(int raw, bool alert) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("Upload skipped: no Wi-Fi"); return; }
  HTTPClient http;
  String url = String("http://api.thingspeak.com/update?api_key=") + TS_API_KEY +
               "&field1=" + raw + "&field2=" + (alert ? 1 : 0) + "&field3=" + THRESHOLD;
  http.begin(url);
  int code = http.GET();
  String body = http.getString();
  Serial.printf("ThingSpeak HTTP %d, entry id: %s (0 = rejected)\n", code, body.c_str());
  http.end();
}

void calibrate() {
  long sum = 0;
  for (int i = 0; i < 50; i++) { sum += readMQ2(); delay(100); server.handleClient(); }
  baseline = sum / 50;
  if (AUTO_BASELINE) THRESHOLD = baseline + MARGIN;
  if (THRESHOLD > 2900) THRESHOLD = 2900;
  Serial.printf("Baseline=%d  Threshold=%d\n", baseline, THRESHOLD);
}

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  long left = warming ? (long)(warmEnd - millis()) / 1000 : 0;
  if (left < 0) left = 0;
  String j = "{\"raw\":" + String(lastRaw) +
             ",\"threshold\":" + String(THRESHOLD) +
             ",\"alert\":" + (alarmOn ? "true" : "false") +
             ",\"warming\":" + (warming ? "true" : "false") +
             ",\"left\":" + String(left) +
             ",\"sim\":" + (simulate ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleSim() {
  if (server.hasArg("on")) simulate = (server.arg("on") == "1");
  Serial.println(simulate ? ">> TEST MODE ON (web)" : ">> TEST MODE OFF (web)");
  server.send(200, "text/plain", simulate ? "on" : "off");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2, ADC_11db);

  connectWiFi(15000);
  showIP();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/sim", handleSim);
  server.begin();

  warmEnd = millis() + WARMUP_MS;
  Serial.println("Warming up MQ-2. Keep the sensor in clean air...");
  Serial.println("Type 't' in the Serial Monitor, or use the web button, to test the alarm.");
}

void loop() {
  server.handleClient();
  showIP();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't' || c == 'T') {
      simulate = !simulate;
      Serial.println(simulate ? ">> TEST MODE ON" : ">> TEST MODE OFF");
    }
  }
  unsigned long now = millis();

  if (now - lastRead >= READ_MS) {
    lastRead = now;
    lastRaw = simulate ? THRESHOLD + 500 : readMQ2();

    if (warming && now >= warmEnd) { calibrate(); warming = false; }

    bool wasAlarm = alarmOn;
    if (warming && !simulate) alarmOn = false;
    else if (!alarmOn && lastRaw >= THRESHOLD) alarmOn = true;
    else if (alarmOn && lastRaw < THRESHOLD - HYSTERESIS) alarmOn = false;
    if (alarmOn != wasAlarm)
      Serial.println(alarmOn ? "*** ALERT: threshold exceeded ***" : "--- Alert cleared ---");

    Serial.printf("raw=%d threshold=%d state=%s%s%s\n", lastRaw, THRESHOLD,
                  alarmOn ? "WARNING" : "NORMAL",
                  warming ? " (warming up)" : "", simulate ? " (SIMULATED)" : "");
  }

  if (!warming && now - lastUpload >= UPLOAD_MS) {
    lastUpload = now;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    uploadToThingSpeak(lastRaw, alarmOn);
  }
}