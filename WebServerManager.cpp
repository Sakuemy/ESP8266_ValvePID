#include "WebServerManager.h"
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <base64.h> // base64::decode - входит в состав ESP8266 core, доп. библиотека не нужна
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
// Логин фиксирован (ADMIN_USERNAME), пароль проверяется через
// Storage::verifyPassword() (хранится только SHA-256 хэш, см. Storage.h) -
// поэтому встроенный server.authenticate() тут не подходит (он сравнивает
// с открытым паролем), разбираем заголовок Authorization вручную, по той
// же схеме, что использует сам ESP8266HTTPUpdateServer из ядра. Возвращает
// false и уже отправляет клиенту 401, если не прошли проверку - вызывающий
// обработчик должен в этом случае просто выйти.
static bool requireAuth() {
    AppSettings *s = callbacks_.getSettings();

    String authReq = server.header("Authorization");
    if (authReq.startsWith("Basic ")) {
        authReq = authReq.substring(6);
        authReq.trim();
        String decoded = base64::decode(authReq);
        int colonIdx = decoded.indexOf(':');
        if (colonIdx > 0) {
            String user = decoded.substring(0, colonIdx);
            String pass = decoded.substring(colonIdx + 1);
            if (user == ADMIN_USERNAME && Storage::verifyPassword(*s, pass.c_str())) {
                return true;
            }
        }
    }

    server.sendHeader("WWW-Authenticate", "Basic realm=\"ValvePID\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
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
canvas{width:100%;height:260px}
.legend{font-size:12px;margin-bottom:6px}.legend span{margin-right:14px;white-space:nowrap}
.badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:12px;color:#fff}
.ok{background:#2ea44f}.bad{background:#cb2431}.warn{background:#e36209}
</style></head><body>
<nav><a href="/">Мониторинг</a><a href="/settings">Настройки</a></nav>
<div class="card"><h1>Состояние устройства</h1>
<div class="row">
<div class="stat"><div class="val" id="temp">--</div><div class="lbl">Температура, &deg;C</div></div>
<div class="stat"><div class="val" id="valve">--</div><div class="lbl">Открытие крана, %</div></div>
<div class="stat"><div class="val" id="setpoint">--</div><div class="lbl">Уставка, &deg;C (&plusmn;<span id="tol">-</span>)</div></div>
<div class="stat"><div class="val"><span id="mode" class="badge">...</span></div><div class="lbl">Режим крана<span id="hold"></span></div></div>
<div class="stat"><div class="val"><span id="wifi" class="badge">...</span></div><div class="lbl">Сеть / IP: <span id="ip">-</span></div></div>
<div class="stat"><div class="val" id="battVal">--</div><div class="lbl">Батарея: <span id="battBadge" class="badge">...</span></div></div>
</div></div>
<div class="card"><h1>График за 24 часа</h1>
<div class="legend"><span style="color:#0366d6">&#9632; Температура, &deg;C (левая шкала)</span><span style="color:#e36209">&#9632; Кран, % (правая шкала, среднее за 5 мин)</span><span style="color:#2ea44f">&#9632; Уставка &plusmn; погрешность</span></div>
<canvas id="chart"></canvas></div>
<script>
let status = null;
const MODE_NAMES = {auto:'Авто (ПИД)', manual:'Ручной', preview:'Настройка', test:'Тест хода'};
async function refreshStatus(){
  try{
    const r = await fetch('/api/status'); const d = await r.json(); status = d;
    document.getElementById('temp').textContent = (d.temp == null) ? '--' : d.temp.toFixed(1);
    document.getElementById('valve').textContent = d.valve.toFixed(0);
    document.getElementById('setpoint').textContent = d.setpoint.toFixed(1);
    document.getElementById('tol').textContent = d.tol.toFixed(1);
    const modeEl = document.getElementById('mode');
    modeEl.textContent = d.safety ? 'Аварийное закрытие' : (MODE_NAMES[d.mode] || d.mode);
    modeEl.className = 'badge ' + (d.mode === 'auto' && !d.safety ? 'ok' : 'warn');
    document.getElementById('hold').textContent = (d.mode === 'auto' && d.hold) ? ' (в зоне погрешности - удержание)' : '';
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
    drawChart();
  }catch(e){}
}
let historyCache = [];
async function refreshHistory(){
  try{
    const r = await fetch('/api/history'); historyCache = await r.json(); drawChart();
  }catch(e){}
}
function p2(n){ return (n < 10 ? '0' : '') + n; }
function fmtTime(t){
  if (t < 1700000000) return ''; // время ещё не синхронизировано по NTP
  const d = new Date(t * 1000); // метка уже в локальном времени - читаем как UTC
  return p2(d.getUTCHours()) + ':' + p2(d.getUTCMinutes());
}
function drawChart(){
  const c = document.getElementById('chart'); const ctx = c.getContext('2d');
  const w = c.clientWidth, h = c.clientHeight;
  c.width = w; c.height = h;
  ctx.clearRect(0,0,w,h);
  ctx.font = '11px sans-serif';
  if (!historyCache.length){ ctx.fillStyle = '#777'; ctx.fillText('Нет данных', 10, 20); return; }
  const padL = 38, padR = 34, padT = 10, padB = 20;
  const pw = w - padL - padR, ph = h - padT - padB, n = historyCache.length;
  const sp = status ? status.setpoint : null, tol = status ? status.tol : 0;
  let lo = Math.min(...historyCache.map(p=>p.v)), hi = Math.max(...historyCache.map(p=>p.v));
  if (sp !== null){ lo = Math.min(lo, sp - tol); hi = Math.max(hi, sp + tol); }
  lo -= 1; hi += 1;
  const X = i => padL + pw * (i / (n - 1 || 1));
  const YT = v => padT + ph - ph * ((v - lo) / (hi - lo || 1));
  const YS = v => padT + ph - ph * (Math.max(0, Math.min(100, v)) / 100);

  ctx.lineWidth = 1; ctx.strokeStyle = '#e5e7eb';
  for (let k = 0; k <= 4; k++){
    const y = padT + ph * k / 4;
    ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y); ctx.stroke();
    ctx.fillStyle = '#0366d6'; ctx.textAlign = 'right';
    ctx.fillText((hi - (hi - lo) * k / 4).toFixed(1), padL - 4, y + 4);
    ctx.fillStyle = '#e36209'; ctx.textAlign = 'left';
    ctx.fillText(Math.round(100 - 25 * k) + '%', w - padR + 4, y + 4);
  }
  if (sp !== null){
    if (tol > 0){
      const y1 = YT(sp + tol), y2 = YT(sp - tol);
      ctx.fillStyle = 'rgba(46,164,79,.15)'; ctx.fillRect(padL, y1, pw, y2 - y1);
    }
    ctx.strokeStyle = '#2ea44f'; ctx.lineWidth = 1; ctx.setLineDash([5,4]);
    ctx.beginPath(); ctx.moveTo(padL, YT(sp)); ctx.lineTo(w - padR, YT(sp)); ctx.stroke();
    ctx.setLineDash([]);
  }
  // работа сервопривода (открытие крана, %)
  ctx.strokeStyle = '#e36209'; ctx.lineWidth = 1.5; ctx.beginPath();
  historyCache.forEach((p,i)=>{ const x = X(i), y = YS(p.s || 0); if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y); });
  ctx.stroke();
  // температура
  ctx.strokeStyle = '#0366d6'; ctx.lineWidth = 2; ctx.beginPath();
  historyCache.forEach((p,i)=>{ const x = X(i), y = YT(p.v); if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y); });
  ctx.stroke();
  // время по оси X
  ctx.fillStyle = '#777';
  ctx.textAlign = 'left'; ctx.fillText(fmtTime(historyCache[0].t), padL, h - 4);
  ctx.textAlign = 'right'; ctx.fillText(fmtTime(historyCache[n-1].t), w - padR, h - 4);
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
button{margin-top:14px;margin-right:8px;padding:10px 18px;border:none;border-radius:6px;background:#0366d6;color:#fff;font-weight:bold;cursor:pointer}
button.sec{background:#6a737d}button:disabled{opacity:.5;cursor:default}
input[type=range]{padding:0}.hint{font-size:13px;color:#555;margin:0 0 6px}
.msg{margin-top:10px;font-size:13px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}
</style></head><body>
<nav><a href="/">Мониторинг</a><a href="/settings">Настройки</a></nav>

<div class="card"><h1>Управление краном</h1>
<p class="hint">В ручном режиме ПИД отключён. Если 10 минут не менять положение, кран сам вернётся в авто.</p>
<div>Режим: <b id="vMode">--</b> &nbsp; Открытие: <b id="vNow">--</b>%<span id="vLeft"></span></div>
<label>Положение, %: <b id="vSliderVal">--</b>
<input type="range" id="vSlider" min="0" max="100" step="1" disabled></label>
<button type="button" id="vManualBtn">Ручной режим</button><button type="button" id="vAutoBtn" class="sec">Вернуть в авто</button>
<div class="msg" id="vMsg"></div></div>

<div class="card"><h1>ПИД-регулятор</h1>
<form id="pidForm">
<div class="grid">
<div><label>Kp<input type="number" step="0.01" name="kp"></label></div>
<div><label>Ki<input type="number" step="0.01" name="ki"></label></div>
<div><label>Kd<input type="number" step="0.01" name="kd"></label></div>
<div><label>Уставка, &deg;C<input type="number" step="0.1" name="setpoint"></label></div>
<div><label>Допустимая погрешность, &plusmn;&deg;C<input type="number" step="0.1" min="0" max="20" name="tolerance"></label></div>
</div>
<p class="hint" style="margin-top:8px">Пока температура в пределах уставка &plusmn; погрешность, кран остаётся на месте и не дёргается мелкими поправками. 0 - выключено.</p>
<button type="submit">Сохранить ПИД</button><div class="msg" id="pidMsg"></div>
</form></div>

<div class="card"><h1>Сервопривод / кран</h1>
<form id="servoForm">
<p class="hint">При изменении &laquo;Мин.&raquo; и &laquo;Макс.&raquo; серва сразу поворачивается на это значение (предпросмотр). Импульсы &laquo;закрыто&raquo;/&laquo;открыто&raquo; применяются к серве сразу (плавно). Если не сохранить, через ~20 с без правок всё вернётся к сохранённым значениям.</p>
<div class="grid">
<div><label>Мин. открытие, %<input type="number" min="0" max="100" name="minPercent"></label></div>
<div><label>Макс. открытие, %<input type="number" min="0" max="100" name="maxPercent"></label></div>
<div><label>Импульс "закрыто", мкс<input type="number" min="500" max="2500" step="10" name="closedPulseUs"></label></div>
<div><label>Импульс "открыто", мкс<input type="number" min="500" max="2500" step="10" name="openPulseUs"></label></div>
</div>
<button type="submit">Сохранить сервопривод</button><button type="button" id="testBtn" class="sec">Тест: мин &harr; макс</button>
<div class="msg" id="servoMsg"></div><div class="msg" id="testMsg"></div>
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

<div class="card"><h1>Время / часовой пояс</h1>
<p style="font-size:13px;color:#555;margin-top:0">Синхронизация времени идёт по NTP в UTC; здесь задаётся только смещение для отображения локального времени на графике. Работает без Wi-Fi только приблизительно (счёт от последней успешной синхронизации).</p>
<form id="timeForm">
<label>Часовой пояс (смещение от UTC)
<select name="gmtOffsetHours">
<option value="-12">UTC-12</option><option value="-11">UTC-11</option><option value="-10">UTC-10</option>
<option value="-9">UTC-9</option><option value="-8">UTC-8</option><option value="-7">UTC-7</option>
<option value="-6">UTC-6</option><option value="-5">UTC-5</option><option value="-4">UTC-4</option>
<option value="-3">UTC-3</option><option value="-2">UTC-2</option><option value="-1">UTC-1</option>
<option value="0">UTC+0</option><option value="1">UTC+1</option><option value="2">UTC+2 (Калининград)</option>
<option value="3">UTC+3 (Москва)</option><option value="4">UTC+4 (Самара)</option><option value="5">UTC+5 (Екатеринбург)</option>
<option value="5.5">UTC+5:30 (Индия)</option><option value="6">UTC+6 (Омск)</option><option value="7">UTC+7 (Красноярск)</option>
<option value="8">UTC+8 (Иркутск)</option><option value="9">UTC+9 (Якутск)</option><option value="10">UTC+10 (Владивосток)</option>
<option value="11">UTC+11 (Магадан)</option><option value="12">UTC+12 (Камчатка)</option><option value="13">UTC+13</option><option value="14">UTC+14</option>
</select>
</label>
<button type="submit">Сохранить часовой пояс</button><div class="msg" id="timeMsg"></div>
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
  pf.kp.value = d.pid.kp; pf.ki.value = d.pid.ki; pf.kd.value = d.pid.kd; pf.setpoint.value = d.pid.setpoint; pf.tolerance.value = d.pid.tolerance;
  const sf = document.getElementById('servoForm');
  sf.minPercent.value = d.servo.minPercent; sf.maxPercent.value = d.servo.maxPercent;
  sf.closedPulseUs.value = d.servo.closedPulseUs; sf.openPulseUs.value = d.servo.openPulseUs;
  setSliderBounds(d.servo.minPercent, d.servo.maxPercent);
  const bf = document.getElementById('battForm');
  bf.v0.value = d.battery.v0; bf.v100.value = d.battery.v100; bf.dividerRatio.value = d.battery.dividerRatio;
  const tf = document.getElementById('timeForm');
  tf.gmtOffsetHours.value = (d.time.gmtOffsetSec / 3600);
  const nf = document.getElementById('netForm');
  nf.ssid.value = d.net.ssid; nf.dhcp.value = d.net.dhcp ? '1':'0';
  nf.ip.value = ipToStr(d.net.ip); nf.gateway.value = ipToStr(d.net.gateway);
  nf.subnet.value = ipToStr(d.net.subnet); nf.dns.value = ipToStr(d.net.dns);
}
// После сохранения серво подтягиваем с устройства только ЕГО поля (сервер мог
// поменять местами min/max) - остальные несохранённые формы не трогаем.
async function reloadServoForm(){
  try{
    const r = await fetch('/api/settings'); const d = await r.json();
    const sf = document.getElementById('servoForm');
    sf.minPercent.value = d.servo.minPercent; sf.maxPercent.value = d.servo.maxPercent;
    sf.closedPulseUs.value = d.servo.closedPulseUs; sf.openPulseUs.value = d.servo.openPulseUs;
    setSliderBounds(d.servo.minPercent, d.servo.maxPercent);
  }catch(e){}
}
async function postForm(url, data, msgId){
  const msg = document.getElementById(msgId);
  msg.textContent = 'Сохранение...';
  try{
    const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: new URLSearchParams(data)});
    if (r.ok) { msg.textContent = 'Сохранено'; msg.style.color = 'green'; }
    else { const d = await r.json().catch(function(){return {};}); msg.textContent = d.error || 'Ошибка сохранения'; msg.style.color = 'red'; }
  }catch(e){ msg.textContent = 'Ошибка сети'; msg.style.color = 'red'; }
}
document.getElementById('pidForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/pid', {kp:f.kp.value, ki:f.ki.value, kd:f.kd.value, setpoint:f.setpoint.value, tolerance:f.tolerance.value}, 'pidMsg');
});
document.getElementById('servoForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  clearTimeout(previewTimer);
  postForm('/api/settings/servo', {minPercent:f.minPercent.value, maxPercent:f.maxPercent.value, closedPulseUs:f.closedPulseUs.value, openPulseUs:f.openPulseUs.value}, 'servoMsg')
    .then(reloadServoForm);
});
document.getElementById('battForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  postForm('/api/settings/battery', {v0:f.v0.value, v100:f.v100.value, dividerRatio:f.dividerRatio.value}, 'battMsg');
});
document.getElementById('timeForm').addEventListener('submit', function(e){
  e.preventDefault();
  const f = e.target;
  const gmtOffsetSec = Math.round(parseFloat(f.gmtOffsetHours.value) * 3600);
  postForm('/api/settings/time', {gmtOffsetSec}, 'timeMsg');
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

// ---------- Управление краном / предпросмотр min-max / тест хода ----------
function api(url, data){
  return fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:new URLSearchParams(data)});
}
const vs = document.getElementById('vSlider');
let vDragging = false, vTimer = null, lastStatus = null, previewTimer = null, activeField = null;
function setSliderBounds(mn, mx){ vs.min = Math.min(mn, mx); vs.max = Math.max(mn, mx); }
function fmtLeft(sec){ return Math.floor(sec/60) + ':' + (sec%60 < 10 ? '0' : '') + (sec%60); }

vs.addEventListener('input', function(){
  vDragging = true;
  document.getElementById('vSliderVal').textContent = vs.value;
  clearTimeout(vTimer);
  vTimer = setTimeout(async function(){
    try{ await api('/api/valve', {mode:'manual', percent:vs.value}); }catch(e){}
    vDragging = false;
  }, 200);
});
document.getElementById('vManualBtn').addEventListener('click', async function(){
  const msg = document.getElementById('vMsg');
  try{
    const r = await api('/api/valve', {mode:'manual', percent: lastStatus ? lastStatus.valve : 0});
    if (r.ok){ msg.textContent = ''; }
    else { const d = await r.json().catch(function(){return {};}); msg.textContent = d.error || 'Ошибка'; }
    msg.style.color = 'red';
  }catch(e){ msg.textContent = 'Ошибка сети'; msg.style.color = 'red'; }
  pollStatus();
});
document.getElementById('vAutoBtn').addEventListener('click', async function(){
  try{ await api('/api/valve', {mode:'auto'}); }catch(e){}
  pollStatus();
});

// Предпросмотр: при вводе Мин/Макс серва сразу едет на это значение.
function sendCalib(){
  const f = document.getElementById('servoForm');
  const c = parseInt(f.closedPulseUs.value), o = parseInt(f.openPulseUs.value);
  if (isNaN(c) || isNaN(o)) return;
  api('/api/servo/calibrate', {closedPulseUs: c, openPulseUs: o}).catch(function(){});
}
function sendPreview(input){
  if (input.name === 'closedPulseUs' || input.name === 'openPulseUs'){ sendCalib(); return; }
  const v = parseInt(input.value);
  if (isNaN(v)) return;
  api('/api/servo/preview', {percent: Math.max(0, Math.min(100, v))}).catch(function(){});
}
function schedulePreview(input){
  clearTimeout(previewTimer);
  previewTimer = setTimeout(function(){ sendPreview(input); }, 250);
}
['minPercent','maxPercent','closedPulseUs','openPulseUs'].forEach(function(name){
  const inp = document.getElementById('servoForm')[name];
  inp.addEventListener('input', function(){ schedulePreview(inp); });
  inp.addEventListener('focus', function(){ activeField = inp; schedulePreview(inp); });
  inp.addEventListener('blur', function(){ if (activeField === inp) activeField = null; });
});
// Пока поле в фокусе - продлеваем предпросмотр (на устройстве он гаснет через ~20с;
// для импульсов это ещё и откат к сохранённым значениям, если не сохранить).
setInterval(function(){ if (activeField) sendPreview(activeField); }, 8000);

document.getElementById('testBtn').addEventListener('click', async function(){
  const f = document.getElementById('servoForm');
  const msg = document.getElementById('testMsg');
  const running = lastStatus && lastStatus.mode === 'test';
  msg.textContent = ''; msg.style.color = '';
  clearTimeout(previewTimer);
  try{
    const r = await api('/api/servo/test', running ? {action:'stop'} :
      {action:'start', min:f.minPercent.value, max:f.maxPercent.value});
    if (!r.ok && !running){ const d = await r.json().catch(function(){return {};}); msg.textContent = d.error || 'Ошибка'; msg.style.color = 'red'; }
  }catch(e){ msg.textContent = 'Ошибка сети'; msg.style.color = 'red'; }
  pollStatus();
});

const MODE_NAMES = {auto:'Авто (ПИД)', manual:'Ручной', preview:'Настройка min/max', test:'Тест хода'};
const TEST_STAGES = ['', 'едет в минимум...', 'едет в максимум...', 'возврат в минимум...'];
async function pollStatus(){
  try{
    const r = await fetch('/api/status'); const d = await r.json(); lastStatus = d;
    document.getElementById('vMode').textContent = d.safety ? 'Аварийное закрытие (нет датчика / батарея)' : (MODE_NAMES[d.mode] || d.mode);
    document.getElementById('vNow').textContent = d.valve.toFixed(0);
    document.getElementById('vLeft').textContent = d.modeLeft > 0 ? ' \u00b7 возврат в авто через ' + fmtLeft(d.modeLeft) : '';
    const manual = d.mode === 'manual';
    vs.disabled = !manual;
    if (manual && !vDragging){ vs.value = Math.round(d.valveTarget); document.getElementById('vSliderVal').textContent = vs.value; }
    if (!manual && !vDragging){ document.getElementById('vSliderVal').textContent = '--'; }
    document.getElementById('vManualBtn').disabled = manual || d.mode === 'test' || !!d.safety;
    document.getElementById('vAutoBtn').disabled = d.mode === 'auto';
    const running = d.mode === 'test';
    document.getElementById('testBtn').innerHTML = running ? 'Остановить тест' : 'Тест: мин &harr; макс';
    document.getElementById('testBtn').disabled = !!d.safety && !running;
    const tm = document.getElementById('testMsg');
    if (running){ tm.textContent = 'Тест: ' + (TEST_STAGES[d.testStage] || ''); tm.style.color = '#555'; }
    else if (tm.style.color !== 'red'){ tm.textContent = ''; }
  }catch(e){}
}
pollStatus(); setInterval(pollStatus, 1500);

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
    StaticJsonDocument<640> doc;
    doc["temp"] = TempSensor::isValid() ? TempSensor::getTemperature() : NAN; // NaN -> null в JSON
    doc["valve"] = ValveServo::getCurrentPercent();
    doc["valveTarget"] = ValveServo::getTargetPercent();
    doc["mode"] = ValveServo::getModeName();            // auto / manual / preview / test
    doc["modeLeft"] = ValveServo::getModeRemainingSec(); // сек до авто-возврата (0 - нет)
    doc["testStage"] = ValveServo::getRangeTestStage();
    doc["safety"] = ValveServo::isSafetyClose();          // аварийное закрытие: нет датчика / критический заряд
    doc["setpoint"] = callbacks_.getSettings()->pid.setpoint;
    doc["tol"] = callbacks_.getSettings()->pid.tolerance;
    doc["hold"] = callbacks_.isPidHolding ? callbacks_.isPidHolding() : false;
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
    doc["pid"]["tolerance"] = s->pid.tolerance;

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

    doc["time"]["gmtOffsetSec"] = s->time.gmtOffsetSec;

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
    if (server.hasArg("tolerance")) {
        double tol = server.arg("tolerance").toDouble();
        if (tol < 0.0) tol = 0.0;
        if (tol > 20.0) tol = 20.0;
        s->pid.tolerance = tol;
    }

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiSettingsServo() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    uint16_t closedUs = server.hasArg("closedPulseUs") ? (uint16_t)constrain(server.arg("closedPulseUs").toInt(), 0, 65535) : s->servo.closedPulseUs;
    uint16_t openUs = server.hasArg("openPulseUs") ? (uint16_t)constrain(server.arg("openPulseUs").toInt(), 0, 65535) : s->servo.openPulseUs;
    if (closedUs < SERVO_PULSE_MIN_US || closedUs > SERVO_PULSE_MAX_US ||
        openUs < SERVO_PULSE_MIN_US || openUs > SERVO_PULSE_MAX_US ||
        abs((int)openUs - (int)closedUs) < SERVO_MIN_PULSE_SPAN_US) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Импульсы: 500..2500 мкс, разница не менее 100\"}");
        return;
    }
    // Всё проверено - только теперь меняем настройки в памяти.
    if (server.hasArg("minPercent")) s->servo.minPercent = (uint8_t)constrain(server.arg("minPercent").toInt(), 0, 100);
    if (server.hasArg("maxPercent")) s->servo.maxPercent = (uint8_t)constrain(server.arg("maxPercent").toInt(), 0, 100);
    s->servo.closedPulseUs = closedUs;
    s->servo.openPulseUs = openUs;

    if (s->servo.minPercent > s->servo.maxPercent) {
        // защита от некорректного ввода - не даём min стать больше max
        uint8_t tmp = s->servo.minPercent;
        s->servo.minPercent = s->servo.maxPercent;
        s->servo.maxPercent = tmp;
    }

    Storage::save(*s);
    if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
    // Настройка границ закончена (сохранены) - выходим из предпросмотра,
    // кран возвращается к ПИД с новыми лимитами.
    ValveServo::endPreview();
    server.send(200, "application/json", "{\"ok\":true}");
}

// Ручное управление краном из веба: mode=manual&percent=NN | mode=auto.
// Ручной режим сам возвращается в авто через VALVE_MANUAL_WEB_TIMEOUT_MS
// без новых команд - чтобы кран не остался без присмотра при закрытой вкладке.
static void handleApiValve() {
    if (!requireAuth()) return;
    String mode = server.hasArg("mode") ? server.arg("mode") : "";

    if (mode == "auto") {
        ValveServo::setManualOverride(false);
    } else if (mode == "manual") {
        // Из предпросмотра/теста тоже можно перейти в ручной режим.
        if (ValveServo::getMode() != ValveServo::Mode::MANUAL) {
            if (!ValveServo::setManualOverride(true, VALVE_MANUAL_WEB_TIMEOUT_MS)) {
                server.send(409, "application/json", "{\"ok\":false,\"error\":\"Аварийное закрытие (нет датчика / батарея): ручной режим недоступен\"}");
                return;
            }
        }
        if (ValveServo::isSafetyClose()) { // сработало уже в ручном режиме - до ближайшего такта
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"Аварийное закрытие (нет датчика / батарея): ручной режим недоступен\"}");
            return;
        }
        if (server.hasArg("percent")) {
            ValveServo::setManualPercent(server.arg("percent").toDouble()); // границы min/max применяет ValveServo::update()
        } else {
            ValveServo::setManualPercent(ValveServo::getCurrentPercent()); // просто продлить тайм-аут
        }
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"mode: auto|manual\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// Предпросмотр границы хода: пока пользователь редактирует Мин/Макс в
// форме, серва сразу поворачивается на это значение (без сохранения).
static void handleApiServoPreview() {
    if (!requireAuth()) return;
    if (!server.hasArg("percent")) {
        server.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    if (ValveServo::isSafetyClose()) {
        server.send(409, "application/json", "{\"ok\":false,\"error\":\"Аварийное закрытие: предпросмотр недоступен\"}");
        return;
    }
    ValveServo::previewPercent(server.arg("percent").toDouble());
    server.send(200, "application/json", "{\"ok\":true}");
}

// Предпросмотр калибровки импульсов: значения из формы применяются к серве
// сразу (плавно), но не сохраняются. Без повторного вызова ~20 с - откат.
static void handleApiServoCalibrate() {
    if (!requireAuth()) return;
    if (!server.hasArg("closedPulseUs") || !server.hasArg("openPulseUs")) {
        server.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    long c = server.arg("closedPulseUs").toInt();
    long o = server.arg("openPulseUs").toInt();
    if (c < 0 || c > 65535 || o < 0 || o > 65535 ||
        !ValveServo::previewCalibration((uint16_t)c, (uint16_t)o)) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Импульсы: 500..2500 мкс, разница не менее 100\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// Тест хода: action=start[&min=..&max=..] | action=stop. Значения min/max
// берутся из формы (ещё не сохранённые), иначе - из сохранённых настроек.
static void handleApiServoTest() {
    if (!requireAuth()) return;
    String action = server.hasArg("action") ? server.arg("action") : "";

    if (action == "stop") {
        ValveServo::stopRangeTest();
        server.send(200, "application/json", "{\"ok\":true}");
        return;
    }
    if (action == "start") {
        if (ValveServo::isSafetyClose()) {
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"Аварийное закрытие (нет датчика / батарея): тест недоступен\"}");
            return;
        }
        AppSettings *s = callbacks_.getSettings();
        double mn = server.hasArg("min") ? server.arg("min").toDouble() : s->servo.minPercent;
        double mx = server.hasArg("max") ? server.arg("max").toDouble() : s->servo.maxPercent;
        if (mn > mx) { double t = mn; mn = mx; mx = t; }
        if (!ValveServo::startRangeTest(mn, mx)) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Мин. должно быть меньше макс.\"}");
            return;
        }
        server.send(200, "application/json", "{\"ok\":true}");
        return;
    }
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"action: start|stop\"}");
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

// Смещение часового пояса, в секундах от UTC. Диапазон ограничен реально
// существующими часовыми поясами (-12..+14ч) на случай некорректного ввода.
static void handleApiSettingsTime() {
    if (!requireAuth()) return;
    AppSettings *s = callbacks_.getSettings();
    if (server.hasArg("gmtOffsetSec")) {
        long offset = server.arg("gmtOffsetSec").toInt();
        const long minOffset = -12L * 3600L;
        const long maxOffset =  14L * 3600L;
        if (offset < minOffset) offset = minOffset;
        if (offset > maxOffset) offset = maxOffset;
        s->time.gmtOffsetSec = (int32_t)offset;
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

    if (!Storage::verifyPassword(*s, current.c_str())) {
        server.send(403, "application/json", "{\"ok\":false,\"error\":\"Неверный текущий пароль\"}");
        return;
    }
    if (newPass.length() < 4) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Пароль слишком короткий\"}");
        return;
    }
    if (newPass.length() >= ADMIN_PASSWORD_MAX_LEN) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Пароль слишком длинный\"}");
        return;
    }

    Storage::setPassword(*s, newPass.c_str());
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

    // Явно просим сервер сохранять заголовок Authorization - без этого
    // server.header("Authorization") в requireAuth() всегда возвращал бы
    // пустую строку (сервер по умолчанию не хранит произвольные заголовки).
    static const char *authHeaderKeys[] = { "Authorization" };
    server.collectHeaders(authHeaderKeys, 1);

    server.on("/", HTTP_GET, handleIndex);
    server.on("/settings", HTTP_GET, handleSettingsPage);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/history", HTTP_GET, handleApiHistory);
    server.on("/api/settings", HTTP_GET, handleApiSettingsGet);
    server.on("/api/settings/pid", HTTP_POST, handleApiSettingsPid);
    server.on("/api/settings/servo", HTTP_POST, handleApiSettingsServo);
    server.on("/api/settings/battery", HTTP_POST, handleApiSettingsBattery);
    server.on("/api/settings/time", HTTP_POST, handleApiSettingsTime);
    server.on("/api/settings/network", HTTP_POST, handleApiSettingsNetwork);
    server.on("/api/settings/security", HTTP_POST, handleApiSettingsSecurity);
    server.on("/api/valve", HTTP_POST, handleApiValve);
    server.on("/api/servo/preview", HTTP_POST, handleApiServoPreview);
    server.on("/api/servo/calibrate", HTTP_POST, handleApiServoCalibrate);
    server.on("/api/servo/test", HTTP_POST, handleApiServoTest);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println(F("[Web] HTTP-сервер запущен на порту 80"));
}

void update() {
    server.handleClient();
}

} // namespace WebServerManager
