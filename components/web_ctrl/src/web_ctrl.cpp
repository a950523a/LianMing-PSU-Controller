#include "web_ctrl.h"
#include "config_common.h"

#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "WebCtrl";

// ── Embedded HTML control page ────────────────────────────────────────────────

static const char HTML_PAGE[] = R"HTMLEOF(<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PSU Controller</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#c9d1d9;font-family:'Courier New',monospace;padding:16px;max-width:500px;margin:0 auto}
h1{color:#58a6ff;text-align:center;padding:14px 0 4px;font-size:1.15em;letter-spacing:3px;text-transform:uppercase}
#ver{text-align:center;color:#484f58;font-size:.75em;margin-bottom:14px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;margin:10px 0}
.card h2{color:#58a6ff;font-size:.8em;letter-spacing:2px;text-transform:uppercase;margin-bottom:12px;border-bottom:1px solid #21262d;padding-bottom:6px}
.row{display:flex;justify-content:space-between;align-items:center;margin:6px 0}
.lbl{color:#8b949e;font-size:.8em}
.val{color:#58a6ff;font-weight:bold}
.big{font-size:2em}
.badge{padding:3px 10px;border-radius:10px;font-size:.75em;font-weight:bold}
.on{background:#0d4a23;color:#3fb950}
.off{background:#3d1212;color:#f85149}
.ss{background:#4a2d00;color:#d29922}
input[type=number]{background:#0d1117;border:1px solid #30363d;color:#c9d1d9;
  padding:7px 6px;border-radius:4px;width:96px;text-align:center;font-family:inherit;font-size:.9em}
input[type=number]:focus{outline:none;border-color:#58a6ff}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
button{background:#161b22;border:1px solid #30363d;color:#c9d1d9;padding:9px 12px;
  border-radius:6px;cursor:pointer;font-family:inherit;font-size:.82em;transition:.15s}
button:hover{border-color:#58a6ff;color:#58a6ff}
button.g{border-color:#238636;color:#3fb950}
button.g:hover{background:#0d4a23}
button.r{border-color:#b62324;color:#f85149}
button.r:hover{background:#3d1212}
button.full{grid-column:span 2}
input[type=file]{font-size:.8em;color:#8b949e;margin-top:8px;width:100%}
#prog-wrap{display:none;background:#0d1117;border-radius:4px;height:6px;margin:10px 0;overflow:hidden;border:1px solid #30363d}
#prog{height:100%;background:#58a6ff;width:0%;transition:width .25s}
#ota-msg{font-size:.78em;color:#8b949e;margin-top:6px;min-height:1.2em}
.sep{border:none;border-top:1px solid #21262d;margin:10px 0}
</style>
</head>
<body>
<h1>LianMing PSU</h1>
<div id="ver">讀取中...</div>

<div class="card">
  <h2>即時狀態</h2>
  <div class="row">
    <div><div class="lbl">輸出電壓</div><div class="val big"><span id="v">--.-</span> V</div></div>
    <div style="text-align:right"><div class="lbl">輸出電流</div><div class="val big"><span id="i">--.-</span> A</div></div>
  </div>
  <hr class="sep">
  <div class="row">
    <span class="lbl">設定值</span>
    <span id="sp" class="val" style="font-size:.9em">-- V / -- A</span>
  </div>
  <div class="row">
    <span class="lbl">AC 輸入</span>
    <span id="ac" class="val" style="font-size:.9em">-- V</span>
  </div>
  <div class="row">
    <span class="lbl">均流模式</span>
    <span id="eq" class="val" style="font-size:.9em">--</span>
  </div>
  <div class="row">
    <span class="lbl">狀態</span>
    <span id="st" class="badge off">--</span>
  </div>
</div>

<div class="card">
  <h2>輸出設定</h2>
  <div class="row">
    <div><span class="lbl">電壓 (V)</span><br><input type="number" id="vIn" min="0" max="120" step="0.5" value="0"></div>
    <div style="text-align:right"><span class="lbl">電流 (A)</span><br><input type="number" id="iIn" min="0" max="100" step="0.5" value="0"></div>
  </div>
  <div class="btns" style="margin-top:12px">
    <button onclick="setVI()" class="full">套用 V / I 設定</button>
  </div>
</div>

<div class="card">
  <h2>控制</h2>
  <div class="btns">
    <button class="g" onclick="cmd('ON')">▶ 開啟輸出</button>
    <button class="r" onclick="cmd('OFF')">■ 關閉輸出</button>
    <button onclick="cmd('EQ_ON')">均流 ON</button>
    <button onclick="cmd('EQ_OFF')">均流 OFF</button>
    <button onclick="cmd('QUERY_AC')" class="full">查詢 AC 電壓</button>
  </div>
</div>

<div class="card">
  <h2>OTA 韌體更新</h2>
  <div class="lbl">選擇 <b>.bin</b> 韌體檔後點「上傳」，裝置更新完成後自動重新啟動。</div>
  <input type="file" id="fw" accept=".bin">
  <div class="btns">
    <button onclick="uploadFw()" class="full" style="margin-top:4px">上傳韌體</button>
  </div>
  <div id="prog-wrap"><div id="prog"></div></div>
  <div id="ota-msg"></div>
</div>

<script>
async function fetchStatus() {
  try {
    const d = await (await fetch('/api/status')).json();
    document.getElementById('v').textContent   = d.v.toFixed(1);
    document.getElementById('i').textContent   = d.i.toFixed(1);
    document.getElementById('sp').textContent  = d.vset.toFixed(1)+' V / '+d.iset.toFixed(1)+' A';
    document.getElementById('ac').textContent  = d.ac > 0 ? d.ac.toFixed(1)+' V' : '-- V';
    document.getElementById('eq').textContent  = d.eq ? 'ON' : 'OFF';
    document.getElementById('ver').textContent = 'Firmware: '+d.version+'  │  IP: 192.168.4.1';
    const st = document.getElementById('st');
    if (d.softstart) { st.textContent='SOFT-START'; st.className='badge ss'; }
    else if (d.on)   { st.textContent='ON';         st.className='badge on'; }
    else             { st.textContent='OFF';         st.className='badge off'; }
  } catch(e) {}
}

async function cmd(c) {
  try {
    await fetch('/api/cmd', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({cmd: c})
    });
  } catch(e) {}
}

async function setVI() {
  const v = parseFloat(document.getElementById('vIn').value);
  const i = parseFloat(document.getElementById('iIn').value);
  try {
    await fetch('/api/cmd', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({cmd:'SET', v:v, i:i})
    });
  } catch(e) {}
}

function uploadFw() {
  const file = document.getElementById('fw').files[0];
  if (!file) { alert('請先選擇 .bin 韌體檔'); return; }
  const msg  = document.getElementById('ota-msg');
  const prog = document.getElementById('prog');
  const wrap = document.getElementById('prog-wrap');
  wrap.style.display = 'block';
  prog.style.width = '0%';
  msg.textContent = '準備上傳...';
  const xhr = new XMLHttpRequest();
  xhr.upload.onprogress = e => {
    if (e.lengthComputable) prog.style.width = (e.loaded/e.total*90)+'%';
  };
  xhr.onload = () => {
    if (xhr.status === 200) {
      prog.style.width = '100%';
      msg.textContent = '更新成功！裝置 5 秒後重新啟動...';
    } else {
      msg.textContent = '失敗 (HTTP '+xhr.status+'): '+xhr.responseText;
    }
  };
  xhr.onerror = () => { msg.textContent = '上傳錯誤，請檢查連線'; };
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.send(file);
}

fetchStatus();
setInterval(fetchStatus, 1000);
</script>
</body>
</html>
)HTMLEOF";

// ── OTA guard (prevent concurrent uploads) ────────────────────────────────────

static volatile bool s_ota_running = false;

// ── HTTP request handlers (static free functions) ─────────────────────────────

static esp_err_t handle_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, HTML_PAGE, (ssize_t)strlen(HTML_PAGE));
}

static esp_err_t handle_status(httpd_req_t* req) {
    WebCtrl*     self = static_cast<WebCtrl*>(req->user_ctx);
    PowerStatus  st   = self->_psu->getStatus();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v",         (double)st.voltageOut);
    cJSON_AddNumberToObject(root, "i",         (double)st.currentOut);
    cJSON_AddNumberToObject(root, "vset",      (double)st.voltageSet);
    cJSON_AddNumberToObject(root, "iset",      (double)st.currentSet);
    cJSON_AddNumberToObject(root, "ac",        (double)st.inputVoltage);
    cJSON_AddBoolToObject  (root, "on",        st.isOn);
    cJSON_AddBoolToObject  (root, "softstart", st.isSoftStarting);
    cJSON_AddBoolToObject  (root, "eq",        false);
    cJSON_AddStringToObject(root, "version",   FIRMWARE_VERSION);

    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_cmd(httpd_req_t* req) {
    WebCtrl* self = static_cast<WebCtrl*>(req->user_ctx);

    char buf[256] = {};
    int  recv_len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[recv_len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (cJSON_IsString(jcmd) && jcmd->valuestring) {
        WebCmd      wc = {};
        const char* cs = jcmd->valuestring;

        if      (strcmp(cs, "ON")       == 0) { wc.type = WebCmd::ON; }
        else if (strcmp(cs, "OFF")      == 0) { wc.type = WebCmd::OFF; }
        else if (strcmp(cs, "EQ_ON")    == 0) { wc.type = WebCmd::EQ_ON; }
        else if (strcmp(cs, "EQ_OFF")   == 0) { wc.type = WebCmd::EQ_OFF; }
        else if (strcmp(cs, "QUERY_AC") == 0) { wc.type = WebCmd::QUERY_AC; }
        else if (strcmp(cs, "SET")      == 0) {
            wc.type = WebCmd::SET_VI;
            cJSON* jv = cJSON_GetObjectItemCaseSensitive(root, "v");
            cJSON* ji = cJSON_GetObjectItemCaseSensitive(root, "i");
            wc.v = cJSON_IsNumber(jv) ? (float)jv->valuedouble : 0.0f;
            wc.i = cJSON_IsNumber(ji) ? (float)ji->valuedouble : 0.0f;
        }

        if (wc.type != WebCmd::NONE) {
            xQueueSend(self->_cmdQueue, &wc, 0);
        }
    }
    cJSON_Delete(root);

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t* req) {
    if (s_ota_running) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already in progress");
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition — flash with OTA partition table first (idf.py erase-flash && idf.py flash)");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    s_ota_running = true;
    ESP_LOGI(TAG, "OTA start: %d bytes -> partition '%s'", req->content_len, update_part->label);

    static char buf[2048];
    int  remaining = req->content_len;
    bool failed    = false;

    while (remaining > 0) {
        int chunk = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int len   = httpd_req_recv(req, buf, chunk);
        if (len <= 0) { failed = true; break; }
        if (esp_ota_write(ota_handle, buf, (size_t)len) != ESP_OK) { failed = true; break; }
        remaining -= len;
    }

    if (failed || esp_ota_end(ota_handle) != ESP_OK) {
        s_ota_running = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write/verify failed");
        return ESP_FAIL;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        s_ota_running = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success — restarting in 5 s");
    httpd_resp_sendstr(req, "OTA success — restarting");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── WebCtrl public API ────────────────────────────────────────────────────────

WebCtrl::WebCtrl(PowerProtocol* psu)
    : _psu(psu), _cmdQueue(nullptr) {}

void WebCtrl::begin() {
    _cmdQueue = xQueueCreate(8, sizeof(WebCmd));

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers  = 8;
    config.stack_size        = 8192;
    config.recv_wait_timeout = 60;  // seconds — needed for large OTA uploads
    config.send_wait_timeout = 60;
    config.lru_purge_enable  = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // httpd_register_uri_handler copies the struct internally, so local vars are safe.
    // Designated initializers zero-fill optional fields (WS support, etc.).
    httpd_uri_t uri_root   = { .uri="/",           .method=HTTP_GET,  .handler=handle_root,   .user_ctx=this };
    httpd_uri_t uri_status = { .uri="/api/status", .method=HTTP_GET,  .handler=handle_status, .user_ctx=this };
    httpd_uri_t uri_cmd    = { .uri="/api/cmd",    .method=HTTP_POST, .handler=handle_cmd,    .user_ctx=this };
    httpd_uri_t uri_ota    = { .uri="/api/ota",    .method=HTTP_POST, .handler=handle_ota,    .user_ctx=this };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_cmd);
    httpd_register_uri_handler(server, &uri_ota);

    ESP_LOGI(TAG, "HTTP server ready — SSID: PSU-Controller  Password: psu12345  URL: http://192.168.4.1");
}

void WebCtrl::processCommands() {
    WebCmd wc;
    while (xQueueReceive(_cmdQueue, &wc, 0) == pdTRUE) {
        switch (wc.type) {
            case WebCmd::ON:       _psu->setPower(true);          break;
            case WebCmd::OFF:      _psu->setPower(false);         break;
            case WebCmd::SET_VI:   _psu->setOutput(wc.v, wc.i);  break;
            case WebCmd::EQ_ON:    _psu->setEqualization(true);   break;
            case WebCmd::EQ_OFF:   _psu->setEqualization(false);  break;
            case WebCmd::QUERY_AC: _psu->queryInputVoltage();     break;
            default: break;
        }
    }
}
