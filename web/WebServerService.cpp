// Scopo del file:
// implementa la dashboard web e le API HTTP del progetto.
#include "WebServerService.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace ccms {

namespace {
// Helper per serializzare interi signed a 64 bit in JSON costruito a mano.
void appendInt64(String& out, int64_t value) {
  char buf[24] = {0};
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  out += buf;
}

void appendUInt64(String& out, uint64_t value) {
  char buf[24] = {0};
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
  out += buf;
}

// Copia bounded da String Arduino a buffer C.
void copyBounded(const String& in, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  size_t n = in.length();
  if (n > outLen - 1) n = outLen - 1;
  memcpy(out, in.c_str(), n);
  out[n] = '\0';
}

bool hasHttpUrlPrefix(const char* value) {
  if (!value) return false;
  return strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0;
}

bool hasAnyTestConnectionArg(WebServer& server) {
  return server.hasArg("wifiSsid") ||
         server.hasArg("wifiPass") ||
         server.hasArg("serverUrl") ||
         server.hasArg("remoteEventUrl") ||
         server.hasArg("locationCode") ||
         server.hasArg("apiKey") ||
         server.hasArg("mqttBrokerHost") ||
         server.hasArg("mqttEnabled");
}

void trimBuffer(char* value) {
  if (!value) return;

  size_t len = strlen(value);
  size_t start = 0;
  while (start < len && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
    start++;
  }

  size_t end = len;
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
    end--;
  }

  if (start > 0 && end > start) {
    memmove(value, value + start, end - start);
  } else if (start >= end) {
    value[0] = '\0';
    return;
  }

  value[end - start] = '\0';
}

void normalizeHttpUrlBuffer(char* value, size_t outLen) {
  if (!value || outLen < 9) return;
  trimBuffer(value);
  if (value[0] == '\0' || hasHttpUrlPrefix(value)) return;

  const char* prefix = "https://";
  const size_t prefixLen = strlen(prefix);
  const size_t valueLen = strlen(value);
  if (prefixLen + valueLen >= outLen) {
    value[0] = '\0';
    return;
  }

  memmove(value + prefixLen, value, valueLen + 1);
  memcpy(value, prefix, prefixLen);
}

bool parseUnsignedLongStrict(const String& value, unsigned long& out) {
  if (value.length() == 0) return false;

  unsigned long parsed = 0;
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10UL + (unsigned long)(ch - '0');
  }

  out = parsed;
  return true;
}

bool extractJsonIntegerField(const String& body, const char* key, int64_t& outValue) {
  if (!key || key[0] == '\0') return false;

  const int keyPos = body.indexOf(key);
  if (keyPos < 0) return false;

  const int colonPos = body.indexOf(':', keyPos);
  if (colonPos < 0) return false;

  int startPos = colonPos + 1;
  while (startPos < body.length() &&
         (body[startPos] == ' ' || body[startPos] == '\t' ||
          body[startPos] == '\r' || body[startPos] == '\n')) {
    startPos++;
  }

  int endPos = startPos;
  if (endPos < body.length() && body[endPos] == '-') endPos++;
  while (endPos < body.length() && (body[endPos] >= '0' && body[endPos] <= '9')) {
    endPos++;
  }

  const String token = body.substring(startPos, endPos);
  if (token.length() == 0) return false;

  char buffer[32] = {0};
  copyBounded(token, buffer, sizeof(buffer));
  char* endPtr = nullptr;
  const long long parsed = strtoll(buffer, &endPtr, 10);
  if (endPtr == buffer || (endPtr && *endPtr != '\0')) return false;

  outValue = (int64_t)parsed;
  return true;
}
} // namespace

WebServerService::WebServerService(SystemStatus& status, WifiService& wifi, uint16_t port)
  : _status(status), _wifi(wifi), _server(port) {}

void WebServerService::setActions(ResetCountersCallback onResetCounters,
                                  SetCoinBaseCallback onSetCoinBase,
                                  SetBillRecyclerBaseCallback onSetBillRecyclerBase,
                                  SaveRemoteSnapshotCallback onSaveRemoteSnapshot,
                                  void* userData) {
  _onResetCounters = onResetCounters;
  _onSetCoinBase = onSetCoinBase;
  _onSetBillRecyclerBase = onSetBillRecyclerBase;
  _onSaveRemoteSnapshot = onSaveRemoteSnapshot;
  _actionsUserData = userData;
}

void WebServerService::setUiMode(UiMode mode) {
  _uiMode = mode;
}

void WebServerService::setEnterProgModeAction(EnterProgModeCallback cb, void* userData) {
  _onEnterProgMode = cb;
  _enterProgModeUserData = userData;
}

void WebServerService::setSettingsActions(GetSettingsCallback onGetSettings,
                                          GetPresentPeripheralCatalogCallback onGetPresentPeripheralCatalog,
                                          SaveSettingsCallback onSaveSettings,
                                          TestConnectionCallback onTestConnection,
                                          void* userData) {
  _onGetSettings = onGetSettings;
  _onGetPresentPeripheralCatalog = onGetPresentPeripheralCatalog;
  _onSaveSettings = onSaveSettings;
  _onTestConnection = onTestConnection;
  _settingsUserData = userData;
}

void WebServerService::begin() {
  if (_started) return;

  // Routing HTTP centralizzato: ogni endpoint e registrato una sola volta.
  _server.on("/", HTTP_GET, [this]() { handleRoot(); });
  _server.on("/status", HTTP_GET, [this]() { handleStatusPage(); });
  _server.on("/settings", HTTP_GET, [this]() { handleSettingsPage(); });
  _server.on("/health", HTTP_GET, [this]() { handleHealth(); });
  _server.on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
  _server.on("/api/logs", HTTP_GET, [this]() { handleApiLogs(); });
  _server.on("/api/wifi/networks", HTTP_GET, [this]() { handleApiWifiNetworks(); });
  _server.on("/api/settings", HTTP_GET, [this]() { handleApiGetSettings(); });
  _server.on("/api/settings", HTTP_POST, [this]() { handleApiSaveSettings(); });
  _server.on("/api/settings/testconnection", HTTP_POST, [this]() { handleApiTestConnection(); });
  _server.on("/api/counters/reset", HTTP_POST, [this]() { handleApiResetCounters(); });
  _server.on("/api/coins/base", HTTP_POST, [this]() { handleApiSetCoinBase(); });
  _server.on("/api/bills/recycler/base", HTTP_POST, [this]() { handleApiSetBillRecyclerBase(); });
  _server.on("/api/remote/change", HTTP_POST, [this]() { handleApiSaveRemoteSnapshot(); });
  _server.on("/api/mode/prog", HTTP_POST, [this]() { handleApiEnterProgMode(); });
  _server.onNotFound([this]() { _server.send(404, "text/plain", "Not found"); });

  _server.begin();
  _started = true;
}

void WebServerService::loop() {
  if (!_started) return;
  _server.handleClient();
}

void WebServerService::handleRoot() {
  // Root page dinamica in base alla modalita UI attiva.
  if (_uiMode != UI_MODE_PROG) {
    handleStatusPage();
    return;
  }

  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>CcTalkMultiSniffer PROG</title>
  <style>
    body { font-family: Consolas, Menlo, monospace; margin: 18px; background: #f5f7fb; color: #1f2933; }
    h1 { margin-top: 0; margin-bottom: 14px; }
    .card { background: #fff; border: 1px solid #d9e2ec; border-radius: 10px; padding: 14px; max-width: 540px; }
    .actions { display: flex; gap: 10px; flex-wrap: wrap; }
    a.button, button.button { display: inline-block; text-decoration: none; background: #1f6feb; color: #fff; padding: 10px 14px; border-radius: 8px; border: 0; cursor: pointer; }
    .note { margin-top: 14px; font-size: 13px; color: #486581; }
    .status { margin-top: 12px; font-size: 12px; color: #334e68; white-space: pre-wrap; }
    .wifi-indicator { display: inline-block; margin-bottom: 12px; background: #e9f2ff; border: 1px solid #bcd2f7; border-radius: 999px; padding: 8px 12px; font-size: 12px; }
    .section { margin-top: 14px; padding-top: 14px; border-top: 1px solid #d9e2ec; }
    .field-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 8px; }
    .field-grid label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: #334e68; }
    .field-grid input { width: 100%; font-family: Consolas, Menlo, monospace; font-size: 13px; padding: 8px; }
  </style>
</head>
<body>
  <div class="card">
    <div id="wifiIndicator" class="wifi-indicator">WiFi: loading...</div>
    <h1>Modalita PROG</h1>
    <div class="actions">
      <a class="button" href="/settings">Impostazioni</a>
      <a class="button" href="/status">Stato</a>
      <button id="btnTestConnection" class="button" type="button">Test connessione DB</button>
    </div>
    <div id="progStatus" class="status"></div>
    <div class="section">
      <div class="note">Valore manuale cassette banconote recycler.</div>
      <div class="field-grid">
        <label>
          Cassetta 10 EUR
          <input id="billCassette10Input" type="number" step="1" placeholder="0">
        </label>
        <label>
          Cassetta 20 EUR
          <input id="billCassette20Input" type="number" step="1" placeholder="0">
        </label>
        <label>
          Cassetta 50 EUR
          <input id="billCassette50Input" type="number" step="1" placeholder="0">
        </label>
      </div>
      <div class="actions" style="margin-top:10px;">
        <button id="btnSaveBillRecycler" class="button" type="button">Salva cassette banconote</button>
      </div>
      <div class="note">Inserisci manualmente il numero di banconote presenti nelle tre cassette recycler.</div>
    </div>
    <div class="note">AP locale attivo: 192.168.4.1</div>
    <div class="note">Il test usa le impostazioni salvate e prova prima il server remoto, poi il socket MySQL se configurato.</div>
  </div>
  <script>
    function s(v) { return (v === undefined || v === null) ? '' : String(v); }
    let billRecyclerDirty = false;
    function findRecyclerEntry(data) {
      const list = (data && data.cctalk && Array.isArray(data.cctalk.recycler)) ? data.cctalk.recycler : [];
      return list.length > 0 ? list[0] : null;
    }
    function setBillRecyclerInputs(entry) {
      if (billRecyclerDirty) return;
      const values = {
        billCassette10Input: String(entry ? (Number(entry.count10 || 0)) : 0),
        billCassette20Input: String(entry ? (Number(entry.count20 || 0)) : 0),
        billCassette50Input: String(entry ? (Number(entry.count50 || 0)) : 0)
      };
      Object.entries(values).forEach(([id, value]) => {
        const input = document.getElementById(id);
        if (input && document.activeElement !== input) input.value = value;
      });
    }

    let wifiIndicatorInFlight = false;
    async function refreshWifiIndicator() {
      if (wifiIndicatorInFlight) return;
      wifiIndicatorInFlight = true;
      try {
        const data = await fetch('/api/status', { cache: 'no-store' }).then(r => r.json());
        const ssid = s(data.wifi.ssid) || '-';
        const label = data.wifi.connected ? `WiFi connesso: ${ssid}` : `WiFi non connesso: ${ssid}`;
        document.getElementById('wifiIndicator').textContent = label;
        setBillRecyclerInputs(findRecyclerEntry(data));
      } catch (e) {
        document.getElementById('wifiIndicator').textContent = 'WiFi: stato non disponibile';
      } finally {
        wifiIndicatorInFlight = false;
      }
    }

    async function testConnection() {
      const status = document.getElementById('progStatus');
      status.textContent = 'Test connessione in corso...';
      try {
        const r = await fetch('/api/settings/testconnection', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: ''
        });
        const data = await r.json();
        status.textContent = data.ok ? `OK: ${s(data.message)}` : `Errore: ${s(data.message)}`;
        refreshWifiIndicator();
      } catch (e) {
        status.textContent = 'Errore rete durante il test di connessione';
      }
    }

    async function saveBillRecycler() {
      const status = document.getElementById('progStatus');
      const raw10 = Number(document.getElementById('billCassette10Input').value);
      const raw20 = Number(document.getElementById('billCassette20Input').value);
      const raw50 = Number(document.getElementById('billCassette50Input').value);
      if (!Number.isFinite(raw10) || raw10 < 0 || !Number.isFinite(raw20) || raw20 < 0 || !Number.isFinite(raw50) || raw50 < 0) {
        status.textContent = 'Valori cassette banconote non validi';
        return;
      }

      status.textContent = 'Salvataggio cassette banconote in corso...';
      try {
        const payload = {
          cassette10Count: Math.round(raw10),
          cassette20Count: Math.round(raw20),
          cassette50Count: Math.round(raw50)
        };
        const r = await fetch('/api/bills/recycler/base', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload)
        });
        const data = await r.json();
        status.textContent = data.ok ? `OK: ${s(data.message)}` : `Errore: ${s(data.message)}`;
        refreshWifiIndicator();
      } catch (e) {
        status.textContent = 'Errore rete durante il salvataggio cassette banconote';
      }
    }

    document.getElementById('btnTestConnection').addEventListener('click', testConnection);
    document.getElementById('btnSaveBillRecycler').addEventListener('click', saveBillRecycler);
    ['billCassette10Input', 'billCassette20Input', 'billCassette50Input'].forEach((id) => {
      document.getElementById(id).addEventListener('input', () => { billRecyclerDirty = true; });
    });
    refreshWifiIndicator();
    setInterval(refreshWifiIndicator, 5000);
  </script>
</body>
</html>
)HTML";
  _server.send(200, "text/html", PAGE);
}

void WebServerService::handleStatusPage() {
  // Pagina principale di monitoraggio: HTML statico con refresh via fetch JSON.
  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>CcTalkMultiSniffer Dashboard</title>
  <style>
    body { font-family: Consolas, Menlo, monospace; margin: 14px; background: #f5f7fb; color: #1f2933; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 10px; }
    .panel { background: #fff; border: 1px solid #d9e2ec; border-radius: 8px; padding: 10px; }
    .topbar { display: flex; gap: 10px; align-items: center; justify-content: space-between; flex-wrap: wrap; margin-bottom: 8px; }
    h1 { margin: 0 0 8px 0; font-size: 18px; }
    h2 { margin: 0 0 8px 0; font-size: 14px; }
    pre { margin: 0; white-space: pre-wrap; word-break: break-word; }
    .controls { display: flex; flex-direction: column; gap: 8px; align-items: flex-start; }
    .controls-row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; width: 100%; }
    .controls-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 8px; width: 100%; }
    .controls-grid label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: #334e68; }
    input, button { font-family: Consolas, Menlo, monospace; font-size: 13px; padding: 6px; }
    .status { margin-top: 8px; font-size: 12px; color: #334e68; }
    .wifi-indicator { display: inline-block; margin-bottom: 10px; background: #e9f2ff; border: 1px solid #bcd2f7; border-radius: 999px; padding: 8px 12px; font-size: 12px; }
    a.button { display: inline-block; text-decoration: none; background: #1f6feb; color: #fff; padding: 8px 12px; border-radius: 8px; }
  </style>
</head>
<body>
  <div id="wifiIndicator" class="wifi-indicator">WiFi: loading...</div>
  <div class="topbar">
    <h1>CcTalkMultiSniffer</h1>
    <!--PROG_MENU_BUTTON-->
  </div>
  <div class="grid">
    <div class="panel"><h2>WiFi</h2><pre id="wifi">loading...</pre></div>
    <div class="panel"><h2>MQTT</h2><pre id="mqttStatus">loading...</pre></div>
    <div class="panel"><h2>Data e ora</h2><pre id="clock">loading...</pre></div>
    <div class="panel"><h2>ccTalk Bus</h2><pre id="bus">loading...</pre></div>
    <div class="panel">
      <h2>Azioni</h2>
      <div class="controls">
        <div class="controls-row">
          <button id="btnReset" type="button">Azzera</button>
          <button id="btnSaveRemote" type="button">Salva su DB server</button>
        </div>
        <div class="controls-row">
          <input id="coinLevelInput" type="number" step="0.01" min="0" placeholder="Livello iniziale monete (EUR)">
          <button id="btnSetBase" type="button">Imposta livello iniziale + reset</button>
        </div>
        <div class="controls-grid">
          <label>
            Cassetta 10 EUR
            <input id="billCassette10Input" type="number" step="1" placeholder="0">
          </label>
          <label>
            Cassetta 20 EUR
            <input id="billCassette20Input" type="number" step="1" placeholder="0">
          </label>
          <label>
            Cassetta 50 EUR
            <input id="billCassette50Input" type="number" step="1" placeholder="0">
          </label>
        </div>
        <div class="controls-row">
          <button id="btnSaveBillRecycler" type="button">Salva cassette banconote</button>
        </div>
      </div>
      <div id="actionStatus" class="status"></div>
    </div>
    <div class="panel"><h2>Economics</h2><pre id="econ">loading...</pre></div>
    <div class="panel"><h2>Logs</h2><pre id="logs">loading...</pre></div>
  </div>
  <script>
    function s(v) { return (v === undefined || v === null) ? '' : String(v); }
    let billRecyclerDirty = false;
    function findRecyclerEntry(data) {
      const list = (data && data.cctalk && Array.isArray(data.cctalk.recycler)) ? data.cctalk.recycler : [];
      return list.length > 0 ? list[0] : null;
    }
    function setBillRecyclerInputs(entry) {
      if (billRecyclerDirty) return;
      const values = {
        billCassette10Input: String(entry ? (Number(entry.count10 || 0)) : 0),
        billCassette20Input: String(entry ? (Number(entry.count20 || 0)) : 0),
        billCassette50Input: String(entry ? (Number(entry.count50 || 0)) : 0)
      };
      Object.entries(values).forEach(([id, value]) => {
        const input = document.getElementById(id);
        if (input && document.activeElement !== input) input.value = value;
      });
    }
    function updateWifiIndicator(wifi) {
      const ssid = s(wifi.ssid) || '-';
      document.getElementById('wifiIndicator').textContent =
        wifi.connected ? `WiFi connesso: ${ssid}` : `WiFi non connesso: ${ssid}`;
    }

    let refreshInFlight = false;
    async function refresh() {
      if (refreshInFlight) return;
      refreshInFlight = true;
      try {
        const data = await fetch('/api/status', { cache: 'no-store' }).then(r => r.json());
        updateWifiIndicator(data.wifi || {});

        const wifi = [];
        wifi.push(`timestamp: ${s(data.timestamp)} ms`);
        wifi.push(`connected: ${data.wifi.connected ? 'yes' : 'no'}`);
        wifi.push(`status: ${s(data.wifi.status) || '-'}`);
        wifi.push(`ssid: ${s(data.wifi.ssid) || '-'}`);
        wifi.push(`apActive: ${data.wifi.apActive ? 'yes' : 'no'}`);
        wifi.push(`ip: ${s(data.wifi.ip) || '-'}`);
        wifi.push(`rssi: ${s(data.wifi.rssi)}`);
        document.getElementById('wifi').textContent = wifi.join('\n');

        const mqtt = [];
        const mqttConn = data.mqtt && data.mqtt.connected;
        mqtt.push(`connected: ${mqttConn ? 'yes' : 'no'}`);
        document.getElementById('mqttStatus').textContent = mqtt.join('\n');

        const clock = [];
        if (data.time && data.time.valid) {
          clock.push(`source: ${data.time.syncedFromInternet ? 'internet' : 'rtc'}`);
          clock.push(`local: ${s(data.time.display)}`);
          clock.push(`iso8601: ${s(data.time.iso8601)}`);
          clock.push(`unix: ${s(data.time.unix)}`);
        } else {
          clock.push('source: internet (attesa sync)');
          clock.push('local: non disponibile');
          clock.push('note: data/ora valida disponibile dopo sincronizzazione NTP');
        }
        document.getElementById('clock').textContent = clock.join('\n');

        const bus = [];
        bus.push(`devices: ${s(data.cctalk.detectedDevices)}`);
        bus.push(`txFrames: ${s(data.cctalk.txFrames)}`);
        bus.push(`rxFrames: ${s(data.cctalk.rxFrames)}`);
        bus.push(`transactions: ${s(data.cctalk.transactions)}`);
        bus.push(`snifferLoops: ${s(data.cctalk.snifferLoops)}`);
        bus.push(`snifferLastUs: ${s(data.cctalk.snifferLoopLastUs)}`);
        bus.push(`snifferMaxUs: ${s(data.cctalk.snifferLoopMaxUs)}`);
        bus.push(`snifferGapMaxUs: ${s(data.cctalk.snifferLoopGapMaxUs)}`);
        bus.push(`snifferOverBudget: ${s(data.cctalk.snifferOverBudgetLoops)}`);
        bus.push(`lastTx: ${s(data.cctalk.lastTx)}`);
        bus.push(`lastRx: ${s(data.cctalk.lastRx)}`);
        bus.push(`lastEvent: ${s(data.cctalk.lastEventDecoded)}`);
        document.getElementById('bus').textContent = bus.join('\n');

        const econ = [];
        econ.push(s(data.cctalk.formatted.line1));
        econ.push(s(data.cctalk.formatted.line2));
        econ.push(s(data.cctalk.formatted.line3));
        econ.push(`ValoreMoneteIniziale: ${(data.cctalk.economic.coinLevelBaseCents / 100.0).toFixed(2)} EUR`);
        econ.push(`ValoreMoneteAttuale: ${(data.cctalk.economic.coinCurrentCents / 100.0).toFixed(2)} EUR`);
        for (const r of (data.cctalk.recycler || [])) {
          econ.push(`BV[${r.addr}] RecyclerInventory: 10EUR=${r.count10} 20EUR=${r.count20} 50EUR=${r.count50}`);
        }
        document.getElementById('econ').textContent = econ.join('\n');
        setBillRecyclerInputs(findRecyclerEntry(data));

        const baseInput = document.getElementById('coinLevelInput');
        if (baseInput && document.activeElement !== baseInput) {
          baseInput.value = (data.cctalk.economic.coinLevelBaseCents / 100.0).toFixed(2);
        }

        document.getElementById('logs').textContent = (data.logs || []).join('\n');
      } catch (e) {
        document.getElementById('wifiIndicator').textContent = 'WiFi: stato non disponibile';
        document.getElementById('wifi').textContent = 'Errore fetch /api/status';
        document.getElementById('mqttStatus').textContent = 'Errore fetch /api/status';
        document.getElementById('clock').textContent = 'Errore fetch /api/status';
      } finally {
        refreshInFlight = false;
      }
    }

    refresh();
    setInterval(refresh, 5000);

    async function postJson(url, payload) {
      const r = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload || {})
      });
      return r.json();
    }

    document.getElementById('btnReset').addEventListener('click', async () => {
      const status = document.getElementById('actionStatus');
      status.textContent = 'Reset in corso...';
      try {
        const res = await postJson('/api/counters/reset', {});
        status.textContent = res.ok ? `OK: ${res.message}` : `Errore: ${res.message}`;
        refresh();
      } catch (e) {
        status.textContent = 'Errore rete durante reset';
      }
    });

    document.getElementById('btnSaveRemote').addEventListener('click', async () => {
      const status = document.getElementById('actionStatus');
      status.textContent = 'Salvataggio su DB server in corso...';
      try {
        const res = await postJson('/api/remote/change', {});
        status.textContent = res.ok ? `OK: ${res.message}` : `Errore: ${res.message}`;
        refresh();
      } catch (e) {
        status.textContent = 'Errore rete durante salvataggio su DB server';
      }
    });

    document.getElementById('btnSetBase').addEventListener('click', async () => {
      const status = document.getElementById('actionStatus');
      const raw = document.getElementById('coinLevelInput').value;
      const eur = Number(raw);
      if (!Number.isFinite(eur) || eur < 0) {
        status.textContent = 'Valore monete non valido';
        return;
      }
      const cents = Math.round(eur * 100);
      status.textContent = 'Salvataggio livello iniziale...';
      try {
        const res = await postJson('/api/coins/base', { coinLevelBaseCents: cents });
        status.textContent = res.ok ? `OK: ${res.message}` : `Errore: ${res.message}`;
        refresh();
      } catch (e) {
        status.textContent = 'Errore rete durante salvataggio';
      }
    });

    document.getElementById('btnSaveBillRecycler').addEventListener('click', async () => {
      const status = document.getElementById('actionStatus');
      const raw10 = Number(document.getElementById('billCassette10Input').value);
      const raw20 = Number(document.getElementById('billCassette20Input').value);
      const raw50 = Number(document.getElementById('billCassette50Input').value);
      if (!Number.isFinite(raw10) || raw10 < 0 || !Number.isFinite(raw20) || raw20 < 0 || !Number.isFinite(raw50) || raw50 < 0) {
        status.textContent = 'Valori cassette banconote non validi';
        return;
      }

      status.textContent = 'Salvataggio cassette banconote in corso...';
      try {
        const res = await postJson('/api/bills/recycler/base', {
          cassette10Count: Math.round(raw10),
          cassette20Count: Math.round(raw20),
          cassette50Count: Math.round(raw50)
        });
        if (res.ok) billRecyclerDirty = false;
        status.textContent = res.ok ? `OK: ${res.message}` : `Errore: ${res.message}`;
        refresh();
      } catch (e) {
        status.textContent = 'Errore rete durante salvataggio';
      }
    });

    ['billCassette10Input', 'billCassette20Input', 'billCassette50Input'].forEach((id) => {
      document.getElementById(id).addEventListener('input', () => { billRecyclerDirty = true; });
    });
  </script>
</body>
</html>
)HTML";

  String page(PAGE);
  if (_uiMode == UI_MODE_PROG) {
    page.replace("<!--PROG_MENU_BUTTON-->",
      "<a class=\"button\" href=\"/settings\">Impostazioni</a>"
      " <a class=\"button\" href=\"/\">Menu</a>");
  } else {
    page.replace("<!--PROG_MENU_BUTTON-->",
      "<button style=\"font-family:Consolas,Menlo,monospace;font-size:13px;"
      "padding:8px 12px;background:#d97706;color:#fff;border:0;border-radius:8px;cursor:pointer;\""
      " onclick=\"this.disabled=true;this.textContent='Riavvio...';"
      "fetch('/api/mode/prog',{method:'POST'})"
      ".catch(()=>{this.disabled=false;this.textContent='Modalita PROG';})\""
      ">Modalita PROG</button>");
  }
  _server.send(200, "text/html", page);
}

void WebServerService::handleSettingsPage() {
  // Le impostazioni sono esposte solo in modalita PROG per ridurre il rischio
  // di modifiche accidentali in esercizio normale.
  if (_uiMode != UI_MODE_PROG) {
    _server.send(403, "text/plain", "Settings disponibili solo in modalita PROG");
    return;
  }

  static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>CcTalkMultiSniffer Impostazioni</title>
  <style>
    * { box-sizing: border-box; }
    body { font-family: Consolas, Menlo, monospace; margin: 14px; background: #f5f7fb; color: #1f2933; }
    .panel { width: min(100%, 780px); max-width: 780px; background: #fff; border: 1px solid #d9e2ec; border-radius: 8px; padding: 12px; }
    .section { border-top: 1px solid #d9e2ec; padding-top: 14px; margin-top: 14px; }
    .section:first-of-type { border-top: 0; padding-top: 0; margin-top: 0; }
    .section-title { margin: 0 0 10px 0; font-size: 15px; }
    .section-note { margin: -4px 0 12px 0; font-size: 12px; color: #486581; }
    .row { display: grid; grid-template-columns: 210px 1fr; gap: 8px; margin-bottom: 8px; align-items: center; }
    .row-top { align-items: start; }
    label { font-size: 13px; }
    input, select, button, a { font-family: Consolas, Menlo, monospace; font-size: 13px; padding: 7px; }
    input[type="text"], input[type="password"], select { width: 100%; min-width: 0; max-width: 100%; }
    .actions { display: flex; gap: 8px; margin-top: 12px; }
    .status { margin-top: 12px; font-size: 12px; color: #334e68; }
    .wifi-tools { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .server-tools { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .secret-field { display: flex; gap: 8px; align-items: center; }
    .secret-field input { flex: 1 1 auto; min-width: 0; }
    .secret-toggle { white-space: nowrap; }
    .hint { font-size: 12px; color: #486581; }
    .checkbox { display: flex; align-items: center; gap: 8px; }
    .mask-group { display: flex; flex-direction: column; gap: 8px; }
    .mask-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 8px; }
    .mask-grid label { display: flex; align-items: center; gap: 8px; border: 1px solid #d9e2ec; border-radius: 8px; padding: 8px; background: #f8fbff; }
    .device-item { display: grid; grid-template-columns: minmax(0, 1fr) minmax(180px, 220px); gap: 8px; border: 1px solid #d9e2ec; border-radius: 8px; padding: 8px; background: #f8fbff; font-size: 13px; align-items: center; }
    .device-item.info { background: #fff7e6; border-color: #f7d08a; }
    .device-meta { display: flex; flex-direction: column; gap: 4px; }
    .device-title { font-weight: 600; }
    .device-note { font-size: 12px; color: #486581; }
    .device-list { margin: 0; padding-left: 18px; }
    .hidden { display: none; }
    .wifi-indicator { display: inline-block; margin-bottom: 12px; background: #e9f2ff; border: 1px solid #bcd2f7; border-radius: 999px; padding: 8px 12px; font-size: 12px; }
    .row > :nth-child(2),
    .device-item > :nth-child(2) { min-width: 0; }
    @media (max-width: 680px) {
      body { margin: 8px; }
      .panel { padding: 10px; }
      .row { grid-template-columns: 1fr; gap: 6px; }
      .device-item { grid-template-columns: 1fr; }
      .secret-field { flex-wrap: wrap; }
      .secret-field > * { width: 100%; }
      .secret-toggle { white-space: normal; }
      .wifi-tools, .server-tools, .actions { flex-direction: column; align-items: stretch; }
      .wifi-tools > *, .server-tools > *, .actions > * { width: 100%; }
      .checkbox { align-items: flex-start; }
      .mask-grid { grid-template-columns: 1fr; }
      .wifi-indicator { display: block; width: 100%; border-radius: 12px; }
    }
  </style>
</head>
<body>
  <div id="wifiIndicator" class="wifi-indicator">WiFi: loading...</div>
  <div class="panel">
    <h2>Impostazioni</h2>
    <div class="section">
      <h3 class="section-title">Connessione rete</h3>
      <div class="row row-top">
        <label for="wifiSsidSelect">Rete WiFi</label>
        <div>
          <div class="wifi-tools">
            <select id="wifiSsidSelect"></select>
            <button id="btnScanWifi" type="button">Aggiorna reti</button>
          </div>
          <input id="wifiSsidManual" class="hidden" type="text" placeholder="SSID rete nascosta">
          <div id="wifiScanStatus" class="hint"></div>
        </div>
      </div>
      <div class="row">
        <label for="wifiPass">WiFi Password</label>
        <div class="secret-field">
          <input id="wifiPass" type="password">
          <button id="toggleWifiPass" class="secret-toggle" type="button">Mostra</button>
        </div>
      </div>
      <div class="row">
        <label for="saveWifiCredentials">Salva credenziali WiFi</label>
        <label class="checkbox"><input id="saveWifiCredentials" type="checkbox">Memorizza SSID e password per la connessione automatica</label>
      </div>
    </div>

    <div class="section">
      <h3 class="section-title">Configurazione server</h3>
      <div class="section-note">Questa sezione e opzionale. Compilala solo se vuoi usare servizi remoti o database.</div>
      <div class="row"><label for="serverUrl">Server Web URL</label><input id="serverUrl" type="text" placeholder="https://example.com/api"></div>
      <div class="row"><label for="locationCode">Codice ubicazione</label><input id="locationCode" type="text" maxlength="14" placeholder="richiesto solo se usi il registro remoto"></div>
      <div class="row">
        <label for="apiKey">API Key</label>
        <div class="secret-field">
          <input id="apiKey" type="password" placeholder="opzionale ma consigliata">
          <button id="toggleApiKey" class="secret-toggle" type="button">Mostra</button>
        </div>
      </div>
      <h3 class="section-title">MQTT (EMQX)</h3>
      <div class="section-note">Abilita MQTT per ricevere comandi in push dal broker EMQX invece del polling HTTP.</div>
      <div class="row"><label><input id="mqttEnabled" type="checkbox"> Abilita MQTT</label></div>
      <div class="row"><label for="mqttBrokerHost">Broker host</label><input id="mqttBrokerHost" type="text" placeholder="xxxx.ala.eu-central-1.emqxsl.com"></div>
      <div class="row"><label for="mqttBrokerPort">Porta</label><input id="mqttBrokerPort" type="number" min="1" max="65535" value="1883"></div>
      <div class="row"><label for="mqttUsername">Username</label><input id="mqttUsername" type="text" placeholder="username EMQX"></div>
      <div class="row">
        <label for="mqttPassword">Password</label>
        <div class="secret-field">
          <input id="mqttPassword" type="password" placeholder="password EMQX">
          <button id="toggleMqttPassword" class="secret-toggle" type="button">Mostra</button>
        </div>
      </div>
      <div class="row row-top">
        <label>Test connessione</label>
        <div>
          <div class="server-tools">
            <button id="btnTestConnection" type="button">Test endpoint remoto</button>
          </div>
          <div class="hint">Usa i valori correnti del form; se necessario tenta prima la connessione WiFi e poi il controllo dell'endpoint remoto.</div>
        </div>
      </div>
    </div>

    <div class="section">
      <h3 class="section-title">Impostazioni periferiche</h3>
      <div class="row row-top">
        <label>Dispositivi rilevati</label>
        <div class="mask-group">
          <div id="detectedDevicesList" class="mask-group"></div>
          <div class="hint">Per ogni indirizzo ccTalk rilevato scegli il tipo di periferica tra quelli supportati dal firmware. Le nuove assegnazioni modello richiedono riavvio per essere applicate ai decoder.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Contatore monete IN</label>
        <div class="mask-group">
          <div id="coinInHopperMaskGroup" class="mask-grid"></div>
          <div id="coinAcceptorInfo" class="mask-grid"></div>
          <div class="hint">Elenco periferiche con indirizzo che devono contribuire a `CntotMoneteIn`. Lo stesso hopper non puo essere selezionato anche in OUT.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Contatore monete OUT</label>
        <div class="mask-group">
          <div id="coinOutHopperMaskGroup" class="mask-grid"></div>
          <div class="hint">Elenco periferiche con indirizzo che devono contribuire a `CntotMoneteOut`. Lo stesso hopper non puo essere selezionato anche in IN.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Valore hopper mono-moneta / unita base discriminatore</label>
        <div class="mask-group">
          <div id="hopperCoinValueGroup" class="mask-group"></div>
          <div class="hint">Per gli hopper mono-moneta imposta il taglio fisso. Per i discriminatori multi-taglio imposta il valore dell'unita base monetaria usata dal device per convertire i conteggi type1/type2 in valore economico.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Contatore banconote IN</label>
        <div class="mask-group">
          <div id="billInValidatorMaskGroup" class="mask-grid"></div>
          <div class="hint">Elenco periferiche con indirizzo che devono contribuire a `CntotBanconoteIN`.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Contatore banconote OUT</label>
        <div class="mask-group">
          <div id="billOutValidatorMaskGroup" class="mask-grid"></div>
          <div class="hint">Elenco periferiche con indirizzo che devono contribuire a `CntotBanconoteOUT`. La stessa periferica puo essere selezionata sia in IN sia in OUT.</div>
        </div>
      </div>
      <div class="row row-top">
        <label>Periferiche non utilizzate o sconosciute</label>
        <div class="mask-group">
          <ul id="unusedPeripheralList" class="device-list"></ul>
          <div class="hint">Elenco delle periferiche presenti sul bus ma non assegnate a contatori, oppure non riconosciute.</div>
        </div>
      </div>
    </div>
    <div class="actions">
      <button id="btnSave" type="button">Salva</button>
      <a href="/">Menu</a>
      <a href="/status">Stato</a>
    </div>
    <div id="status" class="status"></div>
  </div>
  <script>
    const MANUAL_WIFI_VALUE = '__manual__';
    let currentPeripheralCatalog = {
      presentCoinAcceptor: false,
      presentHopperMask: 0,
      presentBillValidatorMask: 0,
      unknownDevicesCsv: '',
      detectedDevices: [],
      coinAcceptorInEnabled: false,
      coinAcceptorFalconProfile: 1,
      hopperCoinValueCents: []
    };
    function s(v) { return (v === undefined || v === null) ? '' : String(v); }

    function wifiSelect() { return document.getElementById('wifiSsidSelect'); }
    function wifiManual() { return document.getElementById('wifiSsidManual'); }
    function wifiStatus() { return document.getElementById('wifiScanStatus'); }
    function wifiIndicator() { return document.getElementById('wifiIndicator'); }
    function detectedDeviceRows() { return Array.from(document.querySelectorAll('select[data-device-family]')); }

    function syncSecretToggle(inputId, buttonId) {
      const input = document.getElementById(inputId);
      const button = document.getElementById(buttonId);
      const visible = input && input.type === 'text';
      button.textContent = visible ? 'Nascondi' : 'Mostra';
    }

    function initSecretToggle(inputId, buttonId) {
      const input = document.getElementById(inputId);
      const button = document.getElementById(buttonId);
      if (!input || !button) return;
      syncSecretToggle(inputId, buttonId);
      button.addEventListener('click', () => {
        input.type = input.type === 'password' ? 'text' : 'password';
        syncSecretToggle(inputId, buttonId);
      });
    }

    let wifiIndicatorInFlight = false;
    async function refreshWifiIndicator() {
      if (wifiIndicatorInFlight) return;
      wifiIndicatorInFlight = true;
      try {
        const data = await fetch('/api/status', { cache: 'no-store' }).then(r => r.json());
        const ssid = s(data.wifi.ssid) || '-';
        wifiIndicator().textContent = data.wifi.connected
          ? `WiFi connesso: ${ssid}`
          : `WiFi non connesso: ${ssid}`;
      } catch (e) {
        wifiIndicator().textContent = 'WiFi: stato non disponibile';
      } finally {
        wifiIndicatorInFlight = false;
      }
    }

    function selectedWifiSsid() {
      const sel = wifiSelect();
      if (sel.value === MANUAL_WIFI_VALUE) {
        return wifiManual().value.trim();
      }
      return sel.value.trim();
    }

    function syncWifiManualVisibility() {
      const showManual = wifiSelect().value === MANUAL_WIFI_VALUE;
      wifiManual().classList.toggle('hidden', !showManual);
    }

    function ensureWifiOption(value, label) {
      const sel = wifiSelect();
      for (const opt of sel.options) {
        if (opt.value === value) return opt;
      }
      const opt = document.createElement('option');
      opt.value = value;
      opt.textContent = label;
      sel.appendChild(opt);
      return opt;
    }

    function setWifiSelection(ssid) {
      const value = (ssid || '').trim();
      if (!value) {
        wifiSelect().value = MANUAL_WIFI_VALUE;
        wifiManual().value = '';
        syncWifiManualVisibility();
        return;
      }

      let found = false;
      for (const opt of wifiSelect().options) {
        if (opt.value === value) {
          found = true;
          break;
        }
      }

      if (!found) {
        ensureWifiOption(value, `${value} (non rilevata ora)`);
      }
      wifiSelect().value = value;
      wifiManual().value = value;
      syncWifiManualVisibility();
    }

    function addressesFromMask(maskValue, baseAddr, count) {
      const mask = Number(maskValue) || 0;
      const out = [];
      for (let i = 0; i < count; i++) {
        if (((mask >>> i) & 1) === 1) out.push(baseAddr + i);
      }
      return out;
    }

    function csvToAddresses(csv) {
      const text = s(csv).trim();
      if (!text) return [];
      return text.split(',').map((part) => Number(part.trim())).filter((addr) => Number.isFinite(addr) && addr > 0);
    }

    function defaultDeviceLabel(device) {
      const addr = Number(device.addr) || 0;
      if (device.family === 'coin') return `Gettoniera indirizzo ${addr}`;
      if (device.family === 'hopper') return `Hopper indirizzo ${addr}`;
      if (device.family === 'bill_validator') return `Bill Validator indirizzo ${addr}`;
      return `Periferica indirizzo ${addr}`;
    }

    function deviceModelOptions(device) {
      if (!device || !device.supported) return [{ value: 0, label: 'Indirizzo non supportato' }];
      if (device.family === 'coin') {
        return [{ value: 1, label: 'Gettoniera NRI Falcon' }];
      }
      if (device.family === 'hopper') {
        return [
          { value: 0, label: 'Non assegnata' },
          { value: 1, label: 'Hopper AlbericiDiscriminator' },
          { value: 2, label: 'Hopper Alberici HopperCD' },
          { value: 3, label: 'Hopper Suzo Evolution' },
          { value: 4, label: 'Hopper Azkoyen Discriminator' }
        ];
      }
      if (device.family === 'bill_validator') {
        return [
          { value: 0, label: 'Non assegnata' },
          { value: 1, label: 'Bill Validator MD100' },
          { value: 2, label: 'Bill Validator SMART Payout' },
          { value: 3, label: 'Bill Validator IPRO' }
        ];
      }
      return [{ value: 0, label: 'Indirizzo non supportato' }];
    }

    function selectedModelForAddress(addr) {
      const input = document.querySelector(`select[data-device-addr="${addr}"]`);
      if (!input) return 0;
      return Number(input.value) || 0;
    }

    function selectedAddressesForFamily(family) {
      return detectedDeviceRows()
        .filter((input) => input.dataset.deviceFamily === family && (Number(input.value) || 0) > 0)
        .map((input) => Number(input.dataset.deviceAddr))
        .filter((addr) => Number.isFinite(addr));
    }

    function hopperCoinValueForAddr(values, addr) {
      const index = Number(addr) - 3;
      if (!Array.isArray(values) || index < 0 || index >= values.length) return 0;
      return Number(values[index]) || 0;
    }

    function buildModelMask(family, modelValue, baseAddr) {
      let mask = 0;
      detectedDeviceRows().forEach((input) => {
        if (input.dataset.deviceFamily !== family) return;
        if ((Number(input.value) || 0) !== modelValue) return;
        const addr = Number(input.dataset.deviceAddr);
        const bit = addr - baseAddr;
        if (bit >= 0) mask |= (1 << bit);
      });
      return String(mask >>> 0);
    }

    function renderMaskOptions(containerId, maskName, addresses, labelBuilder) {
      const container = document.getElementById(containerId);
      if (!container) return;
      container.innerHTML = '';
      if (!addresses || addresses.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'hint';
        empty.textContent = 'Nessuna periferica presente sul bus ccTalk';
        container.appendChild(empty);
        return;
      }
      for (const addr of addresses) {
        const label = document.createElement('label');
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.dataset.maskName = maskName;
        input.value = String(addr);
        label.appendChild(input);
        label.appendChild(document.createTextNode(labelBuilder(addr)));
        container.appendChild(label);
      }
    }

    function renderHopperCoinValueOptions(addresses, values) {
      const container = document.getElementById('hopperCoinValueGroup');
      if (!container) return;
      container.innerHTML = '';
      if (!addresses || addresses.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'hint';
        empty.textContent = 'Nessun hopper selezionato';
        container.appendChild(empty);
        return;
      }

      for (const addr of addresses) {
        const row = document.createElement('div');
        row.className = 'device-item';

        const meta = document.createElement('div');
        meta.className = 'device-meta';
        const title = document.createElement('div');
        title.className = 'device-title';
        title.textContent = `Hopper indirizzo ${addr}`;
        meta.appendChild(title);
        const note = document.createElement('div');
        note.className = 'device-note';
        note.textContent = 'Taglio moneta usato dal contatore su comandi A7/A6';
        meta.appendChild(note);
        row.appendChild(meta);

        const select = document.createElement('select');
        select.dataset.hopperCoinValueAddr = String(addr);
        const options = [
          { value: 0, label: 'Non impostato' },
          { value: 100, label: '1.00 EUR' },
          { value: 200, label: '2.00 EUR' }
        ];
        for (const option of options) {
          const opt = document.createElement('option');
          opt.value = String(option.value);
          opt.textContent = option.label;
          select.appendChild(opt);
        }
        select.value = String(hopperCoinValueForAddr(values, addr));
        row.appendChild(select);
        container.appendChild(row);
      }
    }

    function renderCoinAcceptorInfo(present, enabled, falconProfile) {
      const container = document.getElementById('coinAcceptorInfo');
      if (!container) return;
      container.innerHTML = '';
      if (!present) return;

      const enabledLabel = document.createElement('label');
      const enabledInput = document.createElement('input');
      enabledInput.type = 'checkbox';
      enabledInput.id = 'coinAcceptorInEnabled';
      enabledInput.checked = !!enabled;
      enabledLabel.appendChild(enabledInput);
      enabledLabel.appendChild(document.createTextNode('Gettoniera indirizzo 2'));
      container.appendChild(enabledLabel);

      const profileRow = document.createElement('div');
      profileRow.className = 'device-item info';
      const meta = document.createElement('div');
      meta.className = 'device-meta';
      const title = document.createElement('div');
      title.className = 'device-title';
      title.textContent = 'Profilo valori NRI Falcon';
      meta.appendChild(title);
      const note = document.createElement('div');
      note.className = 'device-note';
      note.textContent = 'Seleziona il blocco etichetta corrispondente alla programmazione della gettoniera';
      meta.appendChild(note);
      profileRow.appendChild(meta);

      const select = document.createElement('select');
      select.id = 'coinAcceptorFalconProfile';
      const options = [
        { value: 1, label: 'Block 0: 0.05 / 0.10 / 0.20 / 0.50 / 1 / 2 / 0.50 EUR' },
        { value: 2, label: 'Block 1: 0.50 / 1 / 2 / 0.50 EUR' }
      ];
      for (const option of options) {
        const opt = document.createElement('option');
        opt.value = String(option.value);
        opt.textContent = option.label;
        select.appendChild(opt);
      }
      select.value = String(Number(falconProfile) || 1);
      profileRow.appendChild(select);
      container.appendChild(profileRow);
    }

    function renderDetectedDevices(devices) {
      const container = document.getElementById('detectedDevicesList');
      if (!container) return;
      container.innerHTML = '';

      if (!devices || devices.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'hint';
        empty.textContent = 'Nessuna periferica ccTalk rilevata sul bus';
        container.appendChild(empty);
        return;
      }

      devices
        .slice()
        .sort((a, b) => (Number(a.addr) || 0) - (Number(b.addr) || 0))
        .forEach((device) => {
          const row = document.createElement('div');
          row.className = `device-item${device.supported ? '' : ' info'}`;

          const meta = document.createElement('div');
          meta.className = 'device-meta';
          const title = document.createElement('div');
          title.className = 'device-title';
          title.textContent = defaultDeviceLabel(device);
          meta.appendChild(title);

          const note = document.createElement('div');
          note.className = 'device-note';
          note.textContent = device.supported
            ? 'Seleziona il modello parser associato a questo indirizzo'
            : 'Indirizzo rilevato ma non gestito dai parser del firmware';
          meta.appendChild(note);
          row.appendChild(meta);

          const select = document.createElement('select');
          select.dataset.deviceAddr = String(device.addr);
          select.dataset.deviceFamily = s(device.family);
          const options = deviceModelOptions(device);
          for (const option of options) {
            const opt = document.createElement('option');
            opt.value = String(option.value);
            opt.textContent = option.label;
            select.appendChild(opt);
          }
          select.value = String(device.selectedModel || 0);
          if (options.length === 1 && !device.supported) select.disabled = true;
          if (device.family === 'coin') select.disabled = true;
          row.appendChild(select);
          container.appendChild(row);
        });
    }

    function setMaskSelection(maskName, maskValue, baseAddr) {
      const numericMask = Number(maskValue) || 0;
      document.querySelectorAll(`input[data-mask-name="${maskName}"]`).forEach((input) => {
        const addr = Number(input.value);
        const bit = addr - baseAddr;
        input.checked = bit >= 0 && (((numericMask >>> bit) & 1) === 1);
      });
    }

    function getMaskSelection(maskName, baseAddr) {
      let mask = 0;
      document.querySelectorAll(`input[data-mask-name="${maskName}"]`).forEach((input) => {
        if (!input.checked) return;
        const addr = Number(input.value);
        const bit = addr - baseAddr;
        if (bit >= 0) mask |= (1 << bit);
      });
      return String(mask >>> 0);
    }

    function syncExclusiveHopperSelection(target) {
      if (!target || !target.dataset || !target.checked) return;
      if (target.dataset.maskName !== 'coinInHopperMask' && target.dataset.maskName !== 'coinOutHopperMask') return;

      const otherMaskName = target.dataset.maskName === 'coinInHopperMask'
        ? 'coinOutHopperMask'
        : 'coinInHopperMask';
      const other = document.querySelector(`input[data-mask-name="${otherMaskName}"][value="${target.value}"]`);
      if (other) other.checked = false;
    }

    function refreshUnusedPeripheralList(settings) {
      const list = document.getElementById('unusedPeripheralList');
      if (!list) return;
      list.innerHTML = '';

      const selectedHopperAddrs = selectedAddressesForFamily('hopper');
      const selectedBillValidatorAddrs = selectedAddressesForFamily('bill_validator');
      const unknownAddrs = csvToAddresses(settings.unknownDevicesCsv);
      const unused = [];

      if (settings.detectedDevices.some((device) => Number(device.addr) === 2) &&
          !document.getElementById('coinAcceptorInEnabled')?.checked) {
        unused.push('Gettoniera indirizzo 2 (presente ma non assegnata)');
      }

      for (const addr of selectedHopperAddrs) {
        const inUse = document.querySelector(`input[data-mask-name="coinInHopperMask"][value="${addr}"]`)?.checked;
        const outUse = document.querySelector(`input[data-mask-name="coinOutHopperMask"][value="${addr}"]`)?.checked;
        if (!inUse && !outUse) unused.push(`Hopper indirizzo ${addr} (presente ma non assegnato)`);
      }

      for (const addr of selectedBillValidatorAddrs) {
        const inUse = document.querySelector(`input[data-mask-name="billInValidatorMask"][value="${addr}"]`)?.checked;
        const outUse = document.querySelector(`input[data-mask-name="billOutValidatorMask"][value="${addr}"]`)?.checked;
        if (!inUse && !outUse) unused.push(`Bill Validator indirizzo ${addr} (presente ma non assegnato)`);
      }

      for (const device of (settings.detectedDevices || [])) {
        if (!device.supported) continue;
        if ((Number(device.selectedModel) || 0) > 0) continue;
        unused.push(`${defaultDeviceLabel(device)} (presente ma tipo non assegnato)`);
      }

      for (const addr of unknownAddrs) {
        unused.push(`Periferica sconosciuta indirizzo ${addr}`);
      }

      if (unused.length === 0) {
        const li = document.createElement('li');
        li.textContent = 'Nessuna periferica non utilizzata o sconosciuta';
        list.appendChild(li);
        return;
      }

      for (const itemText of unused) {
        const li = document.createElement('li');
        li.textContent = itemText;
        list.appendChild(li);
      }
    }

    function collectHopperCoinValueCents() {
      const out = new Array(8).fill(0);
      document.querySelectorAll('select[data-hopper-coin-value-addr]').forEach((input) => {
        const addr = Number(input.dataset.hopperCoinValueAddr);
        const index = addr - 3;
        if (index < 0 || index >= out.length) return;
        out[index] = Number(input.value) || 0;
      });
      return out;
    }

    function refreshRoutingAssignments(settings) {
      const hasCoinInInputs = document.querySelectorAll('input[data-mask-name="coinInHopperMask"]').length > 0;
      const hasCoinOutInputs = document.querySelectorAll('input[data-mask-name="coinOutHopperMask"]').length > 0;
      const hasBillInInputs = document.querySelectorAll('input[data-mask-name="billInValidatorMask"]').length > 0;
      const hasBillOutInputs = document.querySelectorAll('input[data-mask-name="billOutValidatorMask"]').length > 0;
      const hasHopperCoinValueInputs = document.querySelectorAll('select[data-hopper-coin-value-addr]').length > 0;
      const currentSelections = {
        coinInHopperMask: getMaskSelection('coinInHopperMask', 3),
        coinOutHopperMask: getMaskSelection('coinOutHopperMask', 3),
        billInValidatorMask: getMaskSelection('billInValidatorMask', 40),
        billOutValidatorMask: getMaskSelection('billOutValidatorMask', 40),
        hopperCoinValueCents: collectHopperCoinValueCents()
      };
      const coinCheckbox = document.getElementById('coinAcceptorInEnabled');
      const coinEnabled = coinCheckbox ? coinCheckbox.checked : !!settings.coinAcceptorInEnabled;
      const coinProfileSelect = document.getElementById('coinAcceptorFalconProfile');
      const coinProfile = coinProfileSelect
        ? (Number(coinProfileSelect.value) || 1)
        : (Number(settings.coinAcceptorFalconProfile) || 1);

      const hopperAddrs = selectedAddressesForFamily('hopper');
      const billValidatorAddrs = selectedAddressesForFamily('bill_validator');
      const hasCoin = selectedModelForAddress(2) > 0;

      renderMaskOptions('coinInHopperMaskGroup', 'coinInHopperMask', hopperAddrs, (addr) => `Hopper indirizzo ${addr}`);
      renderMaskOptions('coinOutHopperMaskGroup', 'coinOutHopperMask', hopperAddrs, (addr) => `Hopper indirizzo ${addr}`);
      renderHopperCoinValueOptions(
        hopperAddrs,
        hasHopperCoinValueInputs ? currentSelections.hopperCoinValueCents : settings.hopperCoinValueCents);
      renderMaskOptions('billInValidatorMaskGroup', 'billInValidatorMask', billValidatorAddrs, (addr) => `Bill Validator indirizzo ${addr}`);
      renderMaskOptions('billOutValidatorMaskGroup', 'billOutValidatorMask', billValidatorAddrs, (addr) => `Bill Validator indirizzo ${addr}`);
      renderCoinAcceptorInfo(hasCoin, coinEnabled, coinProfile);

      setMaskSelection('coinInHopperMask', hasCoinInInputs ? currentSelections.coinInHopperMask : settings.coinInHopperMask, 3);
      setMaskSelection('coinOutHopperMask', hasCoinOutInputs ? currentSelections.coinOutHopperMask : settings.coinOutHopperMask, 3);
      setMaskSelection('billInValidatorMask', hasBillInInputs ? currentSelections.billInValidatorMask : settings.billInValidatorMask, 40);
      setMaskSelection('billOutValidatorMask', hasBillOutInputs ? currentSelections.billOutValidatorMask : settings.billOutValidatorMask, 40);

      currentPeripheralCatalog.coinAcceptorInEnabled = hasCoin
        ? !!document.getElementById('coinAcceptorInEnabled')?.checked
        : false;
      currentPeripheralCatalog.coinAcceptorFalconProfile = hasCoin
        ? (Number(document.getElementById('coinAcceptorFalconProfile')?.value) || 1)
        : (Number(settings.coinAcceptorFalconProfile) || 1);
      currentPeripheralCatalog.hopperCoinValueCents = collectHopperCoinValueCents();
      refreshUnusedPeripheralList(currentPeripheralCatalog);
    }

    async function loadWifiNetworks(selectedSsid) {
      wifiStatus().textContent = 'Scansione reti in corso...';
      try {
        const r = await fetch('/api/wifi/networks', { cache: 'no-store' });
        const data = await r.json();
        const sel = wifiSelect();
        sel.innerHTML = '';

        const networks = data.networks || [];
        for (const net of networks) {
          const opt = document.createElement('option');
          opt.value = net.ssid || '';
          const mode = net.secure ? 'secure' : 'open';
          opt.textContent = `${net.ssid} (${net.rssi} dBm, ${mode})`;
          sel.appendChild(opt);
        }

        const manual = document.createElement('option');
        manual.value = MANUAL_WIFI_VALUE;
        manual.textContent = 'Rete nascosta / inserimento manuale';
        sel.appendChild(manual);

        setWifiSelection(selectedSsid || data.connectedSsid || '');
        const count = networks.length;
        wifiStatus().textContent = count > 0 ? `Reti trovate: ${count}` : 'Nessuna rete rilevata';
      } catch (e) {
        wifiStatus().textContent = 'Errore durante la scansione WiFi';
        ensureWifiOption(MANUAL_WIFI_VALUE, 'Rete nascosta / inserimento manuale');
        setWifiSelection(selectedSsid || '');
      }
    }

    async function loadSettings() {
      const r = await fetch('/api/settings', { cache: 'no-store' });
      const data = await r.json();
      const s = data.settings || {};
      currentPeripheralCatalog = {
        presentCoinAcceptor: !!s.presentCoinAcceptor,
        coinAcceptorInEnabled: !!s.coinAcceptorInEnabled,
        coinAcceptorFalconProfile: Number(s.coinAcceptorFalconProfile) || 1,
        presentHopperMask: Number(s.presentHopperMask) || 0,
        presentBillValidatorMask: Number(s.presentBillValidatorMask) || 0,
        unknownDevicesCsv: s.unknownDevicesCsv || '',
        detectedDevices: Array.isArray(s.detectedDevices) ? s.detectedDevices : [],
        hopperCoinValueCents: Array.isArray(s.hopperCoinValueCents) ? s.hopperCoinValueCents : new Array(8).fill(0)
      };
      renderDetectedDevices(currentPeripheralCatalog.detectedDevices);
      document.getElementById('wifiPass').value = s.wifiPass || '';
      document.getElementById('saveWifiCredentials').checked = !!s.saveWifiCredentials;
      document.getElementById('serverUrl').value = s.serverUrl || '';
      document.getElementById('locationCode').value = s.locationCode || '';
      document.getElementById('apiKey').value = s.apiKey || '';
      document.getElementById('mqttEnabled').checked = !!s.mqttEnabled;
      document.getElementById('mqttBrokerHost').value = s.mqttBrokerHost || '';
      document.getElementById('mqttBrokerPort').value = s.mqttBrokerPort || 1883;
      document.getElementById('mqttUsername').value = s.mqttUsername || '';
      document.getElementById('mqttPassword').value = s.mqttPassword || '';
      refreshRoutingAssignments(s);
      document.getElementById('status').textContent = data.ok ? 'Impostazioni caricate' : ('Errore: ' + (data.message || 'lettura'));
      await loadWifiNetworks(s.wifiSsid || '');
    }

    function buildSettingsParams() {
      const params = new URLSearchParams();
      const hopperModel1Mask = buildModelMask('hopper', 1, 3);
      const hopperModel2Mask = buildModelMask('hopper', 2, 3);
      const hopperModel3Mask = buildModelMask('hopper', 3, 3);
      const hopperModel4Mask = buildModelMask('hopper', 4, 3);
      const billValidatorModel1Mask = buildModelMask('bill_validator', 1, 40);
      const billValidatorModel2Mask = buildModelMask('bill_validator', 2, 40);
      const billValidatorModel3Mask = buildModelMask('bill_validator', 3, 40);
      params.set('wifiSsid', selectedWifiSsid());
      params.set('wifiPass', document.getElementById('wifiPass').value);
      params.set('saveWifiCredentials', document.getElementById('saveWifiCredentials').checked ? '1' : '0');
      params.set('serverUrl', document.getElementById('serverUrl').value);
      params.set('locationCode', document.getElementById('locationCode').value);
      params.set('apiKey', document.getElementById('apiKey').value);
      params.set('mqttEnabled', document.getElementById('mqttEnabled').checked ? '1' : '0');
      params.set('mqttBrokerHost', document.getElementById('mqttBrokerHost').value);
      params.set('mqttBrokerPort', document.getElementById('mqttBrokerPort').value);
      params.set('mqttUsername', document.getElementById('mqttUsername').value);
      params.set('mqttPassword', document.getElementById('mqttPassword').value);
      params.set('hopperAlbericiDiscriminatorMask', hopperModel1Mask);
      params.set('hopperAlbericiHopperCdMask', hopperModel2Mask);
      params.set('hopperSuzoEvolutionMask', hopperModel3Mask);
      params.set('hopperAzkoyenDiscriminatorMask', hopperModel4Mask);
      params.set('billValidatorMd100Mask', billValidatorModel1Mask);
      params.set('billValidatorSmartPayoutMask', billValidatorModel2Mask);
      params.set('billValidatorIproMask', billValidatorModel3Mask);
      params.set('hopperModel',
        hopperModel4Mask !== '0' && hopperModel1Mask === '0' && hopperModel2Mask === '0' && hopperModel3Mask === '0'
          ? '4'
          : (hopperModel3Mask !== '0' && hopperModel1Mask === '0' && hopperModel2Mask === '0'
            ? '3'
            : (hopperModel2Mask !== '0' && hopperModel1Mask === '0' ? '2' : '1')));
      params.set('billValidatorModel',
        billValidatorModel3Mask !== '0' && billValidatorModel1Mask === '0' && billValidatorModel2Mask === '0'
          ? '3'
          : (billValidatorModel2Mask !== '0' && billValidatorModel1Mask === '0' ? '2' : '1'));
      const coinAcceptorCheckbox = document.getElementById('coinAcceptorInEnabled');
      params.set('coinAcceptorInEnabled',
        (coinAcceptorCheckbox ? coinAcceptorCheckbox.checked : !!currentPeripheralCatalog.coinAcceptorInEnabled) ? '1' : '0');
      const coinAcceptorProfileSelect = document.getElementById('coinAcceptorFalconProfile');
      params.set('coinAcceptorFalconProfile',
        String(coinAcceptorProfileSelect
          ? (Number(coinAcceptorProfileSelect.value) || 1)
          : (Number(currentPeripheralCatalog.coinAcceptorFalconProfile) || 1)));
      params.set('coinInHopperMask', getMaskSelection('coinInHopperMask', 3));
      params.set('coinOutHopperMask', getMaskSelection('coinOutHopperMask', 3));
      const hopperCoinValues = collectHopperCoinValueCents();
      for (let addr = 3; addr <= 10; addr++) {
        const index = addr - 3;
        params.set(`hopperCoinValueCents${addr}`, String(Number(hopperCoinValues[index]) || 0));
      }
      params.set('billInValidatorMask', getMaskSelection('billInValidatorMask', 40));
      params.set('billOutValidatorMask', getMaskSelection('billOutValidatorMask', 40));
      return params;
    }

    async function saveSettings() {
      const params = buildSettingsParams();
      const r = await fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params.toString()
      });
      const data = await r.json();
      document.getElementById('status').textContent = data.ok ? ('OK: ' + data.message) : ('Errore: ' + data.message);
    }

    async function testConnection() {
      const status = document.getElementById('status');
      status.textContent = 'Test connessione in corso...';
      const params = buildSettingsParams();
      const r = await fetch('/api/settings/testconnection', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params.toString()
      });
      const data = await r.json();
      status.textContent = data.ok ? ('OK: ' + data.message) : ('Errore: ' + data.message);
      await refreshWifiIndicator();
    }

    document.getElementById('btnSave').addEventListener('click', () => {
      saveSettings().catch(() => {
        document.getElementById('status').textContent = 'Errore rete durante salvataggio';
      });
    });

    document.getElementById('btnScanWifi').addEventListener('click', () => {
      loadWifiNetworks(selectedWifiSsid()).catch(() => {
        wifiStatus().textContent = 'Errore durante la scansione WiFi';
      });
    });
    document.getElementById('btnTestConnection').addEventListener('click', () => {
      testConnection().catch(() => {
        document.getElementById('status').textContent = 'Errore rete durante il test di connessione';
      });
    });

    wifiSelect().addEventListener('change', syncWifiManualVisibility);
    document.addEventListener('change', (event) => {
      const target = event.target;
      if (!target) return;
      if (target.dataset && target.dataset.deviceFamily) {
        const addr = Number(target.dataset.deviceAddr);
        currentPeripheralCatalog.detectedDevices = currentPeripheralCatalog.detectedDevices.map((device) =>
          Number(device.addr) === addr
            ? { ...device, selectedModel: Number(target.value) || 0 }
            : device);
        refreshRoutingAssignments(currentPeripheralCatalog);
        return;
      }
      if (target.id === 'coinAcceptorInEnabled') {
        refreshUnusedPeripheralList(currentPeripheralCatalog);
        return;
      }
      if (!target.dataset || !target.dataset.maskName) return;
      syncExclusiveHopperSelection(target);
      refreshUnusedPeripheralList(currentPeripheralCatalog);
    });
    renderMaskOptions('coinInHopperMaskGroup', 'coinInHopperMask', [], (addr) => `Hopper indirizzo ${addr}`);
    renderMaskOptions('coinOutHopperMaskGroup', 'coinOutHopperMask', [], (addr) => `Hopper indirizzo ${addr}`);
    renderHopperCoinValueOptions([], []);
    renderMaskOptions('billInValidatorMaskGroup', 'billInValidatorMask', [], (addr) => `Bill Validator indirizzo ${addr}`);
    renderMaskOptions('billOutValidatorMaskGroup', 'billOutValidatorMask', [], (addr) => `Bill Validator indirizzo ${addr}`);
    renderDetectedDevices([]);
    renderCoinAcceptorInfo(false, false);
    refreshUnusedPeripheralList(currentPeripheralCatalog);
    ensureWifiOption(MANUAL_WIFI_VALUE, 'Rete nascosta / inserimento manuale');
    syncWifiManualVisibility();
    initSecretToggle('wifiPass', 'toggleWifiPass');
    initSecretToggle('apiKey', 'toggleApiKey');
    initSecretToggle('mqttPassword', 'toggleMqttPassword');

    loadSettings().catch(() => {
      document.getElementById('status').textContent = 'Errore rete durante caricamento';
    });
    refreshWifiIndicator();
    setInterval(refreshWifiIndicator, 5000);
  </script>
</body>
</html>
)HTML";

  _server.send(200, "text/html", PAGE);
}

void WebServerService::handleHealth() {
  _server.send(200, "text/plain", "ok");
}

void WebServerService::appendLogsArrayJson(String& out, uint16_t limit) {
  // Estrae dal ring log solo il sottoinsieme richiesto, dal piu vecchio al piu nuovo.
  out += "[";

  const uint8_t total = _status.logCount();
  uint16_t capped = limit;
  if (capped == 0) capped = total;
  if (capped > total) capped = total;

  const uint8_t start = (uint8_t)(total - capped);
  char line[RingLog::kLineSize] = {0};
  for (uint8_t i = start; i < total; i++) {
    if (!_status.logLineAt(i, line, sizeof(line))) continue;
    if (out[out.length() - 1] != '[') out += ",";
    out += "\"";
    appendJsonEscaped(out, line);
    out += "\"";
  }

  out += "]";
}

void WebServerService::handleApiStatus() {
  // Costruisce un JSON completo di stato senza dipendere da librerie JSON esterne.
  const SystemStatus::EconomicFields econ = _status.economicCopy();
  const SystemStatus::CcTalkFields cc = _status.cctalkCopy();
  WifiService::ClockInfo clock;
  const bool hasClock = _wifi.getClockInfo(clock);

  char ip[24] = {0};
  _wifi.ipToString(ip, sizeof(ip));

  char line1[SystemStatus::kFormattedLineSize] = {0};
  char line2[SystemStatus::kFormattedLineSize] = {0};
  char line3[SystemStatus::kFormattedLineSize] = {0};
  _status.formatEconomicLine1(line1, sizeof(line1));
  _status.formatEconomicLine2(line2, sizeof(line2));
  _status.formatEconomicLine3(line3, sizeof(line3));

  String out;
  out.reserve(4096);
  out += "{";

  out += "\"timestamp\":";
  out += String((unsigned long)millis());
  out += ",";

  out += "\"time\":{";
  out += "\"valid\":";
  out += (hasClock ? "true" : "false");
  out += ",\"syncedFromInternet\":";
  out += (clock.syncedFromInternet ? "true" : "false");
  out += ",\"display\":\"";
  appendJsonEscaped(out, hasClock ? clock.display : "");
  out += "\",\"iso8601\":\"";
  appendJsonEscaped(out, hasClock ? clock.iso8601 : "");
  out += "\",\"unix\":";
  appendUInt64(out, hasClock ? clock.unixTime : 0);
  out += "},";

  out += "\"wifi\":{";
  out += "\"connected\":";
  out += (_wifi.isConnected() ? "true" : "false");
  out += ",\"status\":\"";
  appendJsonEscaped(out, _wifi.statusText());
  out += "\",\"ssid\":\"";
  appendJsonEscaped(out, _wifi.connectedSsid().c_str());
  out += "\",\"apActive\":";
  out += (_wifi.isApFallbackActive() ? "true" : "false");
  out += ",\"ip\":\"";
  appendJsonEscaped(out, ip);
  out += "\",\"rssi\":";
  out += String(_wifi.rssi());
  out += "},";

  out += "\"mqtt\":{";
  out += "\"connected\":";
  out += (_status.mqttConnected() ? "true" : "false");
  out += "},";

  out += "\"cctalk\":{";
  out += "\"detectedDevices\":";
  out += String((unsigned)cc.detectedDevices);
  out += ",\"txFrames\":";
  out += String((unsigned long)cc.txFrames);
  out += ",\"rxFrames\":";
  out += String((unsigned long)cc.rxFrames);
  out += ",\"transactions\":";
  out += String((unsigned long)cc.transactions);
  out += ",\"snifferLoops\":";
  out += String((unsigned long)cc.snifferLoops);
  out += ",\"snifferLoopLastUs\":";
  out += String((unsigned long)cc.snifferLoopLastUs);
  out += ",\"snifferLoopMaxUs\":";
  out += String((unsigned long)cc.snifferLoopMaxUs);
  out += ",\"snifferLoopGapMaxUs\":";
  out += String((unsigned long)cc.snifferLoopGapMaxUs);
  out += ",\"snifferOverBudgetLoops\":";
  out += String((unsigned long)cc.snifferOverBudgetLoops);
  out += ",\"lastTx\":\"";
  appendJsonEscaped(out, cc.lastTxFrame);
  out += "\",\"lastRx\":\"";
  appendJsonEscaped(out, cc.lastRxFrame);
  out += "\",\"lastEventDecoded\":\"";
  appendJsonEscaped(out, cc.lastEventDecoded);
  out += "\",";

  out += "\"economic\":{";
  out += "\"cntotBanconoteInCents\":";
  out += String((unsigned long)econ.cntotBanconoteInCents);
  out += ",\"cntotMoneteOutCents\":";
  out += String((unsigned long)econ.cntotMoneteOutCents);
  out += ",\"cntotMoneteInCents\":";
  out += String((unsigned long)econ.cntotMoneteInCents);
  out += ",\"cntotBanconoteOutCents\":";
  out += String((unsigned long)econ.cntotBanconoteOutCents);
  out += ",\"cntotBanconoteCents\":";
  appendInt64(out, econ.cntotBanconoteCents);
  out += ",\"cntotMoneteCents\":";
  appendInt64(out, econ.cntotMoneteCents);
  out += ",\"saldoCents\":";
  appendInt64(out, econ.saldoCents);
  out += ",\"cassaCents\":";
  out += String((unsigned long)econ.cassaCents);
  out += ",\"recyclerInventoryTotaleCents\":";
  out += String((unsigned long)econ.recyclerInventoryTotaleCents);
  out += ",\"coinLevelBaseCents\":";
  out += String((unsigned long)econ.coinLevelBaseCents);
  out += ",\"coinCurrentCents\":";
  appendInt64(out, econ.coinCurrentCents);
  out += "},";

  out += "\"formatted\":{";
  out += "\"line1\":\"";
  appendJsonEscaped(out, line1);
  out += "\",\"line2\":\"";
  appendJsonEscaped(out, line2);
  out += "\",\"line3\":\"";
  appendJsonEscaped(out, line3);
  out += "\"},";

  out += "\"recycler\":[";
  const uint8_t rn = _status.recyclerEntryCount();
  for (uint8_t i = 0; i < rn; i++) {
    SystemStatus::RecyclerInventoryEntry e;
    if (!_status.recyclerEntryAt(i, e)) continue;
    if (out[out.length() - 1] != '[') out += ",";
    out += "{";
    out += "\"addr\":";
    out += String(e.addr);
    out += ",\"count10\":";
    out += String(e.count10);
    out += ",\"count20\":";
    out += String(e.count20);
    out += ",\"count50\":";
    out += String(e.count50);
    out += ",\"totalCents\":";
    out += String((unsigned long)e.totalCents);
    out += "}";
  }
  out += "]";

  out += "},";

  out += "\"logs\":";
  appendLogsArrayJson(out, RingLog::kCapacity);

  out += "}";

  _server.send(200, "application/json", out);
}

void WebServerService::handleApiResetCounters() {
  // Sottile wrapper HTTP sopra la callback applicativa.
  String message;
  bool ok = false;
  if (_onResetCounters) {
    ok = _onResetCounters(message, _actionsUserData);
  } else {
    message = "azione non configurata";
  }

  String out;
  out.reserve(192);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 500, "application/json", out);
}

void WebServerService::handleApiSetCoinBase() {
  // Parsing JSON volutamente minimale: il payload atteso e molto piccolo e noto.
  String message;
  bool ok = false;

  if (!_onSetCoinBase) {
    message = "azione non configurata";
  } else if (!_server.hasArg("plain")) {
    message = "payload mancante";
  } else {
    const String body = _server.arg("plain");
    int64_t parsed = 0;
    if (!extractJsonIntegerField(body, "coinLevelBaseCents", parsed)) {
      message = "campo coinLevelBaseCents mancante";
    } else if (parsed < 0) {
      message = "coinLevelBaseCents non valido";
    } else {
      ok = _onSetCoinBase(parsed, message, _actionsUserData);
    }
  }

  String out;
  out.reserve(192);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 400, "application/json", out);
}

void WebServerService::handleApiSetBillRecyclerBase() {
  String message;
  bool ok = false;

  if (!_onSetBillRecyclerBase) {
    message = "azione non configurata";
  } else if (!_server.hasArg("plain")) {
    message = "payload mancante";
  } else {
    const String body = _server.arg("plain");
    int64_t cassette10Count = 0;
    int64_t cassette20Count = 0;
    int64_t cassette50Count = 0;

    if (!extractJsonIntegerField(body, "cassette10Count", cassette10Count)) {
      message = "campo cassette10Count mancante";
    } else if (!extractJsonIntegerField(body, "cassette20Count", cassette20Count)) {
      message = "campo cassette20Count mancante";
    } else if (!extractJsonIntegerField(body, "cassette50Count", cassette50Count)) {
      message = "campo cassette50Count mancante";
    } else if (cassette10Count < 0 || cassette20Count < 0 || cassette50Count < 0) {
      message = "valori cassette banconote non validi";
    } else {
      ok = _onSetBillRecyclerBase(cassette10Count,
                                  cassette20Count,
                                  cassette50Count,
                                  message,
                                  _actionsUserData);
    }
  }

  String out;
  out.reserve(192);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 400, "application/json", out);
}

void WebServerService::handleApiSaveRemoteSnapshot() {
  String message;
  bool ok = false;

  if (_onSaveRemoteSnapshot) {
    ok = _onSaveRemoteSnapshot(message, _actionsUserData);
  } else {
    message = "azione non configurata";
  }

  String out;
  out.reserve(192);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 500, "application/json", out);
}

void WebServerService::handleApiEnterProgMode() {
  // Invia la risposta prima di chiamare la callback perche quest'ultima
  // causa un riavvio immediato del dispositivo (ESP.restart).
  _server.send(200, "application/json", "{\"ok\":true,\"message\":\"riavvio in modalita PROG\"}");
  delay(120);
  if (_onEnterProgMode) {
    String msg;
    _onEnterProgMode(msg, _enterProgModeUserData);
  }
}

void WebServerService::handleApiLogs() {
  uint16_t limit = 100;
  if (_server.hasArg("limit")) {
    long v = _server.arg("limit").toInt();
    if (v > 0 && v <= RingLog::kCapacity) {
      limit = (uint16_t)v;
    } else if (v > RingLog::kCapacity) {
      limit = RingLog::kCapacity;
    }
  }

  String out;
  out.reserve(2048);
  out += "{\"logs\":";
  appendLogsArrayJson(out, limit);
  out += "}";
  _server.send(200, "application/json", out);
}

void WebServerService::handleApiWifiNetworks() {
  if (_uiMode != UI_MODE_PROG) {
    _server.send(403, "application/json", "{\"ok\":false,\"message\":\"solo modalita PROG\"}");
    return;
  }

  WifiService::ScannedNetwork networks[WifiService::kMaxScannedNetworks];
  const uint8_t count = _wifi.scanNetworks(networks, WifiService::kMaxScannedNetworks);

  String out;
  out.reserve(2048);
  out += "{\"ok\":true,\"connectedSsid\":\"";
  appendJsonEscaped(out, _wifi.connectedSsid().c_str());
  out += "\",\"networks\":[";

  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) out += ",";
    out += "{";
    out += "\"ssid\":\"";
    appendJsonEscaped(out, networks[i].ssid);
    out += "\",\"rssi\":";
    out += String(networks[i].rssi);
    out += ",\"secure\":";
    out += (networks[i].encryption == WIFI_AUTH_OPEN ? "false" : "true");
    out += ",\"hidden\":";
    out += (networks[i].hidden ? "true" : "false");
    out += "}";
  }

  out += "]}";
  _server.send(200, "application/json", out);
}

void WebServerService::appendSettingsJson(String& out,
                                          const AppSettings& settings,
                                          bool presentCoinAcceptor,
                                          uint8_t presentHopperMask,
                                          uint16_t presentBillValidatorMask,
                                          const String& unknownDevicesCsv,
                                          const String& detectedDevicesJson) {
  // Serializzazione manuale della configurazione corrente/di default.
  out += "{";
  out += "\"wifiSsid\":\"";
  appendJsonEscaped(out, settings.wifiSsid);
  out += "\",\"wifiPass\":\"";
  appendJsonEscaped(out, settings.wifiPass);
  out += "\",\"saveWifiCredentials\":";
  out += (settings.saveWifiCredentials ? "true" : "false");
  out += ",\"serverUrl\":\"";
  appendJsonEscaped(out, settings.serverUrl);
  out += "\",\"remoteEventUrl\":\"";
  appendJsonEscaped(out, settings.remoteEventUrl);
  out += "\",\"locationCode\":\"";
  appendJsonEscaped(out, settings.locationCode);
  out += "\",\"apiKey\":\"";
  appendJsonEscaped(out, settings.apiKey);
  out += "\",\"mqttBrokerHost\":\"";
  appendJsonEscaped(out, settings.mqttBrokerHost);
  out += "\",\"mqttBrokerPort\":";
  out += String((unsigned)settings.mqttBrokerPort);
  out += ",\"mqttUsername\":\"";
  appendJsonEscaped(out, settings.mqttUsername);
  out += "\",\"mqttPassword\":\"";
  appendJsonEscaped(out, settings.mqttPassword);
  out += "\",\"mqttEnabled\":";
  out += (settings.mqttEnabled ? "true" : "false");
  out += ",\"dbHost\":\"";
  appendJsonEscaped(out, settings.dbHost);
  out += "\",\"dbPort\":";
  out += String((unsigned)settings.dbPort);
  out += ",\"dbName\":\"";
  appendJsonEscaped(out, settings.dbName);
  out += "\",\"dbUser\":\"";
  appendJsonEscaped(out, settings.dbUser);
  out += "\",\"dbPass\":\"";
  appendJsonEscaped(out, settings.dbPass);
  out += "\",\"hopperModel\":";
  out += String((unsigned)settings.hopperModel);
  out += ",\"billValidatorModel\":";
  out += String((unsigned)settings.billValidatorModel);
  out += ",\"coinAcceptorInEnabled\":";
  out += (settings.coinAcceptorInEnabled ? "true" : "false");
  out += ",\"coinAcceptorFalconProfile\":";
  out += String((unsigned)settings.coinAcceptorFalconProfile);
  out += ",\"coinInHopperMask\":";
  out += String((unsigned)settings.coinInHopperMask);
  out += ",\"coinOutHopperMask\":";
  out += String((unsigned)settings.coinOutHopperMask);
  out += ",\"hopperCoinValueCents\":[";
  for (uint8_t i = 0; i < kHopperAddressCount; i++) {
    if (i > 0) out += ",";
    out += String((unsigned)settings.hopperCoinValueCents[i]);
  }
  out += "]";
  out += ",\"billInValidatorMask\":";
  out += String((unsigned)settings.billInValidatorMask);
  out += ",\"billOutValidatorMask\":";
  out += String((unsigned)settings.billOutValidatorMask);
  out += ",\"presentCoinAcceptor\":";
  out += (presentCoinAcceptor ? "true" : "false");
  out += ",\"presentHopperMask\":";
  out += String((unsigned)presentHopperMask);
  out += ",\"presentBillValidatorMask\":";
  out += String((unsigned)presentBillValidatorMask);
  out += ",\"unknownDevicesCsv\":\"";
  appendJsonEscaped(out, unknownDevicesCsv.c_str());
  out += "\",\"detectedDevices\":";
  if (detectedDevicesJson.length() > 0) out += detectedDevicesJson;
  else out += "[]";
  out += "}";
}

bool WebServerService::parseSettingsFromRequest(AppSettings& out, String& message) {
  // La validazione e volutamente severa: blocca subito valori non supportati
  // invece di lasciarli propagare fino ai driver periferica.
  out.clear();

  copyBounded(_server.arg("wifiSsid"), out.wifiSsid, sizeof(out.wifiSsid));
  copyBounded(_server.arg("wifiPass"), out.wifiPass, sizeof(out.wifiPass));
  const String saveWifiValue = _server.arg("saveWifiCredentials");
  out.saveWifiCredentials = (saveWifiValue == "1" || saveWifiValue == "true" || saveWifiValue == "on");
  const String coinAcceptorInValue = _server.arg("coinAcceptorInEnabled");
  out.coinAcceptorInEnabled =
      (coinAcceptorInValue == "1" || coinAcceptorInValue == "true" || coinAcceptorInValue == "on");
  uint8_t coinAcceptorFalconProfile = COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0;
  if (_server.hasArg("coinAcceptorFalconProfile")) {
    const long parsedProfile = _server.arg("coinAcceptorFalconProfile").toInt();
    if (!isValidCoinAcceptorFalconProfile((uint8_t)parsedProfile)) {
      message = "coinAcceptorFalconProfile non valido";
      return false;
    }
    coinAcceptorFalconProfile = (uint8_t)parsedProfile;
  }
  copyBounded(_server.arg("serverUrl"), out.serverUrl, sizeof(out.serverUrl));
  copyBounded(_server.arg("remoteEventUrl"), out.remoteEventUrl, sizeof(out.remoteEventUrl));
  copyBounded(_server.arg("locationCode"), out.locationCode, sizeof(out.locationCode));
  copyBounded(_server.arg("apiKey"), out.apiKey, sizeof(out.apiKey));
  copyBounded(_server.arg("mqttBrokerHost"), out.mqttBrokerHost, sizeof(out.mqttBrokerHost));
  copyBounded(_server.arg("mqttUsername"), out.mqttUsername, sizeof(out.mqttUsername));
  copyBounded(_server.arg("mqttPassword"), out.mqttPassword, sizeof(out.mqttPassword));
  {
    const String mqttPortStr = _server.arg("mqttBrokerPort");
    const long mqttPort = mqttPortStr.length() > 0 ? mqttPortStr.toInt() : 1883;
    out.mqttBrokerPort = (mqttPort > 0 && mqttPort <= 65535) ? (uint16_t)mqttPort : 1883;
  }
  {
    const String mqttEnStr = _server.arg("mqttEnabled");
    out.mqttEnabled = (mqttEnStr == "1" || mqttEnStr == "true" || mqttEnStr == "on");
  }
  trimBuffer(out.mqttBrokerHost);
  {
    static const char* const kMqttSchemes[] = {
      "mqtts://", "mqtt://", "https://", "http://", "ssl://", "tls://"
    };
    for (size_t i = 0; i < sizeof(kMqttSchemes) / sizeof(kMqttSchemes[0]); i++) {
      const size_t slen = strlen(kMqttSchemes[i]);
      if (strncmp(out.mqttBrokerHost, kMqttSchemes[i], slen) == 0) {
        memmove(out.mqttBrokerHost, out.mqttBrokerHost + slen,
                strlen(out.mqttBrokerHost) - slen + 1);
        break;
      }
    }
  }
  copyBounded(_server.arg("dbHost"), out.dbHost, sizeof(out.dbHost));
  copyBounded(_server.arg("dbName"), out.dbName, sizeof(out.dbName));
  copyBounded(_server.arg("dbUser"), out.dbUser, sizeof(out.dbUser));
  copyBounded(_server.arg("dbPass"), out.dbPass, sizeof(out.dbPass));

  trimBuffer(out.wifiSsid);
  normalizeHttpUrlBuffer(out.serverUrl, sizeof(out.serverUrl));
  if (!buildDerivedRemoteEventUrl(out.serverUrl, out.remoteEventUrl, sizeof(out.remoteEventUrl))) {
    normalizeHttpUrlBuffer(out.remoteEventUrl, sizeof(out.remoteEventUrl));
  }
  trimBuffer(out.locationCode);
  trimBuffer(out.dbHost);
  trimBuffer(out.dbName);
  trimBuffer(out.dbUser);
  trimBuffer(out.dbPass);

  long dbPort = 3306;
  if (_server.hasArg("dbPort")) {
    dbPort = _server.arg("dbPort").toInt();
  }
  if (dbPort <= 0 || dbPort > 65535) {
    message = "dbPort non valido";
    return false;
  }
  long hopperModel = (long)HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
  if (_server.hasArg("hopperModel")) {
    hopperModel = _server.arg("hopperModel").toInt();
  }
  if (hopperModel != (long)HOPPER_MODEL_ALBERICI_DISCRIMINATOR &&
      hopperModel != (long)HOPPER_MODEL_ALBERICI_HOPPERCD &&
      hopperModel != (long)HOPPER_MODEL_SUZO_EVOLUTION &&
      hopperModel != (long)HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) {
    message = "hopperModel non valido";
    return false;
  }

  long billValidatorModel = (long)BILL_VALIDATOR_MODEL_MD100;
  if (_server.hasArg("billValidatorModel")) {
    billValidatorModel = _server.arg("billValidatorModel").toInt();
  }
  if (billValidatorModel != (long)BILL_VALIDATOR_MODEL_MD100 &&
      billValidatorModel != (long)BILL_VALIDATOR_MODEL_SMART_PAYOUT &&
      billValidatorModel != (long)BILL_VALIDATOR_MODEL_IPRO) {
    message = "billValidatorModel non valido";
    return false;
  }

  unsigned long hopperAlbericiDiscriminatorMask =
      (hopperModel == (long)HOPPER_MODEL_ALBERICI_DISCRIMINATOR) ? (unsigned long)kAllHopperMask : 0UL;
  if (_server.hasArg("hopperAlbericiDiscriminatorMask")) {
    if (!parseUnsignedLongStrict(_server.arg("hopperAlbericiDiscriminatorMask"), hopperAlbericiDiscriminatorMask) ||
        hopperAlbericiDiscriminatorMask > 0xFFUL) {
      message = "hopperAlbericiDiscriminatorMask non valido";
      return false;
    }
  }

  unsigned long hopperAlbericiHopperCdMask =
      (hopperModel == (long)HOPPER_MODEL_ALBERICI_HOPPERCD) ? (unsigned long)kAllHopperMask : 0UL;
  if (_server.hasArg("hopperAlbericiHopperCdMask")) {
    if (!parseUnsignedLongStrict(_server.arg("hopperAlbericiHopperCdMask"), hopperAlbericiHopperCdMask) ||
        hopperAlbericiHopperCdMask > 0xFFUL) {
      message = "hopperAlbericiHopperCdMask non valido";
      return false;
    }
  }

  unsigned long hopperSuzoEvolutionMask =
      (hopperModel == (long)HOPPER_MODEL_SUZO_EVOLUTION) ? (unsigned long)kAllHopperMask : 0UL;
  if (_server.hasArg("hopperSuzoEvolutionMask")) {
    if (!parseUnsignedLongStrict(_server.arg("hopperSuzoEvolutionMask"), hopperSuzoEvolutionMask) ||
        hopperSuzoEvolutionMask > 0xFFUL) {
      message = "hopperSuzoEvolutionMask non valido";
      return false;
    }
  }

  unsigned long hopperAzkoyenDiscriminatorMask =
      (hopperModel == (long)HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) ? (unsigned long)kAllHopperMask : 0UL;
  if (_server.hasArg("hopperAzkoyenDiscriminatorMask")) {
    if (!parseUnsignedLongStrict(_server.arg("hopperAzkoyenDiscriminatorMask"), hopperAzkoyenDiscriminatorMask) ||
        hopperAzkoyenDiscriminatorMask > 0xFFUL) {
      message = "hopperAzkoyenDiscriminatorMask non valido";
      return false;
    }
  }

  unsigned long billValidatorMd100Mask =
      (billValidatorModel == (long)BILL_VALIDATOR_MODEL_MD100) ? (unsigned long)kAllBillValidatorMask : 0UL;
  if (_server.hasArg("billValidatorMd100Mask")) {
    if (!parseUnsignedLongStrict(_server.arg("billValidatorMd100Mask"), billValidatorMd100Mask) ||
        billValidatorMd100Mask > (unsigned long)kAllBillValidatorMask) {
      message = "billValidatorMd100Mask non valido";
      return false;
    }
  }

  unsigned long billValidatorSmartPayoutMask =
      (billValidatorModel == (long)BILL_VALIDATOR_MODEL_SMART_PAYOUT) ? (unsigned long)kAllBillValidatorMask : 0UL;
  if (_server.hasArg("billValidatorSmartPayoutMask")) {
    if (!parseUnsignedLongStrict(_server.arg("billValidatorSmartPayoutMask"), billValidatorSmartPayoutMask) ||
        billValidatorSmartPayoutMask > (unsigned long)kAllBillValidatorMask) {
      message = "billValidatorSmartPayoutMask non valido";
      return false;
    }
  }

  unsigned long billValidatorIproMask =
      (billValidatorModel == (long)BILL_VALIDATOR_MODEL_IPRO) ? (unsigned long)kAllBillValidatorMask : 0UL;
  if (_server.hasArg("billValidatorIproMask")) {
    if (!parseUnsignedLongStrict(_server.arg("billValidatorIproMask"), billValidatorIproMask) ||
        billValidatorIproMask > (unsigned long)kAllBillValidatorMask) {
      message = "billValidatorIproMask non valido";
      return false;
    }
  }

  unsigned long coinInHopperMask = (unsigned long)kDefaultCoinInHopperMask;
  if (_server.hasArg("coinInHopperMask")) {
    if (!parseUnsignedLongStrict(_server.arg("coinInHopperMask"), coinInHopperMask) || coinInHopperMask > 0xFFUL) {
      message = "coinInHopperMask non valido";
      return false;
    }
  }

  unsigned long coinOutHopperMask = (unsigned long)kDefaultCoinOutHopperMask;
  if (_server.hasArg("coinOutHopperMask")) {
    if (!parseUnsignedLongStrict(_server.arg("coinOutHopperMask"), coinOutHopperMask) || coinOutHopperMask > 0xFFUL) {
      message = "coinOutHopperMask non valido";
      return false;
    }
  }

  for (uint8_t addr = kHopperAddressMin; addr <= kHopperAddressMax; addr++) {
    char argName[24] = {0};
    snprintf(argName, sizeof(argName), "hopperCoinValueCents%u", (unsigned)addr);
    unsigned long hopperCoinValueCents = (unsigned long)kDefaultHopperCoinValueCents;
    if (_server.hasArg(argName)) {
      if (!parseUnsignedLongStrict(_server.arg(argName), hopperCoinValueCents) ||
          hopperCoinValueCents > 0xFFFFUL) {
        message = String(argName) + " non valido";
        return false;
      }
    }
    const uint8_t idx = hopperAddressIndex(addr);
    if (idx < kHopperAddressCount) {
      out.hopperCoinValueCents[idx] = (uint16_t)hopperCoinValueCents;
    }
  }

  unsigned long billInValidatorMask = (unsigned long)kAllBillValidatorMask;
  if (_server.hasArg("billInValidatorMask")) {
    if (!parseUnsignedLongStrict(_server.arg("billInValidatorMask"), billInValidatorMask) ||
        billInValidatorMask > (unsigned long)kAllBillValidatorMask) {
      message = "billInValidatorMask non valido";
      return false;
    }
  }

  unsigned long billOutValidatorMask = (unsigned long)kAllBillValidatorMask;
  if (_server.hasArg("billOutValidatorMask")) {
    if (!parseUnsignedLongStrict(_server.arg("billOutValidatorMask"), billOutValidatorMask) ||
        billOutValidatorMask > (unsigned long)kAllBillValidatorMask) {
      message = "billOutValidatorMask non valido";
      return false;
    }
  }

  out.dbPort = (uint16_t)dbPort;
  out.hopperModel = (uint8_t)hopperModel;
  out.billValidatorModel = (uint8_t)billValidatorModel;
  out.hopperAlbericiDiscriminatorMask =
      sanitizeHopperModelAssignmentMask((uint8_t)hopperAlbericiDiscriminatorMask);
  out.hopperAlbericiHopperCdMask =
      sanitizeHopperModelAssignmentMask((uint8_t)hopperAlbericiHopperCdMask);
  out.hopperSuzoEvolutionMask =
      sanitizeHopperModelAssignmentMask((uint8_t)hopperSuzoEvolutionMask);
  out.hopperAzkoyenDiscriminatorMask =
      sanitizeHopperModelAssignmentMask((uint8_t)hopperAzkoyenDiscriminatorMask);
  out.billValidatorMd100Mask =
      sanitizeBillValidatorModelAssignmentMask((uint16_t)billValidatorMd100Mask);
  out.billValidatorSmartPayoutMask =
      sanitizeBillValidatorModelAssignmentMask((uint16_t)billValidatorSmartPayoutMask);
  out.billValidatorIproMask =
      sanitizeBillValidatorModelAssignmentMask((uint16_t)billValidatorIproMask);
  out.coinAcceptorFalconProfile = sanitizeCoinAcceptorFalconProfile(coinAcceptorFalconProfile);
  out.coinInHopperMask = sanitizeHopperContributionMask((uint8_t)coinInHopperMask);
  out.coinOutHopperMask = sanitizeHopperContributionMask((uint8_t)coinOutHopperMask);
  out.billInValidatorMask = sanitizeBillValidatorContributionMask((uint16_t)billInValidatorMask);
  out.billOutValidatorMask = sanitizeBillValidatorContributionMask((uint16_t)billOutValidatorMask);
  out.valid = true;
  return true;
}

void WebServerService::handleApiGetSettings() {
  // Endpoint di lettura impostazioni, disponibile solo in modalita PROG.
  if (_uiMode != UI_MODE_PROG) {
    _server.send(403, "application/json", "{\"ok\":false,\"message\":\"solo modalita PROG\"}");
    return;
  }

  AppSettings settings;
  settings.clear();
  bool presentCoinAcceptor = false;
  uint8_t presentHopperMask = 0;
  uint16_t presentBillValidatorMask = 0;
  String unknownDevicesCsv;
  String detectedDevicesJson = "[]";
  String message = "ok";
  bool ok = false;

  if (_onGetSettings) {
    ok = _onGetSettings(settings, message, _settingsUserData);
  } else {
    message = "azione non configurata";
  }

  if (_onGetPresentPeripheralCatalog) {
    _onGetPresentPeripheralCatalog(presentCoinAcceptor,
                                   presentHopperMask,
                                   presentBillValidatorMask,
                                   unknownDevicesCsv,
                                   detectedDevicesJson,
                                   _settingsUserData);
  }

  String out;
  out.reserve(1024);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\",\"settings\":";
  appendSettingsJson(out,
                     settings,
                     presentCoinAcceptor,
                     presentHopperMask,
                     presentBillValidatorMask,
                     unknownDevicesCsv,
                     detectedDevicesJson);
  out += "}";
  _server.send(ok ? 200 : 500, "application/json", out);
}

void WebServerService::handleApiSaveSettings() {
  // Endpoint di scrittura impostazioni con validazione sincrona.
  if (_uiMode != UI_MODE_PROG) {
    _server.send(403, "application/json", "{\"ok\":false,\"message\":\"solo modalita PROG\"}");
    return;
  }

  AppSettings settings;
  String message;
  bool ok = false;

  if (!_onSaveSettings) {
    message = "azione non configurata";
  } else if (!parseSettingsFromRequest(settings, message)) {
    // message gia impostato.
  } else {
    ok = _onSaveSettings(settings, message, _settingsUserData);
  }

  String out;
  out.reserve(256);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 400, "application/json", out);
}

void WebServerService::handleApiTestConnection() {
  if (_uiMode != UI_MODE_PROG) {
    _server.send(403, "application/json", "{\"ok\":false,\"message\":\"solo modalita PROG\"}");
    return;
  }

  AppSettings settings;
  settings.clear();
  String message;
  bool ok = false;

  if (!_onTestConnection) {
    message = "azione non configurata";
  } else if (hasAnyTestConnectionArg(_server)) {
    if (parseSettingsFromRequest(settings, message)) {
      ok = _onTestConnection(settings, message, _settingsUserData);
    }
  } else if (_onGetSettings && _onGetSettings(settings, message, _settingsUserData)) {
    ok = _onTestConnection(settings, message, _settingsUserData);
  } else if (message.length() == 0) {
    message = "impossibile caricare le impostazioni correnti";
  }

  String out;
  out.reserve(256);
  out += "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"message\":\"";
  appendJsonEscaped(out, message.c_str());
  out += "\"}";
  _server.send(ok ? 200 : 400, "application/json", out);
}

void WebServerService::appendJsonEscaped(String& out, const char* value) {
  // Escape minimo sufficiente per il JSON costruito manualmente nel file.
  const char* s = value ? value : "";
  while (*s) {
    const char c = *s++;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
}

} // namespace ccms
