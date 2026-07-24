#include "WebServerManager.h"
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include "TempSensor.h"
#include "ValveServo.h"
#include "TempHistory.h"
#include "TimeManager.h"
#include "NetworkManager.h"
#include "Storage.h"
#include "BatteryMonitor.h"

namespace WebServerManager {

static ESP8266WebServer server(80);
static Callbacks callbacks_;

// ----------------------------------------------------------------------
// Доступ по паролю (HTTP Basic Auth) к странице настроек и её API.
// Дашборд (/, /api/status, /api/history) остаётся открытым для мониторинга.
// Логин фиксирован (ADMIN_USERNAME), пароль - из settings.admin.password,
// его можно сменить через /settings ("Безопасность") или через меню на
// дисплее. Возвращает false и уже отправляет клиенту 401, если не прошли
// проверку - вызывающий обработчик должен в этом случае просто выйти.
static bool requireAuth() {
    AppSettings *s = callbacks_.getSettings();
    if (!server.authenticate(ADMIN_USERNAME, s->admin.password)) {
        server.requestAuthentication();
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------
// HTML хранится в PROGMEM, чтобы не занимать оперативную память постоянно.
// Копия во временный буфер делается только на время обработки запроса.
// ----------------------------------------------------------------------

static const char PAGE_INDEX[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Valve PID - Мониторинг</title>
<style>
body{font-family:sans-serif;background:#f4f6f8;margin:0;padding:16px;color:#222}
.card{background:#fff;border-radius:10px;padding:16px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.15)}
h1{font-size:20px;margin:0 0 12px}
.row{display:flex;gap:16px;flex-wrap:wrap}
.stat{flex:1;min-width:120px}
.stat .val{font-size:28px;font-weight:bold}
.stat .lbl{font-size:12px;color:#777}
nav a{margin-right:16px;color:#0366d6;text-decoration:none;font-weight:bold}
canvas{width:100%;height:220px}
.badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:12px;color:#fff}
.ok{background:#2ea44f}.bad{background:#cb2431}
</style></head><body>
<nav><a href="/">Мониторинг</a><a href="/settings">Настройки</a></nav>
<div class="card"><h1>Состояние устройства</h1>
<div class="row">
<div class="stat"><div class="val" id="temp">--</div><div class="lbl">Температура, &deg;C</div></div>
<div class="stat"><div class="val" id="valve">--</div><div class="lbl">Открытие крана, %</div></div>
<div class="stat"><div class="val" id="setpoint">--</div><div class="lbl">Уставка, &deg;C</div></div>
<div class="stat"><div class="val"><span id="wifi" class="badge">...</span></div><div class="lbl">Сеть / IP: <span id="ip">-</span></div></div>
<div class="stat"><div class="val" id="battVal">--</div><div class="lbl">Батарея: <span id="battBadge" class="badge">...</span></div></div>
</div></div>
<div class="card"><h1>График за 24 часа</h1><canvas id="chart"></canvas></div>
<script>
async function refreshStatus(){
  try{
    const r = await fetch('/api/status'); const d = await r.json();
    document.getElementById('temp').textContent = d.temp.toFixed(1);
    document.getElementById('valve').textContent = d.valve.toFixed(0);
    document.getElementById('setpoint').textContent = d.setpoint.toFixed(1);
    const wifiEl = document.getElementById('wifi');
    wifiEl.textContent = d.apMode ? 'AP режим' : (d.wifiConnected ? 'Подключено' : 'Нет сети');
    wifiEl.className = 'badge ' + (d.wifiConnected && !d.apMode ? 'ok' : 'bad');
    document.getElementById('ip').textContent = d.ip;

    const battBadge = document.getElementById('battBadge');
    const battVal = document.getElementById('battVal');
    if (!d.battery.connected) {
      battVal.textContent = '--';
      battBadge.textContent = 'не подключена';
      battBadge.className = 'badge bad';
    } else {
      battVal.textContent = d.battery.percent.toFixed(0) + '% (' + d.battery.voltage.toFixed(2) + 'В)';
      battBadge.textContent = 'подключена';
      battBadge.className = 'badge ok';
    }
  }catch(e){}
}
let historyCache = [];
async function refreshHistory(){
  try{
    const r = await fetch('/api/history'); historyCache = await r.json(); drawChart();
  }catch(e){}
}
function drawChart(){
  const c = document.getElementById('chart'); const ctx = c.getContext('2d');
  const w = c.clientWidth, h = c.clientHeight;
  c.width = w; c.height = h;
  ctx.clearRect(0,0,w,h);
  if (!historyCache.length){ ctx.fillText('Нет данных', 10, 20); return; }
  const vals = historyCache.map(p=>p.v);
  const minV = Math.min(...vals) - 1, maxV = Math.max(...vals) + 1;
  const pad = 30;
  ctx.strokeStyle = '#0366d6'; ctx.lineWidth = 2; ctx.beginPath();
  historyCache.forEach((p,i)=>{
    const x = pad + (w - pad*2) * (i / (historyCache.length - 1 || 1));
    const y = h - pad - (h - pad*2) * ((p.v - minV) / (maxV - minV || 1));
    if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.stroke();
  ctx.fillStyle = '#777'; ctx.font = '11px sans-serif';
  ctx.fillText(maxV.toFixed(1)+'C', 2, pad);
  ctx.fillText(minV.toFixed(1)+'C', 2, h - pad + 10);
}
refreshStatus(); refreshHistory();
setInterval(refreshStatus, 2000);
setInterval(refreshHistory, 60000);
window.addEventListener('resize', drawChart);
</script></body></html>
)HTML";

static const char PAGE_SETTINGS[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Valve PID - Настройки</title>
<style>
body{font-family:sans-serif;background:#f4f6f8;margin:0;padding:16px;color:#222}
.card{background:#fff;border-radius:10px;padding:16px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.15)}
h1{font-size:20px;margin:0 0 12px}
nav a{margin-right:16px;color:#0366d6;text-decoration:none;font-weight:bold}
label{display:block;margin-top:10px;font-size:13px;color:#555}
input,select{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;border:1px solid #ccc;border-radius:6px}
button{margin-top:14px;padding:10px 18px;border:none;border-radius:6px;background:#0366d6;color:#fff;font-weight:bold;cursor:pointer}
.msg{margin-top:10px;font-size:13px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}
</style></head><body>
<nav><a href="/">Мониторинг</a><a href="/settings">Настройки</a></nav>

<div class="card"><h1>ПИД-регулятор</h1>
<form id="pidForm">
<div class="grid">
<div><label>Kp<input type="number" step="0.01" name="kp"></label></div>
<div><label>Ki<input type="number" step="0.01" name="ki"></label></div>
<div><label>Kd<input type="number" step="0.01" name="kd"></label></div>
<div><label>Уставка, &deg;C<input type="number" step="0.1" name="setpoint"></label></div>
</div>
<button type="submit">Сохранить ПИД</button><div class="msg" id="pidMsg"></div>
</form></div>

<div class="card"><h1>Сервопривод / кран</h1>
<form id="servoForm">
<div class="grid">
<div><label>Мин. открытие, %<input type="number" min="0" max="100" name="minPercent"></label></div>
<div><label>Макс. открытие, %<input type="number" min="0" max="100" name="maxPercent"></label></div>
<div><label>Импульс "закрыто", мкс<input type="number" name="closedPulseUs"></label></div>
<div><label>Импульс "открыто", мкс<input type="number" name="openPulseUs"></label></div>
</div>
<button type="submit">Сохранить сервопривод</button><div class="msg" id="servoMsg"></div>
</form></div>

<div class="card"><h1>Батарея</h1>
<p style="font-size:13px;color:#555;margin-top:0">Укажите реальное напряжение батареи, соответствующее 0% и 100% заряда (измерьте мультиметром на полностью разряженной и полностью заряженной батарее).</p>
<p style="font-size:13px;color:#555">Если на пин A0 батарея подключена через собственный (внешний) резистивный делитель напряжения, укажите его коэффициент: ratio = (R1+R2)/R2, где R1 - от плюса батареи к A0, R2 - от A0 к земле. Если делителя нет (кроме встроенного в плату) - оставьте 1.</p>
<form id="battForm">
<div class="grid">
<div><label>Напряжение при 0%, В<input type="number" step="0.01" name="v0"></label></div>
<div><label>Напряжение при 100%, В<input type="number" step="0.01" name="v100"></label></div>
<div><label>Коэффициент делителя (A0)<input type="number" step="0.001" min="1" name="dividerRatio"></label></div>
</div>
<button type="submit">Сохранить калибровку батареи</button><div class="msg" id="battMsg"></div>
</form></div>

<div class="card"><h1>Безопасность</h1>
<p style="font-size:13px;color:#555;margin-top:0">Пароль для входа в эти настройки (веб и меню на дисплее). Логин фиксирован: <b>admin</b>.</p>
<form id="secForm">
<label>Текущий пароль<input type="password" name="currentPassword" autocomplete="current-password"></label>
<label>Новый пароль<input type="password" name="newPassword" maxlength="32" autocomplete="new-password"></label>
<label>Повторите новый пароль<input type="password" name="newPasswordConfirm" maxlength="32" autocomplete="new-password"></label>
<button type="submit">Сменить пароль</button><div class="msg" id="secMsg"></div>
</form></div>

<div class="card"><h1>Wi-Fi / сеть</h1>
<form id="netForm">
<label>SSID<input type="text" name="ssid" maxlength="32"></label>
<label>Пароль (оставьте пустым, чтобы не менять)<input type="password" name="password" maxlength="64"></label>
<label>Режим получения IP
<select name="dhcp"><option value="1">DHCP (автоматически)</option><option value="0">Статический IP</option></select>
</label>
<div class="grid">
<div><label>IP-адрес<input type="text" name="ip" placeholder="192.168.1.50"></label></div>
<div><label>Шлюз<input type="text" name="gateway" placeholder="192.168.1.1"></label></div>
<div><label>Маска подсети<input type="text" name="subnet" placeholder="255.255.255.0"></label></div>
<div><label>DNS<input type="text" name="dns" placeholder="8.8.8.8"></label></div>
</div>
<button type="submit">Сохранить сеть и переподключиться</button><div class="msg" id="netMsg"></div>
</form></div>

<script>
function ipToStr(n){ if(!n) return ''; return [(n>>24)&255,(n>>16)&255,(n>>8)&255,n&255].join('.'); }
function strToIp(s){ const p=(s||'').split('.').map(Number); if(p.length!==4||p.some(isNaN)) return 0; return ((p[0]<<24)>>>0)+((p[1]<<16))+((p[2]<<8))+p[3]; }

async function loadSettings(){
  const r = await fetch('/api/settings'); const d = await r.json();
  const pf = document.getElementById('pidForm');
  pf.kp.value = d.pid.kp; pf.ki.value = d.pid.ki; pf.kd.value = d.pid.kd; pf.setpoint.value = d.pid.setpoint;
  const sf = document.getElementById('servoForm');
  sf.minPercent.value = d.servo.minPercent; sf.maxPercent.value = d.servo.maxPercent;
  sf.closedPulseUs.value = d.servo.closedPulseUs; sf.openPulseUs.value = d.servo.openPulseUs;
  const bf = document.getElementById('battForm');
  bf.v0.value = d.battery.v0; bf.v100.value = d.battery.v100; bf.dividerRatio.value = d.battery.dividerRatio;
  const nf = document.getElementById('netForm');
  nf.ssid.value = d.net.ssid; nf.dhcp.value = d.net.dhcp ? '1':'0';
  nf.ip.value = ipToStr(d.net.ip); nf.gateway.value = ipToStr(d.net.gateway);
  nf.subnet.value = ipToStr(d.net.subnet); nf.dns.value = ipToStr(d.net.dns);
}
async function postForm(url, data, msgId){
  const msg = document.getElementById(msgId);
  msg.textContent = 'Сохранение...';
  try{
    const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: new URLSearchParams(data)});
    if (r.ok) { msg.textContent = 'Сохранено'; msg.style.color = 'green'; }
    else { msg.textContent = 'Ошибка сохранения'; msg.style.color = 'red'; }
  }catch(e){ msg.textContent = 'Ошибка сети'; msg.style.color = 'red'; }
}
document.getElementById('pidForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/pid', {kp:f.kp.value, ki:f.ki.value, kd:f.kd.value, setpoint:f.setpoint.value}, 'pidMsg');
});
document.getElementById('servoForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/servo', {minPercent:f.minPercent.value, maxPercent:f.maxPercent.value, closedPulseUs:f.closedPulseUs.value, openPulseUs:f.openPulseUs.value}, 'servoMsg');
});
document.getElementById('battForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/battery', {v0:f.v0.value, v100:f.v100.value, dividerRatio:f.dividerRatio.value}, 'battMsg');
});
document.getElementById('secForm').addEventListener('submit', async function(e){
  e.preventDefault();
  const f = e.target;
  const msg = document.getElementById('secMsg');
  if (f.newPassword.value !== f.newPasswordConfirm.value) {
    msg.textContent = 'Новые пароли не совпадают'; msg.style.color = 'red'; return;
  }
  if (f.newPassword.value.length < 4) {
    msg.textContent = 'Пароль слишком короткий (мин. 4 симв.)'; msg.style.color = 'red'; return;
  }
  msg.textContent = 'Сохранение...';
  try{
    const r = await fetch('/api/settings/security', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body: new URLSearchParams({currentPassword:f.currentPassword.value, newPassword:f.newPassword.value})});
    const d = await r.json().catch(()=>({}));
    if (r.ok && d.ok) { msg.textContent = 'Пароль изменён'; msg.style.color = 'green'; f.reset(); }
    else { msg.textContent = d.error || 'Ошибка: неверный текущий пароль'; msg.style.color = 'red'; }
  }catch(e){ msg.textContent = 'Ошибка сети'; msg.style.color = 'red'; }
});
document.getElementById('netForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/network', {
    ssid:f.ssid.value, password:f.password.value, dhcp:f.dhcp.value,
    ip: strToIp(f.ip.value), gateway: strToIp(f.gateway.value),
    subnet: strToIp(f.subnet.value), dns: strToIp(f.dns.value)
  }, 'netMsg');
});
loadSettings();
</script></body></html>
)HTML";

// ----------------------------------------------------------------------
// Обработчики
// ----------------------------------------------------------------------

static void handleIndex() {
    server.send_P(200, "text/html", PAGE_INDEX);
}

static void handleSettingsPage() {
    if (!requireAuth()) return;
    server.send_P(200, "text/html", PAGE_SETTINGS);
}

static void handleApiStatus() {
    StaticJsonDocument<384> doc;
    doc["temp"] = TempSensor::isValid() ? TempSensor::getTemperature() : NAN;
    doc["valve"] = ValveServo::getCurrentPercent();
    doc["setpoint"] = callbacks_.getSettings()->pid.setpoint;
    doc["wifiConnected"] = NetworkManager::isConnected();
    doc["apMode"] = NetworkManager::isApMode();
    doc["ip"] = NetworkManager::getIpAddress();
    doc["timeSynced"] = TimeManager::hasSyncedOnce();
    doc["unixTime"] = TimeManager::now();

    doc["battery"]["connected"] = BatteryMonitor::isConnected();
    doc["battery"]["voltage"] = BatteryMonitor::getVoltage();
    doc["battery"]["percent"] = BatteryMonitor::getPercent();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleApiHistory() {
    String out;
    TempHistory::serializeToJson(out);
    server.send(200, "application/json", out);
}

static void handleApiSettingsGet() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    StaticJsonDocument<CONFIG_JSON_CAPACITY> doc;

    doc["pid"]["kp"] = s->pid.kp;
    doc["pid"]["ki"] = s->pid.ki;
    doc["pid"]["kd"] = s->pid.kd;
    doc["pid"]["setpoint"] = s->pid.setpoint;

    doc["servo"]["minPercent"] = s->servo.minPercent;
    doc["servo"]["maxPercent"] = s->servo.maxPercent;
    doc["servo"]["closedPulseUs"] = s->servo.closedPulseUs;
    doc["servo"]["openPulseUs"] = s->servo.openPulseUs;

    doc["net"]["ssid"] = s->network.ssid;
    // Пароль сознательно не отдаём обратно клиенту из соображений безопасности.
    doc["net"]["dhcp"] = s->network.useDhcp;
    doc["net"]["ip"] = s->network.staticIp;
    doc["net"]["gateway"] = s->network.staticGateway;
    doc["net"]["subnet"] = s->network.staticSubnet;
    doc["net"]["dns"] = s->network.staticDns;

    doc["battery"]["v0"] = s->battery.voltageAt0Percent;
    doc["battery"]["v100"] = s->battery.voltageAt100Percent;
    doc["battery"]["dividerRatio"] = s->battery.dividerRatio;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleApiSettingsPid() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    if (server.hasArg("kp")) s->pid.kp = server.arg("kp").toDouble();
    if (server.hasArg("ki")) s->pid.ki = server.arg("ki").toDouble();
    if (server.hasArg("kd")) s->pid.kd = server.arg("kd").toDouble();
    if (server.hasArg("setpoint")) s->pid.setpoint = server.arg("setpoint").toDouble();

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiSettingsServo() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    if (server.hasArg("minPercent")) s->servo.minPercent = (uint8_t)constrain(server.arg("minPercent").toInt(), 0, 100);
    if (server.hasArg("maxPercent")) s->servo.maxPercent = (uint8_t)constrain(server.arg("maxPercent").toInt(), 0, 100);
    if (server.hasArg("closedPulseUs")) s->servo.closedPulseUs = (uint16_t)server.arg("closedPulseUs").toInt();
    if (server.hasArg("openPulseUs")) s->servo.openPulseUs = (uint16_t)server.arg("openPulseUs").toInt();

    if (s->servo.minPercent > s->servo.maxPercent) {
        // защита от некорректного ввода - не даём min стать больше max
        uint8_t tmp = s->servo.minPercent;
        s->servo.minPercent = s->servo.maxPercent;
        s->servo.maxPercent = tmp;
    }

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiSettingsBattery() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    if (server.hasArg("v0")) s->battery.voltageAt0Percent = server.arg("v0").toFloat();
    if (server.hasArg("v100")) s->battery.voltageAt100Percent = server.arg("v100").toFloat();
    if (server.hasArg("dividerRatio")) {
        float ratio = server.arg("dividerRatio").toFloat();
        if (ratio < 1.0f) ratio = 1.0f; // делитель не может "усиливать" напряжение
        s->battery.dividerRatio = ratio;
    }

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    server.send(200, "application/json", "{\"ok\":true}");
}

// Смена пароля доступа к настройкам. Требует правильный текущий пароль,
// иначе тот, кто уже случайно оставил браузер авторизованным, не мог бы
// незаметно "увести" устройство сменой пароля без знания старого.
static void handleApiSettingsSecurity() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();

    String current = server.hasArg("currentPassword") ? server.arg("currentPassword") : "";
    String newPass  = server.hasArg("newPassword") ? server.arg("newPassword") : "";

    if (current != String(s->admin.password)) {
        server.send(403, "application/json", "{\"ok\":false,\"error\":\"Неверный текущий пароль\"}");
        return;
    }
    if (newPass.length() < 4) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Пароль слишком короткий\"}");
        return;
    }
    if (newPass.length() >= sizeof(s->admin.password)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Пароль слишком длинный\"}");
        return;
    }

    strlcpy(s->admin.password, newPass.c_str(), sizeof(s->admin.password));
    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiSettingsNetwork() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    if (server.hasArg("ssid")) strlcpy(s->network.ssid, server.arg("ssid").c_str(), sizeof(s->network.ssid));
    if (server.hasArg("password") && server.arg("password").length() > 0) {
        strlcpy(s->network.password, server.arg("password").c_str(), sizeof(s->network.password));
    }
    if (server.hasArg("dhcp")) s->network.useDhcp = server.arg("dhcp").toInt() != 0;
    if (server.hasArg("ip")) s->network.staticIp = (uint32_t)server.arg("ip").toInt();
    if (server.hasArg("gateway")) s->network.staticGateway = (uint32_t)server.arg("gateway").toInt();
    if (server.hasArg("subnet")) s->network.staticSubnet = (uint32_t)server.arg("subnet").toInt();
    if (server.hasArg("dns")) s->network.staticDns = (uint32_t)server.arg("dns").toInt();

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();

    server.send(200, "application/json", "{\"ok\":true}");
    // Переподключение делаем ПОСЛЕ отправки ответа, чтобы браузер успел
    // получить подтверждение до возможного разрыва текущего соединения.
    NetworkManager::applySettings(s->network);
}

static void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

void begin(const Callbacks &callbacks) {
    callbacks_ = callbacks;

    server.on("/", HTTP_GET, handleIndex);
    server.on("/settings", HTTP_GET, handleSettingsPage);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/history", HTTP_GET, handleApiHistory);
    server.on("/api/settings", HTTP_GET, handleApiSettingsGet);
    server.on("/api/settings/pid", HTTP_POST, handleApiSettingsPid);
    server.on("/api/settings/servo", HTTP_POST, handleApiSettingsServo);
    server.on("/api/settings/battery", HTTP_POST, handleApiSettingsBattery);
    server.on("/api/settings/network", HTTP_POST, handleApiSettingsNetwork);
    server.on("/api/settings/security", HTTP_POST, handleApiSettingsSecurity);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println(F("[Web] HTTP-сервер запущен на порту 80"));
}

void update() {
    server.handleClient();
}

} // namespace WebServerManager
