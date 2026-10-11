#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPSPlus.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>

// =====================================================
// CONFIGURAZIONE HARDWARE
// =====================================================
#define GPS_RX_PIN       16
#define GPS_TX_PIN       17
#define TM1637_CLK_PIN   18
#define TM1637_DIO_PIN   19

// LED RGB catodo comune
#define RGB_R_PIN        25
#define RGB_G_PIN        26
#define RGB_B_PIN        27
#define RGB_PWM_FREQ     5000
#define RGB_PWM_RES      8

// Timezone Italia con ora legale automatica
static const char* TZ_ITALY = "CET-1CEST,M3.5.0/2,M10.5.0/3";

const char* AP_SSID = "TachimetroGPS";
const char* AP_PASS = "12345678";

IPAddress apIP(192, 168, 4, 1);
IPAddress apGW(192, 168, 4, 1);
IPAddress apSN(255, 255, 255, 0);

static const uint32_t GPS_BAUD = 9600;
static const uint32_t SAMPLE_INTERVAL_MS = 1000;
static const uint16_t HISTORY_HOURS_RAM = 24;
static const uint16_t MINUTE_POINTS_RAM = HISTORY_HOURS_RAM * 60;
static const float MIN_VALID_SPEED_KMH = 0.3f;
static const float MAX_VALID_SPEED_KMH = 300.0f;
static const uint8_t SPEED_FILTER_LEN = 5;
static const char* HISTORY_FILE = "/speed_history.csv";
static const char* SETTINGS_FILE = "/settings.json";
static const uint32_t FLASH_TRIM_INTERVAL_MS = 60000;
static const size_t FLASH_MAX_LINES = 10080;
static const int TRANSITION_MINUTES = 45;

// default profili
static const float DEFAULT_DAY_RGB_BRIGHTNESS   = 0.10f;
static const float DEFAULT_NIGHT_RGB_BRIGHTNESS = 0.02f;
static const uint8_t DEFAULT_DAY_DISPLAY_BRIGHTNESS   = 5;
static const uint8_t DEFAULT_NIGHT_DISPLAY_BRIGHTNESS = 1;

// =====================================================
// OGGETTI
// =====================================================
HardwareSerial GPSSerial(2);
TinyGPSPlus gps;
WebServer server(80);
TM1637Display display(TM1637_CLK_PIN, TM1637_DIO_PIN);

// =====================================================
// STRUTTURE DATI
// =====================================================
struct RawPoint {
  uint32_t ts;
  float kmh;
};

struct MinutePoint {
  uint32_t ts;
  float avg;
  float maxv;
  uint16_t count;
};

struct RGBColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct UserSettings {
  float dayRgbBrightness;
  float nightRgbBrightness;
  uint8_t dayDisplayBrightness;
  uint8_t nightDisplayBrightness;
};

// =====================================================
// BUFFER STORICO
// =====================================================
RawPoint* rawHistory = nullptr;
MinutePoint* minuteHistory = nullptr;

size_t rawCapacity = 0;
size_t rawHead = 0;
bool rawWrapped = false;

size_t minuteCapacity = MINUTE_POINTS_RAM;
size_t minuteHead = 0;
bool minuteWrapped = false;

// =====================================================
// STATO
// =====================================================
float currentSpeed = 0.0f;
float filteredSpeed = 0.0f;
float maxToday = 0.0f;
float avgToday = 0.0f;
uint32_t avgCount = 0;

uint8_t sats = 0;
float hdopVal = 99.9f;
bool fixValid = false;

uint32_t lastSampleMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastFlashTrimMs = 0;
uint32_t lastRgbMs = 0;
uint32_t lastProfileMs = 0;

float speedFilter[SPEED_FILTER_LEN] = {0};
uint8_t filterPos = 0;
uint8_t filterCount = 0;

uint32_t curMinuteTs = 0;
float curMinuteSum = 0.0f;
float curMinuteMax = 0.0f;
uint16_t curMinuteCount = 0;

RGBColor currentRgb = {0, 0, 255};
RGBColor targetRgb  = {0, 0, 255};

UserSettings settings = {
  DEFAULT_DAY_RGB_BRIGHTNESS,
  DEFAULT_NIGHT_RGB_BRIGHTNESS,
  DEFAULT_DAY_DISPLAY_BRIGHTNESS,
  DEFAULT_NIGHT_DISPLAY_BRIGHTNESS
};

float currentRgbBrightnessFactor = DEFAULT_DAY_RGB_BRIGHTNESS;
float targetRgbBrightnessFactor  = DEFAULT_DAY_RGB_BRIGHTNESS;
uint8_t currentDisplayBrightness = DEFAULT_DAY_DISPLAY_BRIGHTNESS;
uint8_t targetDisplayBrightness  = DEFAULT_DAY_DISPLAY_BRIGHTNESS;

bool isDayProfile = true;
int sunriseLocalMin = -1;
int sunsetLocalMin  = -1;
int currentLocalMinutes = -1;
String profileName = "giorno";
String sunriseStr = "--:--";
String sunsetStr  = "--:--";

// =====================================================
// HTML
// =====================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tachimetro GPS ESP32</title>
<style>
:root{
  --bg:#08101c; --bg2:#0f1a2f; --panel:#121c30; --panel2:#182640; --text:#edf3ff;
  --muted:#9fb2d4; --line:#243654; --blue:#58b8ff; --amber:#ffb347;
  --green:#2ecc71; --red:#ff6b6b; --chip:#10192b; --shadow:0 18px 40px rgba(0,0,0,.24);
}
*{box-sizing:border-box}
body{margin:0;font-family:Inter,Arial,Helvetica,sans-serif;background:radial-gradient(circle at top,#142342 0%,var(--bg) 35%,var(--bg2) 100%);color:var(--text)}
.wrap{max-width:1320px;margin:0 auto;padding:18px}
.top{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:18px}
.title{font-size:clamp(26px,4vw,40px);font-weight:800;letter-spacing:-.03em}
.sub{color:var(--muted);font-size:14px;margin-top:4px}
.pill{display:inline-flex;align-items:center;gap:8px;padding:9px 14px;border-radius:999px;font-weight:700;font-size:12px;border:1px solid rgba(255,255,255,.06);background:rgba(255,255,255,.04)}
.dot{width:9px;height:9px;border-radius:50%}
.ok .dot{background:var(--green);box-shadow:0 0 12px rgba(46,204,113,.65)}
.bad .dot{background:var(--red);box-shadow:0 0 12px rgba(255,107,107,.55)}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
.card{background:linear-gradient(180deg,var(--panel),var(--panel2));border:1px solid var(--line);border-radius:22px;padding:16px;box-shadow:var(--shadow)}
.hero{grid-column:span 12;display:grid;grid-template-columns:2.1fr 1fr;gap:14px}
.speedbox{min-height:190px;display:flex;flex-direction:column;justify-content:center}
.speed{font-size:clamp(58px,12vw,122px);font-weight:900;line-height:.92;letter-spacing:-.05em;background:linear-gradient(180deg,#fff,#a8d9ff);-webkit-background-clip:text;background-clip:text;color:transparent}
.unit{font-size:22px;color:var(--muted);margin-top:10px}
.meta{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
.mini{background:rgba(255,255,255,.03);border:1px solid rgba(255,255,255,.05);border-radius:16px;padding:13px}
.k{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.09em;margin-bottom:6px}
.v{font-size:26px;font-weight:800;letter-spacing:-.03em}
.status{grid-column:span 12;display:grid;grid-template-columns:repeat(6,1fr);gap:14px}
.chartCard{grid-column:span 7}
.sidePanel{grid-column:span 5;display:grid;gap:14px}
.toolbar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:12px}
button{background:linear-gradient(180deg,#1a2e52,#163356);color:var(--text);border:1px solid #29446d;border-radius:14px;padding:10px 14px;font-weight:700;cursor:pointer}
button:hover{filter:brightness(1.08)}
.ghost{background:transparent}
canvas{width:100%;height:360px;background:#0a1322;border-radius:16px;border:1px solid #22314c}
.footer{margin-top:10px;color:var(--muted);font-size:13px}
.list{display:grid;gap:8px;max-height:280px;overflow:auto;padding-right:4px}
.row{display:grid;grid-template-columns:80px 1fr 1fr;gap:8px;padding:10px 12px;border-radius:14px;background:rgba(255,255,255,.03);border:1px solid rgba(255,255,255,.05);font-size:13px}
.row b{font-size:14px}
.tag{display:inline-block;padding:6px 9px;border-radius:999px;background:var(--chip);border:1px solid rgba(255,255,255,.06);font-size:12px;color:var(--muted)}
.split{display:flex;justify-content:space-between;align-items:center;gap:10px}
.swatch{width:28px;height:28px;border-radius:10px;border:1px solid rgba(255,255,255,.12);box-shadow:inset 0 0 0 1px rgba(255,255,255,.05)}
.formgrid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
label{display:block;font-size:13px;color:var(--muted);margin-bottom:6px}
input[type=range]{width:100%}
.small{font-size:12px;color:var(--muted)}
@media (max-width:980px){.hero{grid-template-columns:1fr}.status{grid-template-columns:repeat(2,1fr)}.chartCard,.sidePanel{grid-column:span 12}.formgrid{grid-template-columns:1fr}}
@media (max-width:640px){.status{grid-template-columns:1fr}canvas{height:280px}.row{grid-template-columns:72px 1fr 1fr}}
</style>
</head>
<body>
<div class="wrap">
  <div class="top">
    <div>
      <div class="title">Tachimetro GPS ESP32</div>
      <div class="sub">Profili giorno/notte automatici da GPS, storico persistente e LED RGB gradiente</div>
    </div>
    <div id="fixPill" class="pill bad"><span class="dot"></span><span>GPS non fixato</span></div>
  </div>

  <div class="grid">
    <div class="card hero">
      <div class="speedbox">
        <div class="speed" id="speed">0.0</div>
        <div class="unit">km/h</div>
      </div>
      <div class="meta">
        <div class="mini"><div class="k">Satelliti</div><div class="v" id="sats">0</div></div>
        <div class="mini"><div class="k">HDOP</div><div class="v" id="hdop">--</div></div>
        <div class="mini"><div class="k">Massima oggi</div><div class="v" id="max">0.0</div></div>
        <div class="mini"><div class="k">Media oggi</div><div class="v" id="avg">0.0</div></div>
      </div>
    </div>

    <div class="card status">
      <div class="mini"><div class="k">Ora locale</div><div class="v" id="gpsTime">--:--:--</div></div>
      <div class="mini"><div class="k">Profilo</div><div class="v" id="profile">--</div></div>
      <div class="mini"><div class="k">Alba</div><div class="v" id="sunrise">--:--</div></div>
      <div class="mini"><div class="k">Tramonto</div><div class="v" id="sunset">--:--</div></div>
      <div class="mini"><div class="k">Display</div><div class="v" id="dispBri">0</div></div>
      <div class="mini"><div class="k">LED RGB</div><div class="split"><div class="v" id="rgbText">0,0,255</div><div class="swatch" id="rgbSwatch"></div></div></div>
    </div>

    <div class="card chartCard">
      <div class="split" style="margin-bottom:10px">
        <div style="font-weight:800">Velocità media e picco per minuto</div>
        <div class="toolbar">
          <span class="tag">24h RAM + storico flash</span>
          <button onclick="loadHistory()">Aggiorna</button>
          <button class="ghost" onclick="downloadCSV()">Scarica CSV</button>
        </div>
      </div>
      <canvas id="chart" width="1100" height="360"></canvas>
      <div class="footer">LED RGB: blu 0 · verde 50 · giallo 90 · rosso 130+ km/h. Profili luminosità automatici con alba/tramonto.</div>
    </div>

    <div class="sidePanel">
      <div class="card">
        <div class="split" style="margin-bottom:10px">
          <div style="font-weight:800">Impostazioni giorno / notte</div>
          <button onclick="saveSettings()">Salva</button>
        </div>
        <div class="formgrid">
          <div>
            <label>Luminosità LED giorno <span id="dayLedVal"></span></label>
            <input id="dayLed" type="range" min="0.01" max="0.25" step="0.01">
          </div>
          <div>
            <label>Luminosità display giorno <span id="dayDispVal"></span></label>
            <input id="dayDisp" type="range" min="0" max="7" step="1">
          </div>
          <div>
            <label>Luminosità LED notte <span id="nightLedVal"></span></label>
            <input id="nightLed" type="range" min="0.01" max="0.25" step="0.01">
          </div>
          <div>
            <label>Luminosità display notte <span id="nightDispVal"></span></label>
            <input id="nightDisp" type="range" min="0" max="7" step="1">
          </div>
        </div>
        <div class="small" style="margin-top:10px">Le impostazioni sono memorizzate su flash. Il profilo attivo cambia automaticamente in base ad alba e tramonto calcolati localmente dal GPS.</div>
      </div>

      <div class="card">
        <div class="split" style="margin-bottom:10px">
          <div style="font-weight:800">Storico recente</div>
          <button class="ghost" onclick="clearHistory()">Azzera storico</button>
        </div>
        <div id="historyList" class="list"></div>
      </div>
    </div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
const cvs = $('chart');
const ctx = cvs.getContext('2d');
let series = [];

function fmt1(v){ return Number(v || 0).toFixed(1); }
function fmt2(v){ return Number(v || 0).toFixed(2); }
function fmtTime(ts){
  if(!ts) return '--:--';
  const d = new Date(ts * 1000);
  return d.toLocaleTimeString('it-IT', {hour:'2-digit', minute:'2-digit'});
}
function updateLabels(){
  $('dayLedVal').textContent = fmt2($('dayLed').value);
  $('nightLedVal').textContent = fmt2($('nightLed').value);
  $('dayDispVal').textContent = $('dayDisp').value;
  $('nightDispVal').textContent = $('nightDisp').value;
}

function drawChart(data){
  const W = cvs.width, H = cvs.height;
  ctx.clearRect(0,0,W,H);
  ctx.fillStyle = '#0a1322';
  ctx.fillRect(0,0,W,H);
  const left = 58, right = 18, top = 20, bottom = 36;
  const pw = W-left-right, ph = H-top-bottom;
  let maxV = 10;
  for(const p of data){ maxV = Math.max(maxV, p.avg || 0, p.max || 0); }
  maxV = Math.max(20, Math.ceil(maxV / 10) * 10);
  ctx.font = '12px Arial';
  for(let i=0;i<=5;i++){
    const y = top + (ph/5)*i;
    ctx.strokeStyle = i===5 ? '#415574' : '#25334f';
    ctx.beginPath(); ctx.moveTo(left,y); ctx.lineTo(left+pw,y); ctx.stroke();
    ctx.fillStyle = '#9db0d1';
    ctx.fillText(String((maxV - (maxV/5)*i).toFixed(0)), 12, y+4);
  }
  const n = data.length;
  if(n < 2){ ctx.fillStyle='#9db0d1'; ctx.fillText('Storico insufficiente', left+20, top+30); return; }
  const xOf = i => left + (i/(n-1))*pw;
  const yOf = v => top + ph - (Math.min(v,maxV)/maxV)*ph;
  const steps = Math.min(6, n-1);
  for(let i=0;i<=steps;i++){
    const idx = Math.round((n-1)*(i/steps));
    const x = xOf(idx);
    ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, top+ph); ctx.strokeStyle='#18253d'; ctx.stroke();
    ctx.fillStyle='#9db0d1'; ctx.fillText(fmtTime(data[idx].ts), x-18, H-10);
  }
  ctx.beginPath(); ctx.strokeStyle = '#ffb347'; ctx.lineWidth = 2;
  data.forEach((p,i)=>{ const x=xOf(i), y=yOf(p.max||0); if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y); }); ctx.stroke();
  ctx.beginPath(); ctx.strokeStyle = '#58b8ff'; ctx.lineWidth = 2.4;
  data.forEach((p,i)=>{ const x=xOf(i), y=yOf(p.avg||0); if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y); }); ctx.stroke();
}

function drawList(data){
  const el = $('historyList');
  const last = data.slice(-18).reverse();
  if(!last.length){ el.innerHTML = '<div class="row"><span>--</span><b>Nessun dato</b><span>Attendi il fix GPS</span></div>'; return; }
  el.innerHTML = last.map(p => `<div class="row"><span>${fmtTime(p.ts)}</span><b>${fmt1(p.avg)} km/h</b><span>picco ${fmt1(p.max)}</span></div>`).join('');
}

async function loadLive(){
  const r = await fetch('/api/live');
  const j = await r.json();
  $('speed').textContent = fmt1(j.speed_kmh);
  $('sats').textContent = j.sats;
  $('hdop').textContent = fmt1(j.hdop);
  $('max').textContent = fmt1(j.max_today_kmh);
  $('avg').textContent = fmt1(j.avg_today_kmh);
  $('gpsTime').textContent = j.local_time || '--:--:--';
  $('profile').textContent = j.profile || '--';
  $('sunrise').textContent = j.sunrise || '--:--';
  $('sunset').textContent = j.sunset || '--:--';
  $('dispBri').textContent = j.display_brightness;
  $('rgbText').textContent = `${j.rgb.r},${j.rgb.g},${j.rgb.b}`;
  $('rgbSwatch').style.background = `rgb(${j.rgb.r},${j.rgb.g},${j.rgb.b})`;
  const pill = $('fixPill');
  if(j.fix){ pill.className='pill ok'; pill.innerHTML='<span class="dot"></span><span>GPS fix valido</span>'; }
  else { pill.className='pill bad'; pill.innerHTML='<span class="dot"></span><span>GPS non fixato</span>'; }
}

async function loadHistory(){
  const r = await fetch('/api/history');
  const j = await r.json();
  series = j.points || [];
  drawChart(series);
  drawList(series);
}

async function loadSettings(){
  const r = await fetch('/api/settings');
  const j = await r.json();
  $('dayLed').value = Number(j.day_rgb_brightness).toFixed(2);
  $('nightLed').value = Number(j.night_rgb_brightness).toFixed(2);
  $('dayDisp').value = j.day_display_brightness;
  $('nightDisp').value = j.night_display_brightness;
  updateLabels();
}

async function saveSettings(){
  const payload = {
    day_rgb_brightness: Number($('dayLed').value),
    night_rgb_brightness: Number($('nightLed').value),
    day_display_brightness: Number($('dayDisp').value),
    night_display_brightness: Number($('nightDisp').value)
  };
  const r = await fetch('/api/settings', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(payload)
  });
  const j = await r.json();
  if(!j.ok) alert('Errore salvataggio');
  await loadLive();
}

function downloadCSV(){ window.location = '/api/history.csv'; }

async function clearHistory(){
  if(!confirm('Vuoi cancellare lo storico salvato in RAM e flash?')) return;
  await fetch('/api/history/clear', {method:'POST'});
  await loadHistory();
}

['dayLed','nightLed','dayDisp','nightDisp'].forEach(id=>$(id).addEventListener('input', updateLabels));

async function refreshAll(){ try{ await loadLive(); }catch(e){} try{ await loadHistory(); }catch(e){} try{ await loadSettings(); }catch(e){} }
async function tick(){ try{ await loadLive(); }catch(e){} }
refreshAll();
setInterval(tick, 1000);
setInterval(loadHistory, 15000);
window.addEventListener('resize', ()=>drawChart(series));
</script>
</body>
</html>
)rawliteral";

// =====================================================
// UTILITY
// =====================================================
bool hasPSRAM() {
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM_SUPPORT)
  return psramFound();
#else
  return false;
#endif
}

void* allocMemory(size_t bytes) {
  if (hasPSRAM()) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

String twoDigits(int v) {
  return (v < 10) ? "0" + String(v) : String(v);
}

String formatHM(int totalMin) {
  if (totalMin < 0) return "--:--";
  totalMin %= 1440;
  if (totalMin < 0) totalMin += 1440;
  return twoDigits(totalMin / 60) + ":" + twoDigits(totalMin % 60);
}

float clampf(float v, float minV, float maxV) {
  if (v < minV) return minV;
  if (v > maxV) return maxV;
  return v;
}

uint8_t clampDisplay(int v) {
  if (v < 0) return 0;
  if (v > 7) return 7;
  return (uint8_t)v;
}

void setupItalyTimeZone() {
  setenv("TZ", TZ_ITALY, 1);
  tzset();
}

uint32_t makeEpochUTC() {
  if (!gps.date.isValid() || !gps.time.isValid()) return 0;

  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&t);

  setenv("TZ", TZ_ITALY, 1);
  tzset();

  return (epoch > 0) ? (uint32_t)epoch : 0;
}

String localTimeString() {
  uint32_t epoch = makeEpochUTC();
  if (epoch == 0) return "--:--:--";

  time_t raw = (time_t)epoch;
  struct tm localTm;
  localtime_r(&raw, &localTm);

  return twoDigits(localTm.tm_hour) + ":" +
         twoDigits(localTm.tm_min) + ":" +
         twoDigits(localTm.tm_sec);
}

int dayOfYearFromDate(int year, int month, int day) {
  static const int daysBeforeMonth[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  int doy = daysBeforeMonth[month - 1] + day;
  bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
  if (leap && month > 2) doy++;
  return doy;
}

// =====================================================
// CALCOLO ALBA/TRAMONTO LOCALE SENZA LIBRERIA ESTERNA
// Formula NOAA semplificata
// restituisce minuti UTC da mezzanotte o -1 se non valido
// =====================================================
int calcSunUtcMinutes(bool sunrise, int year, int month, int day, double latitude, double longitude) {
  const double zenith = 90.833; // alba/tramonto civile standard
  const double D2R = M_PI / 180.0;
  const double R2D = 180.0 / M_PI;

  int N = dayOfYearFromDate(year, month, day);
  double lngHour = longitude / 15.0;
  double t = N + ((sunrise ? 6.0 : 18.0) - lngHour) / 24.0;

  double M = (0.9856 * t) - 3.289;
  double L = M + (1.916 * sin(M * D2R)) + (0.020 * sin(2 * M * D2R)) + 282.634;
  while (L < 0) L += 360.0;
  while (L >= 360.0) L -= 360.0;

  double RA = atan(0.91764 * tan(L * D2R)) * R2D;
  while (RA < 0) RA += 360.0;
  while (RA >= 360.0) RA -= 360.0;

  double Lquadrant  = floor(L / 90.0) * 90.0;
  double RAquadrant = floor(RA / 90.0) * 90.0;
  RA = RA + (Lquadrant - RAquadrant);
  RA /= 15.0;

  double sinDec = 0.39782 * sin(L * D2R);
  double cosDec = cos(asin(sinDec));

  double cosH = (cos(zenith * D2R) - (sinDec * sin(latitude * D2R))) /
                (cosDec * cos(latitude * D2R));

  if (cosH > 1.0 || cosH < -1.0) return -1;

  double H = sunrise ? (360.0 - acos(cosH) * R2D) : (acos(cosH) * R2D);
  H /= 15.0;

  double T = H + RA - (0.06571 * t) - 6.622;
  double UT = T - lngHour;

  while (UT < 0) UT += 24.0;
  while (UT >= 24.0) UT -= 24.0;

  return (int)round(UT * 60.0);
}

float applySpeedFilter(float v) {
  speedFilter[filterPos] = v;
  filterPos = (filterPos + 1) % SPEED_FILTER_LEN;
  if (filterCount < SPEED_FILTER_LEN) filterCount++;
  float sum = 0.0f;
  for (uint8_t i = 0; i < filterCount; i++) sum += speedFilter[i];
  return sum / filterCount;
}

// =====================================================
// SETTINGS
// =====================================================
void saveSettingsToFlash() {
  JsonDocument doc;
  doc["day_rgb_brightness"] = settings.dayRgbBrightness;
  doc["night_rgb_brightness"] = settings.nightRgbBrightness;
  doc["day_display_brightness"] = settings.dayDisplayBrightness;
  doc["night_display_brightness"] = settings.nightDisplayBrightness;

  File f = LittleFS.open(SETTINGS_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void loadSettingsFromFlash() {
  if (!LittleFS.exists(SETTINGS_FILE)) {
    saveSettingsToFlash();
    return;
  }

  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) return;

  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    settings.dayRgbBrightness = clampf(doc["day_rgb_brightness"] | DEFAULT_DAY_RGB_BRIGHTNESS, 0.01f, 0.25f);
    settings.nightRgbBrightness = clampf(doc["night_rgb_brightness"] | DEFAULT_NIGHT_RGB_BRIGHTNESS, 0.01f, 0.25f);
    settings.dayDisplayBrightness = clampDisplay(doc["day_display_brightness"] | DEFAULT_DAY_DISPLAY_BRIGHTNESS);
    settings.nightDisplayBrightness = clampDisplay(doc["night_display_brightness"] | DEFAULT_NIGHT_DISPLAY_BRIGHTNESS);
  }
  f.close();
}

// =====================================================
// RGB LED
// =====================================================
void setupRgb() {
  ledcAttach(RGB_R_PIN, RGB_PWM_FREQ, RGB_PWM_RES);
  ledcAttach(RGB_G_PIN, RGB_PWM_FREQ, RGB_PWM_RES);
  ledcAttach(RGB_B_PIN, RGB_PWM_FREQ, RGB_PWM_RES);
}

uint8_t applyRgbBrightness(uint8_t v) {
  float scaled = v * currentRgbBrightnessFactor;
  if (scaled < 0.0f) scaled = 0.0f;
  if (scaled > 255.0f) scaled = 255.0f;
  return (uint8_t)roundf(scaled);
}

uint8_t mix8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)roundf((1.0f - t) * a + t * b);
}

RGBColor lerpColor(RGBColor a, RGBColor b, float t) {
  t = clampf(t, 0.0f, 1.0f);
  RGBColor c;
  c.r = mix8(a.r, b.r, t);
  c.g = mix8(a.g, b.g, t);
  c.b = mix8(a.b, b.b, t);
  return c;
}

RGBColor speedToColor(float kmh) {
  const RGBColor C0 = {0,   0,   255}; // 0 km/h
  const RGBColor C1 = {0,   255, 0  }; // 50 km/h
  const RGBColor C2 = {255, 255, 0  }; // 90 km/h
  const RGBColor C3 = {255, 0,   0  }; // 130+ km/h

  if (kmh <= 0.0f) return C0;
  if (kmh < 50.0f)  return lerpColor(C0, C1, kmh / 50.0f);
  if (kmh < 90.0f)  return lerpColor(C1, C2, (kmh - 50.0f) / 40.0f);
  if (kmh < 130.0f) return lerpColor(C2, C3, (kmh - 90.0f) / 40.0f);
  return C3;
}

void writeRgbCommonCathode(RGBColor c) {
  ledcWrite(RGB_R_PIN, applyRgbBrightness(c.r));
  ledcWrite(RGB_G_PIN, applyRgbBrightness(c.g));
  ledcWrite(RGB_B_PIN, applyRgbBrightness(c.b));
}

void updateRgbLed(float kmh, bool valid) {
  targetRgb = valid ? speedToColor(kmh) : RGBColor{0, 0, 255};
  const uint8_t step = 4;

  if (currentRgb.r < targetRgb.r) currentRgb.r = min((int)targetRgb.r, (int)currentRgb.r + step);
  else if (currentRgb.r > targetRgb.r) currentRgb.r = max((int)targetRgb.r, (int)currentRgb.r - step);

  if (currentRgb.g < targetRgb.g) currentRgb.g = min((int)targetRgb.g, (int)currentRgb.g + step);
  else if (currentRgb.g > targetRgb.g) currentRgb.g = max((int)targetRgb.g, (int)currentRgb.g - step);

  if (currentRgb.b < targetRgb.b) currentRgb.b = min((int)targetRgb.b, (int)currentRgb.b + step);
  else if (currentRgb.b > targetRgb.b) currentRgb.b = max((int)targetRgb.b, (int)currentRgb.b - step);

  writeRgbCommonCathode(currentRgb);
}

// =====================================================
// DISPLAY TM1637
// =====================================================
void showSpeed(float kmh, bool valid) {
  if (!valid) {
    const uint8_t segs[] = {0x40, 0x40, 0x40, 0x40};
    display.setSegments(segs);
    return;
  }

  if (kmh < 0.0f) kmh = 0.0f;
  if (kmh > 999.9f) kmh = 999.9f;

  uint16_t v10 = (uint16_t)roundf(kmh * 10.0f);
  uint8_t d0 = (v10 / 1000) % 10;
  uint8_t d1 = (v10 / 100) % 10;
  uint8_t d2 = (v10 / 10) % 10;
  uint8_t d3 = v10 % 10;
  uint8_t segs[4] = {0,0,0,0};

  if (v10 >= 1000) {
    segs[0] = display.encodeDigit(d0);
    segs[1] = display.encodeDigit(d1);
    segs[2] = display.encodeDigit(d2) | 0x80;
    segs[3] = display.encodeDigit(d3);
  } else if (v10 >= 100) {
    segs[0] = 0x00;
    segs[1] = display.encodeDigit(d1);
    segs[2] = display.encodeDigit(d2) | 0x80;
    segs[3] = display.encodeDigit(d3);
  } else if (v10 >= 10) {
    segs[0] = 0x00;
    segs[1] = 0x00;
    segs[2] = display.encodeDigit(d2) | 0x80;
    segs[3] = display.encodeDigit(d3);
  } else {
    segs[0] = 0x00;
    segs[1] = 0x00;
    segs[2] = display.encodeDigit(0) | 0x80;
    segs[3] = display.encodeDigit(d3);
  }

  display.setSegments(segs);
}

// =====================================================
// STORICO
// =====================================================
size_t rawCount() { return rawWrapped ? rawCapacity : rawHead; }
size_t minuteCount() { return minuteWrapped ? minuteCapacity : minuteHead; }

void pushRaw(uint32_t ts, float kmh) {
  if (!rawHistory || rawCapacity == 0) return;
  rawHistory[rawHead] = {ts, kmh};
  rawHead = (rawHead + 1) % rawCapacity;
  if (rawHead == 0) rawWrapped = true;
}

void pushMinute(uint32_t ts, float avg, float maxv, uint16_t count) {
  if (!minuteHistory || minuteCapacity == 0) return;
  minuteHistory[minuteHead] = {ts, avg, maxv, count};
  minuteHead = (minuteHead + 1) % minuteCapacity;
  if (minuteHead == 0) minuteWrapped = true;
}

size_t countFlashLines() {
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (!f) return 0;
  size_t lines = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() > 3) lines++;
  }
  f.close();
  return lines;
}

void appendMinuteToFlash(uint32_t ts, float avg, float maxv, uint16_t count) {
  File f = LittleFS.open(HISTORY_FILE, FILE_APPEND);
  if (!f) return;
  f.print(ts); f.print(',');
  f.print(avg, 2); f.print(',');
  f.print(maxv, 2); f.print(',');
  f.println(count);
  f.close();
}

void trimHistoryFileIfNeeded() {
  size_t lines = countFlashLines();
  if (lines <= FLASH_MAX_LINES) return;

  File in = LittleFS.open(HISTORY_FILE, "r");
  File out = LittleFS.open("/history_tmp.csv", "w");
  if (!in || !out) {
    if (in) in.close();
    if (out) out.close();
    return;
  }

  size_t skip = lines - FLASH_MAX_LINES;
  size_t idx = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (line.length() <= 3) continue;
    if (idx++ >= skip) out.println(line);
  }

  in.close();
  out.close();
  LittleFS.remove(HISTORY_FILE);
  LittleFS.rename("/history_tmp.csv", HISTORY_FILE);
}

void clearAllHistory() {
  rawHead = 0; rawWrapped = false;
  minuteHead = 0; minuteWrapped = false;
  curMinuteTs = 0; curMinuteSum = 0; curMinuteMax = 0; curMinuteCount = 0;

  if (rawHistory) memset(rawHistory, 0, rawCapacity * sizeof(RawPoint));
  if (minuteHistory) memset(minuteHistory, 0, minuteCapacity * sizeof(MinutePoint));

  LittleFS.remove(HISTORY_FILE);
}

void loadHistoryFromFlash() {
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() < 5) continue;

    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;

    uint32_t ts = (uint32_t) line.substring(0, p1).toInt();
    float avg = line.substring(p1 + 1, p2).toFloat();
    float maxv = line.substring(p2 + 1, p3).toFloat();
    uint16_t count = (uint16_t) line.substring(p3 + 1).toInt();
    pushMinute(ts, avg, maxv, count);
  }
  f.close();
}

void flushMinuteIfNeeded(uint32_t ts) {
  if (ts == 0) return;
  uint32_t minuteTs = ts - (ts % 60);

  if (curMinuteTs == 0) {
    curMinuteTs = minuteTs;
    return;
  }

  if (minuteTs != curMinuteTs) {
    if (curMinuteCount > 0) {
      float avg = curMinuteSum / curMinuteCount;
      pushMinute(curMinuteTs, avg, curMinuteMax, curMinuteCount);
      appendMinuteToFlash(curMinuteTs, avg, curMinuteMax, curMinuteCount);
    }
    curMinuteTs = minuteTs;
    curMinuteSum = 0.0f;
    curMinuteMax = 0.0f;
    curMinuteCount = 0;
  }
}

void addMinuteSample(uint32_t ts, float kmh) {
  flushMinuteIfNeeded(ts);
  curMinuteSum += kmh;
  if (kmh > curMinuteMax) curMinuteMax = kmh;
  curMinuteCount++;
}

// =====================================================
// PROFILO GIORNO / NOTTE
// =====================================================
float profileBlendWeight() {
  if (currentLocalMinutes < 0 || sunriseLocalMin < 0 || sunsetLocalMin < 0) {
    isDayProfile = true;
    profileName = "giorno";
    return 1.0f;
  }

  int now = currentLocalMinutes;
  int sr = sunriseLocalMin;
  int ss = sunsetLocalMin;
  int trans = TRANSITION_MINUTES;

  if (now < sr - trans || now >= ss + trans) {
    isDayProfile = false;
    profileName = "notte";
    return 0.0f;
  }
  if (now >= sr + trans && now < ss - trans) {
    isDayProfile = true;
    profileName = "giorno";
    return 1.0f;
  }
  if (now >= sr - trans && now < sr + trans) {
    float t = (float)(now - (sr - trans)) / (2.0f * trans);
    profileName = "alba";
    isDayProfile = true;
    return clampf(t, 0.0f, 1.0f);
  }
  if (now >= ss - trans && now < ss + trans) {
    float t = 1.0f - (float)(now - (ss - trans)) / (2.0f * trans);
    profileName = "tramonto";
    isDayProfile = t >= 0.5f;
    return clampf(t, 0.0f, 1.0f);
  }

  isDayProfile = true;
  profileName = "giorno";
  return 1.0f;
}

void updateSunTimes() {
  if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) {
    sunriseLocalMin = -1;
    sunsetLocalMin = -1;
    sunriseStr = "--:--";
    sunsetStr = "--:--";
    currentLocalMinutes = -1;
    return;
  }

  int year = gps.date.year();
  int month = gps.date.month();
  int day = gps.date.day();

  uint32_t epoch = makeEpochUTC();
  if (epoch == 0) {
    sunriseLocalMin = -1;
    sunsetLocalMin = -1;
    sunriseStr = "--:--";
    sunsetStr = "--:--";
    currentLocalMinutes = -1;
    return;
  }

  time_t raw = (time_t)epoch;
  struct tm localTm;
  localtime_r(&raw, &localTm);

  currentLocalMinutes = localTm.tm_hour * 60 + localTm.tm_min;

  int utcSunriseMin = calcSunUtcMinutes(true, year, month, day, gps.location.lat(), gps.location.lng());
  int utcSunsetMin  = calcSunUtcMinutes(false, year, month, day, gps.location.lat(), gps.location.lng());

  if (utcSunriseMin < 0 || utcSunsetMin < 0) {
    sunriseLocalMin = -1;
    sunsetLocalMin = -1;
    sunriseStr = "--:--";
    sunsetStr = "--:--";
  } else {
    long gmtoffSeconds = localTm.tm_isdst > 0 ? 7200 : 3600;
    int offsetMin = (int)(gmtoffSeconds / 60);

    sunriseLocalMin = (utcSunriseMin + offsetMin) % 1440;
    sunsetLocalMin  = (utcSunsetMin + offsetMin) % 1440;
    if (sunriseLocalMin < 0) sunriseLocalMin += 1440;
    if (sunsetLocalMin < 0) sunsetLocalMin += 1440;

    sunriseStr = formatHM(sunriseLocalMin);
    sunsetStr  = formatHM(sunsetLocalMin);
  }

  float w = profileBlendWeight();
  targetRgbBrightnessFactor =
      settings.nightRgbBrightness +
      (settings.dayRgbBrightness - settings.nightRgbBrightness) * w;

  float dispF =
      settings.nightDisplayBrightness +
      (settings.dayDisplayBrightness - settings.nightDisplayBrightness) * w;

  targetDisplayBrightness = clampDisplay((int)roundf(dispF));
}

void applyProfileOutputs() {
  const float step = 0.002f;
  if (currentRgbBrightnessFactor < targetRgbBrightnessFactor) {
    currentRgbBrightnessFactor = min(targetRgbBrightnessFactor, currentRgbBrightnessFactor + step);
  } else if (currentRgbBrightnessFactor > targetRgbBrightnessFactor) {
    currentRgbBrightnessFactor = max(targetRgbBrightnessFactor, currentRgbBrightnessFactor - step);
  }

  if (currentDisplayBrightness != targetDisplayBrightness) {
    currentDisplayBrightness = targetDisplayBrightness;
    display.setBrightness(currentDisplayBrightness, true);
  }
}

// =====================================================
// GPS
// =====================================================
void updateGPS() {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  hdopVal = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
  fixValid = gps.location.isValid() && gps.speed.isValid() && sats >= 4;

  if (gps.speed.isUpdated()) {
    float kmh = gps.speed.kmph();
    if (kmh < MIN_VALID_SPEED_KMH) kmh = 0.0f;
    if (kmh > MAX_VALID_SPEED_KMH) kmh = currentSpeed;

    currentSpeed = kmh;
    filteredSpeed = applySpeedFilter(kmh);

    if (filteredSpeed > maxToday) maxToday = filteredSpeed;
    avgToday = ((avgToday * avgCount) + filteredSpeed) / (avgCount + 1);
    avgCount++;
  }
}

// =====================================================
// WEB API
// =====================================================
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleLive() {
  JsonDocument doc;
  doc["fix"] = fixValid;
  doc["speed_kmh"] = filteredSpeed;
  doc["sats"] = sats;
  doc["hdop"] = hdopVal;
  doc["max_today_kmh"] = maxToday;
  doc["avg_today_kmh"] = avgToday;
  doc["local_time"] = localTimeString();
  doc["profile"] = profileName;
  doc["sunrise"] = sunriseStr;
  doc["sunset"] = sunsetStr;
  doc["display_brightness"] = currentDisplayBrightness;

  JsonObject rgb = doc["rgb"].to<JsonObject>();
  rgb["r"] = currentRgb.r;
  rgb["g"] = currentRgb.g;
  rgb["b"] = currentRgb.b;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleHistory() {
  JsonDocument doc;
  JsonArray arr = doc["points"].to<JsonArray>();

  size_t count = minuteCount();
  size_t start = minuteWrapped ? minuteHead : 0;
  for (size_t i = 0; i < count; i++) {
    size_t idx = (start + i) % minuteCapacity;
    JsonObject p = arr.add<JsonObject>();
    p["ts"] = minuteHistory[idx].ts;
    p["avg"] = minuteHistory[idx].avg;
    p["max"] = minuteHistory[idx].maxv;
    p["n"] = minuteHistory[idx].count;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleHistoryCSV() {
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "Storico non disponibile");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("timestamp,avg_kmh,max_kmh,count\n");

  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() > 3) server.sendContent(line + "\n");
  }

  f.close();
  server.sendContent("");
}

void handleHistoryClear() {
  clearAllHistory();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSettingsGet() {
  JsonDocument doc;
  doc["day_rgb_brightness"] = settings.dayRgbBrightness;
  doc["night_rgb_brightness"] = settings.nightRgbBrightness;
  doc["day_display_brightness"] = settings.dayDisplayBrightness;
  doc["night_display_brightness"] = settings.nightDisplayBrightness;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSettingsPost() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"body mancante\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"json non valido\"}");
    return;
  }

  settings.dayRgbBrightness = clampf(doc["day_rgb_brightness"] | settings.dayRgbBrightness, 0.01f, 0.25f);
  settings.nightRgbBrightness = clampf(doc["night_rgb_brightness"] | settings.nightRgbBrightness, 0.01f, 0.25f);
  settings.dayDisplayBrightness = clampDisplay(doc["day_display_brightness"] | settings.dayDisplayBrightness);
  settings.nightDisplayBrightness = clampDisplay(doc["night_display_brightness"] | settings.nightDisplayBrightness);

  saveSettingsToFlash();
  updateSunTimes();
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/api/live", handleLive);
  server.on("/api/history", handleHistory);
  server.on("/api/history.csv", handleHistoryCSV);
  server.on("/api/history/clear", HTTP_POST, handleHistoryClear);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.begin();
}

// =====================================================
// WIFI AP
// =====================================================
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGW, apSN);
  WiFi.softAP(AP_SSID, AP_PASS);
}

// =====================================================
// MEMORIA
// =====================================================
void setupHistoryBuffers() {
  rawCapacity = HISTORY_HOURS_RAM * 3600UL;
  rawHistory = (RawPoint*)allocMemory(rawCapacity * sizeof(RawPoint));
  minuteHistory = (MinutePoint*)allocMemory(minuteCapacity * sizeof(MinutePoint));

  if (!rawHistory) {
    rawCapacity = 6 * 3600UL;
    rawHistory = (RawPoint*)allocMemory(rawCapacity * sizeof(RawPoint));
  }

  if (!minuteHistory) {
    minuteCapacity = 360;
    minuteHistory = (MinutePoint*)allocMemory(minuteCapacity * sizeof(MinutePoint));
  }

  if (rawHistory) memset(rawHistory, 0, rawCapacity * sizeof(RawPoint));
  if (minuteHistory) memset(minuteHistory, 0, minuteCapacity * sizeof(MinutePoint));
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  setupItalyTimeZone();

  if (!LittleFS.begin(true)) {
    Serial.println("Errore LittleFS");
  }

  loadSettingsFromFlash();
  currentRgbBrightnessFactor = settings.dayRgbBrightness;
  targetRgbBrightnessFactor  = settings.dayRgbBrightness;
  currentDisplayBrightness   = settings.dayDisplayBrightness;
  targetDisplayBrightness    = settings.dayDisplayBrightness;

  display.setBrightness(currentDisplayBrightness, true);
  showSpeed(0.0f, false);

  setupRgb();
  writeRgbCommonCathode(currentRgb);

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  setupHistoryBuffers();
  loadHistoryFromFlash();
  trimHistoryFileIfNeeded();
  setupAP();
  setupWeb();

  Serial.println();
  Serial.println("=== TACHIMETRO GPS ESP32 DAY/NIGHT ===");
  Serial.print("SSID AP: "); Serial.println(AP_SSID);
  Serial.print("PASS AP: "); Serial.println(AP_PASS);
  Serial.print("IP AP  : "); Serial.println(WiFi.softAPIP());
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  updateGPS();
  server.handleClient();

  uint32_t nowMs = millis();

  if (nowMs - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = nowMs;
    uint32_t ts = makeEpochUTC();
    if (fixValid && ts > 0) {
      pushRaw(ts, filteredSpeed);
      addMinuteSample(ts, filteredSpeed);
    }
  }

  if (nowMs - lastDisplayMs >= 200) {
    lastDisplayMs = nowMs;
    showSpeed(filteredSpeed, fixValid);
  }

  if (nowMs - lastRgbMs >= 20) {
    lastRgbMs = nowMs;
    updateRgbLed(filteredSpeed, fixValid);
  }

  if (nowMs - lastProfileMs >= 5000) {
    lastProfileMs = nowMs;
    updateSunTimes();
  }

  applyProfileOutputs();

  if (nowMs - lastFlashTrimMs >= FLASH_TRIM_INTERVAL_MS) {
    lastFlashTrimMs = nowMs;
    trimHistoryFileIfNeeded();
  }
}
