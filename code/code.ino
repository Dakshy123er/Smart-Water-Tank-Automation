#include <WiFi.h>
#include <WebServer.h>

// ===================== Pins =====================
#define TRIG_PIN   5
#define ECHO_PIN   18
#define RELAY_PIN  26   // Active-LOW relay: LOW=ON, HIGH=OFF
#define LED_PIN    27

// ===================== Network ==================
const char* ssid     = "A9";
const char* password = "anshattre09";

// ===================== Tank Config ===============
float tank_height_cm         = 25.0;  // Total tank height (cm)
float full_threshold_cm      = 10.0;  // Gap <= this => tank considered full => valve OFF
float empty_threshold_cm     = 23.0;  // Gap >= this => tank considered empty => valve ON

// Sanity: empty should be > full for hysteresis
// ===============================================

WebServer server(80);

// ===================== State ====================
volatile bool valveOn     = false;   // true => relay LOW, valve open
float last_distance_cm    = -1.0;    // last valid distance reading
bool sensor_ok            = false;
unsigned long boot_ms     = 0;

// LED blink (non-blocking)
unsigned long lastBlinkMs = 0;
const unsigned long BLINK_ON_MS  = 120;
const unsigned long BLINK_OFF_MS = 180;
bool ledState = false;

// ===================== Utils ====================
static inline void valveWrite(bool on) {
  digitalWrite(RELAY_PIN, on ? LOW : HIGH); // Active-LOW
  valveOn = on;
}

static inline void corsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

float readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Timeout 30ms ~ 5m max
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return -1.0f;

  // Speed of sound ~0.0343 cm/us (20-25C typical)
  float d = (duration * 0.0343f) / 2.0f;
  return d;
}

// Take multiple samples with simple outlier resistance
float getDistanceAveraged(uint8_t samples = 7) {
  float acc = 0.0f;
  uint8_t good = 0;

  for (uint8_t i = 0; i < samples; i++) {
    float d = readDistanceOnce();
    if (d > 0 && d < 1000) { // sanity cap
      acc += d;
      good++;
    }
    delay(20);
  }

  if (good == 0) return -1.0f;
  return acc / good;
}

String jsonStatus() {
  float gap = last_distance_cm;
  float level_cm = (gap < 0) ? -1 : max(0.0f, tank_height_cm - gap);
  float level_pct = (gap < 0) ? -1 : (100.0f * level_cm / tank_height_cm);
  String ip = WiFi.localIP().toString();

  String j = "{";
  j += "\"ip\":\"" + ip + "\",";
  j += "\"uptime_ms\":" + String(millis() - boot_ms) + ",";
  j += "\"sensor_ok\":" + String(sensor_ok ? "true" : "false") + ",";
  j += "\"distance_cm\":" + String(last_distance_cm, 1) + ",";
  j += "\"level_cm\":" + String(level_cm, 1) + ",";
  j += "\"level_percent\":" + String(level_pct, 1) + ",";
  j += "\"valveOn\":" + String(valveOn ? "true" : "false") + ",";
  j += "\"thresholds\":{\"full_cm\":" + String(full_threshold_cm, 1) +
       ",\"empty_cm\":" + String(empty_threshold_cm, 1) + "},";
  j += "\"tank_height_cm\":" + String(tank_height_cm, 1);
  j += "}";
  return j;
}

void handleRoot() {
  corsHeaders();
  // Minimal dashboard
  String page = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Water Tank Controller</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;}
  .card{max-width:520px;border:1px solid #ddd;border-radius:12px;padding:16px;box-shadow:0 2px 8px rgba(0,0,0,.05)}
  .row{display:flex;justify-content:space-between;margin:8px 0}
  .bar{height:20px;background:#eee;border-radius:10px;overflow:hidden}
  .fill{height:100%;width:0%}
  button{padding:10px 14px;border-radius:10px;border:1px solid #ccc;background:#fafafa;cursor:pointer}
  input{padding:8px;border-radius:8px;border:1px solid #ccc;width:100px}
  .ok{background:#4caf50}
  .warn{background:#f44336;color:#fff}
</style>
</head>
<body>
  <h2>Smart Water Tank Controller</h2>
  <div class="card">
    <div class="row"><div>IP</div><div id="ip">-</div></div>
    <div class="row"><div>Sensor</div><div id="sensor">-</div></div>
    <div class="row"><div>Gap (cm)</div><div id="gap">-</div></div>
    <div class="row"><div>Level (cm)</div><div id="level">-</div></div>
    <div class="row"><div>Level (%)</div><div id="pct">-</div></div>
    <div class="bar"><div id="fill" class="fill"></div></div>
    <div class="row" style="margin-top:12px;">
      <button id="toggleBtn">Toggle Valve</button>
      <div>Valve: <b id="valve">-</b></div>
    </div>
    <hr/>
    <h4>Settings</h4>
    <div class="row">
      <div>Full threshold (cm)</div>
      <input id="full" type="number" step="0.1"/>
    </div>
    <div class="row">
      <div>Empty threshold (cm)</div>
      <input id="empty" type="number" step="0.1"/>
    </div>
    <div class="row">
      <div>Tank height (cm)</div>
      <input id="height" type="number" step="0.1"/>
    </div>
    <div class="row"><button id="saveBtn">Save</button><div id="saveMsg"></div></div>
  </div>

<script>
const $ = (id)=>document.getElementById(id);
async function getStatus(){
  const r = await fetch('/status');
  const j = await r.json();
  $('ip').textContent = j.ip || '-';
  $('sensor').textContent = j.sensor_ok ? 'OK' : 'ERROR';
  $('sensor').className = j.sensor_ok ? 'ok' : 'warn';
  $('gap').textContent = j.distance_cm;
  $('level').textContent = j.level_cm;
  $('pct').textContent = j.level_percent + '%';
  $('fill').style.width = Math.max(0, Math.min(100, j.level_percent)) + '%';
  $('fill').style.background = j.level_percent > 90 ? '#4caf50' : (j.level_percent < 20 ? '#f44336' : '#2196f3');
  $('valve').textContent = j.valveOn ? 'ON' : 'OFF';
  $('toggleBtn').textContent = j.valveOn ? 'Turn OFF' : 'Turn ON';
  $('full').value = j.thresholds.full_cm;
  $('empty').value = j.thresholds.empty_cm;
  $('height').value = j.tank_height_cm;
}
$('toggleBtn').onclick = async ()=>{
  await fetch('/toggle', {method:'POST'});
  getStatus();
}
$('saveBtn').onclick = async ()=>{
  const full = $('full').value;
  const empty = $('empty').value;
  const height = $('height').value;
  const r = await fetch(`/set?full=${full}&empty=${empty}&height=${height}`, {method:'POST'});
  $('saveMsg').textContent = r.ok ? 'Saved' : 'Failed';
  setTimeout(()=> $('saveMsg').textContent='', 1500);
  getStatus();
}
getStatus();
setInterval(getStatus, 1500);
</script>
</body>
</html>
)HTML";
  server.send(200, "text/html; charset=utf-8", page);
}

void handleStatus() {
  corsHeaders();
  server.send(200, "application/json", jsonStatus());
}

void handleToggle() {
  corsHeaders();
  // Simple toggle
  valveWrite(!valveOn);
  server.send(200, "application/json", String("{\"valveOn\":") + (valveOn ? "true" : "false") + "}");
}

void handleSet() {
  corsHeaders();
  // Accept query params: full, empty, height
  if (server.hasArg("full")) {
    full_threshold_cm = server.arg("full").toFloat();
  }
  if (server.hasArg("empty")) {
    empty_threshold_cm = server.arg("empty").toFloat();
  }
  if (server.hasArg("height")) {
    tank_height_cm = server.arg("height").toFloat();
  }

  // Ensure hysteresis sanity
  if (empty_threshold_cm <= full_threshold_cm) {
    empty_threshold_cm = full_threshold_cm + 1.0; // force a gap
  }
  if (tank_height_cm < full_threshold_cm + 1.0) {
    tank_height_cm = full_threshold_cm + 1.0; // ensure height makes sense
  }

  server.send(200, "application/json", jsonStatus());
}

void handleOptions() {
  corsHeaders();
  server.send(204); // No Content
}

// ===================== Setup ====================
void setup() {
  Serial.begin(115200);
  boot_ms = millis();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  valveWrite(false); // start with valve OFF (safe default)

  Serial.print("Connecting to Wi-Fi ");
  Serial.print(ssid);
  WiFi.begin(ssid, password);
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
    if (++dots % 40 == 0) Serial.println();
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/set", HTTP_GET, handleSet); // allow GET for convenience
  server.onNotFound([](){
    corsHeaders();
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  server.on("/", HTTP_OPTIONS, handleOptions);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/toggle", HTTP_OPTIONS, handleOptions);
  server.on("/set", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("HTTP server started.");
}

// ===================== Loop =====================
void loop() {
  // Read distance
  float d = getDistanceAveraged(7);
  if (d < 0) {
    // Sensor failed this cycle
    sensor_ok = false;
    // Fail-safe: turn valve OFF to avoid overflow risk if we don't know the level
    if (valveOn) {
      Serial.println("Sensor error. Turning OFF valve (fail-safe).");
      valveWrite(false);
    }
  } else {
    sensor_ok = true;
    last_distance_cm = d;

    // Control with hysteresis
    // If tank is low (big gap >= empty_threshold) -> turn valve ON
    if (!valveOn && (last_distance_cm >= empty_threshold_cm)) {
      Serial.printf("Gap %.1fcm >= empty(%.1f). Valve ON.\n", last_distance_cm, empty_threshold_cm);
      valveWrite(true);
    }
    // If tank is full (small gap <= full_threshold) -> turn valve OFF
    else if (valveOn && (last_distance_cm <= full_threshold_cm)) {
      Serial.printf("Gap %.1fcm <= full(%.1f). Valve OFF.\n", last_distance_cm, full_threshold_cm);
      valveWrite(false);
    }
  }

  // Non-blocking LED blink if valve is ON
  unsigned long now = millis();
  if (valveOn) {
    if (!ledState && now - lastBlinkMs >= BLINK_OFF_MS) {
      ledState = true;
      lastBlinkMs = now;
      digitalWrite(LED_PIN, HIGH);
    } else if (ledState && now - lastBlinkMs >= BLINK_ON_MS) {
      ledState = false;
      lastBlinkMs = now;
      digitalWrite(LED_PIN, LOW);
    }
  } else {
    if (ledState) {
      ledState = false;
      digitalWrite(LED_PIN, LOW);
    }
  }

  server.handleClient();
  // Small yield to keep WiFi stack happy
  delay(5);
}
