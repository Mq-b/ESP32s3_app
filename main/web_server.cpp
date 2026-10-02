#include "web_server.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "HTTP";

namespace {

/**
 * @brief 首页：深色卡片风格，表格展示扫描结果，每 2 秒轮询；
 *        「持续扫描」开启时，一轮结束自动发起下一轮。
 */
const char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3 · 设备监控</title>
<style>
* { box-sizing: border-box; margin: 0; }
body { font-family: system-ui, -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
       background: #0f172a; color: #e2e8f0; min-height: 100vh; padding: 36px 20px; }
.wrap { max-width: 820px; margin: 0 auto; }
h1 { font-size: 20px; font-weight: 600; color: #38bdf8; }
.sub { font-size: 12px; color: #64748b; margin: 4px 0 22px; }
.bar { display: flex; align-items: center; gap: 12px; margin-bottom: 14px; font-size: 13px; color: #94a3b8; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: #4ade80; animation: pulse 1.2s ease-in-out infinite; }
@keyframes pulse { 50% { opacity: .25; } }
label { display: flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; }
.card { background: #1e293b; border: 1px solid #2c3a52; border-radius: 14px; overflow: hidden; }
.ledbar { display: flex; align-items: center; gap: 8px; margin: 0 0 14px; padding: 12px 14px; }
.ledbar span { color: #94a3b8; font-size: 13px; margin-right: auto; }
.color-picker { width: 42px; height: 32px; padding: 2px; border: 1px solid #3b4d6d; border-radius: 8px; background: #263852; cursor: pointer; }
.rgb-value { min-width: 92px; margin-right: 4px !important; font-family: ui-monospace, Consolas, monospace; font-size: 12px !important; }
button { border: 1px solid #3b4d6d; border-radius: 8px; background: #263852; color: #cbd5e1; padding: 7px 12px; cursor: pointer; }
button:hover, button.active { background: #0369a1; border-color: #38bdf8; color: white; }
table { width: 100%; border-collapse: collapse; font-size: 14px; }
th { text-align: left; padding: 11px 16px; font-size: 11px; font-weight: 500; color: #64748b; text-transform: uppercase; letter-spacing: 1px; background: #182238; }
td { padding: 11px 16px; border-top: 1px solid #26334a; }
tbody tr:hover { background: #243149; }
.mono { font-family: ui-monospace, Consolas, "Cascadia Mono", monospace; font-size: 13px; }
.badge { display: inline-block; padding: 2px 10px; border-radius: 999px; font-size: 12px; white-space: nowrap; }
.win { background: #164e63; color: #67e8f9; }.nix { background: #14532d; color: #86efac; }.net { background: #713f12; color: #fde047; }.esp { background: #3730a3; color: #c7d2fe; }.unk { background: #3f3f46; color: #d4d4d8; }
.dim { color: #64748b; font-size: 12px; }.empty { padding: 48px; text-align: center; color: #64748b; }
.metrics { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; margin-bottom: 14px; }
.metric { padding: 16px; }
.metric h2 { font-size: 13px; color: #94a3b8; font-weight: 500; margin-bottom: 10px; }
.metric strong { display: block; font-size: 24px; font-weight: 600; margin-bottom: 8px; }
.metric p { color: #94a3b8; font-size: 12px; line-height: 1.8; overflow-wrap: anywhere; }
.meter { height: 6px; background: #0f172a; border-radius: 8px; margin-bottom: 10px; overflow: hidden; }
.meter i { display: block; height: 100%; width: 0; background: #38bdf8; transition: width .3s; }
.resource-bar { display: flex; flex-wrap: wrap; gap: 8px; margin: 0 0 12px; font-size: 12px; color: #94a3b8; }
@media (max-width: 540px) { .metrics { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP32-S3 · 设备监控</h1>
  <p class="sub">资源监控每 2 秒刷新 · CPU 为采样窗口内调度负载估算</p>
  <div class="resource-bar"><span id="systemStatus" role="status">资源读取中…</span><span id="systemInfo"></span></div>
  <div class="metrics">
    <section class="card metric"><h2>CPU 使用率</h2><strong id="cpuValue">—</strong><div class="meter"><i id="cpuMeter"></i></div><p id="cpuDetail">等待采样</p></section>
    <section class="card metric"><h2>内部 RAM · 可分配堆</h2><strong id="ramValue">—</strong><div class="meter"><i id="ramMeter"></i></div><p id="ramDetail">读取中…</p></section>
    <section class="card metric"><h2>PSRAM · 可分配堆</h2><strong id="psramValue">—</strong><div class="meter"><i id="psramMeter"></i></div><p id="psramDetail">读取中…</p></section>
    <section class="card metric"><h2>SPIFFS · 文件存储</h2><strong id="storageValue">—</strong><div class="meter"><i id="storageMeter"></i></div><p id="storageDetail">读取中…</p></section>
  </div>
  <h1>内网设备</h1>
  <p class="sub" id="sub">ESP32-S3 · 持续扫描 /24 网段</p>
  <div class="bar"><span class="dot"></span><span id="status">准备中…</span><label style="margin-left:auto"><input type="checkbox" id="auto" checked>持续扫描</label></div>
  <div class="card ledbar">
    <span>板载 WS2812：<b id="ledStatus">读取中…</b></span>
    <input class="color-picker" id="ledColor" type="color" value="#101010" title="选择 RGB 颜色">
    <span class="rgb-value" id="rgbValue">RGB(16, 16, 16)</span>
    <button data-mode="off">关闭</button><button data-mode="on">常亮</button><button data-mode="blink">闪烁</button>
  </div>
  <div class="card"><table><thead><tr><th>IP 地址</th><th>MAC 地址</th><th>系统</th><th>延迟</th><th>来源</th></tr></thead><tbody id="rows"></tbody></table><div class="empty" id="empty">正在扫描网段…</div></div>
</div>
<script>
const $ = id => document.getElementById(id);
/** @brief 将字节数格式化为可读容量。 */
function formatBytes(value) {
  if (value < 1024) return value + ' B';
  if (value < 1048576) return (value / 1024).toFixed(1) + ' KiB';
  return (value / 1048576).toFixed(2) + ' MiB';
}
/** @brief 渲染使用率文字及进度条；无数据时清除旧值。 */
function showUsage(prefix, percent, unavailable = '—') {
  $(prefix + 'Value').textContent = percent === null ? unavailable : percent.toFixed(1) + '%';
  $(prefix + 'Meter').style.width = percent === null ? '0%' : Math.max(0, Math.min(100, percent)) + '%';
}
/** @brief 渲染可分配堆容量，不将其误标为芯片物理 RAM 总量。 */
function showHeap(prefix, heap) {
  const available = heap.total_bytes > 0;
  showUsage(prefix, available ? 100 * (1 - heap.free_bytes / heap.total_bytes) : null, '未启用');
  $(prefix + 'Detail').textContent = available
    ? '空闲 ' + formatBytes(heap.free_bytes) + ' / ' + formatBytes(heap.total_bytes) +
      ' · 最低空闲 ' + formatBytes(heap.minimum_free_bytes) + ' · 最大连续块 ' + formatBytes(heap.largest_free_block_bytes)
    : '无可分配容量，未启用或不存在';
}
/** @brief 独立轮询资源接口，超时或失败清除旧数据，避免将过期值显示为实时值。 */
async function refreshSystem() {
  if (document.hidden) { setTimeout(refreshSystem, 2000); return; }
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch('/api/system', { cache: 'no-store', signal: controller.signal });
    if (!response.ok) throw new Error('资源接口请求失败');
    const j = await response.json();
    $('systemStatus').textContent = '资源在线 · ' + new Date().toLocaleTimeString();
    $('systemInfo').textContent = '运行 ' + Math.floor(j.uptime_ms / 1000) + ' 秒 · ' + j.task_count +
      ' 个任务 · Flash ' + (j.flash_bytes === null ? '未知' : formatBytes(j.flash_bytes));
    showUsage('cpu', j.cpu.usage_percent, '采样中 / 未启用');
    $('cpuDetail').textContent = j.cpu.available
      ? j.cpu.cores_percent.map((value, core) => '核 ' + core + '：' + value.toFixed(1) + '%').join(' · ') +
        ' · 窗口 ' + (j.cpu.sample_window_ms / 1000).toFixed(1) + ' 秒'
      : '首次读取建立基线；需要启用 FreeRTOS 运行统计';
    showHeap('ram', j.memory.internal);
    showHeap('psram', j.memory.psram);
    showUsage('storage', j.storage.available && j.storage.total_bytes > 0
      ? 100 * j.storage.used_bytes / j.storage.total_bytes : null, '未挂载');
    $('storageDetail').textContent = j.storage.available
      ? '已用 ' + formatBytes(j.storage.used_bytes) + ' / ' + formatBytes(j.storage.total_bytes) + ' · 不包含固件、NVS 等分区'
      : 'SPIFFS 存储不可用';
  } catch (e) {
    $('systemStatus').textContent = '资源连接失败，数据已失效';
    $('systemInfo').textContent = '';
    for (const prefix of ['cpu', 'ram', 'psram', 'storage']) {
      showUsage(prefix, null);
      $(prefix + 'Detail').textContent = '等待重新连接';
    }
  } finally {
    clearTimeout(timeout);
    setTimeout(refreshSystem, 2000);
  }
}
refreshSystem();
const badge = os => { if (os.startsWith('Windows')) return ['win', 'Windows']; if (os.startsWith('Linux')) return ['nix', 'Linux / macOS']; if (os.startsWith('Network')) return ['net', '网络设备']; if (os.startsWith('ESP32')) return ['esp', 'ESP32-S3']; return ['unk', os || '未知']; };
function row(v) { const [cls, label] = badge(v.os); return '<tr><td class="mono">' + v.ip + '</td><td class="mono">' + v.mac + '</td><td><span class="badge ' + cls + '" title="TTL ' + v.ttl + '">' + label + '</span></td><td class="mono">' + (v.ping ? v.rtt_ms + ' ms' : '—') + '</td><td class="dim">' + (v.ping ? 'ICMP' : 'ARP') + '</td></tr>'; }
async function refresh() { try { const j = await (await fetch('/api/devices', { cache: 'no-store' })).json(); $('rows').innerHTML = j.devices.map(row).join(''); $('empty').style.display = j.count ? 'none' : 'block'; $('status').textContent = j.scanning ? '扫描中…' : '空闲'; $('sub').textContent = 'ESP32-S3 · ' + location.hostname + ' · ' + j.count + ' 台设备'; if (!j.scanning && $('auto').checked) fetch('/api/scan', { method: 'POST' }).catch(() => {}); } catch (e) { $('status').textContent = '连接失败'; } }
let ledMode = 'off';
let colorDirty = false;
let ledRequestSerial = 0;
function hexColor(r, g, b) { return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join(''); }
function selectedRgb() { const hex = $('ledColor').value; return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)]; }
function showColor(r, g, b) { $('ledColor').value = hexColor(r, g, b); $('rgbValue').textContent = 'RGB(' + r + ', ' + g + ', ' + b + ')'; }
async function setLed(mode) { const [r, g, b] = selectedRgb(); colorDirty = true; ++ledRequestSerial; const response = await fetch('/api/led?mode=' + mode + '&r=' + r + '&g=' + g + '&b=' + b, { method: 'POST' }); if (!response.ok) throw new Error('LED 控制请求失败'); await refreshLed(true); }
async function refreshLed(syncColor = false) { if (colorDirty && !syncColor) return; const serial = ++ledRequestSerial; try { const j = await (await fetch('/api/led', { cache: 'no-store' })).json(); if (serial !== ledRequestSerial) return; const labels = { off: '关闭', on: '常亮', blink: '闪烁' }; ledMode = j.mode; $('ledStatus').textContent = labels[j.mode] || '未知'; if (!colorDirty || syncColor) { showColor(j.r, j.g, j.b); colorDirty = false; } document.querySelectorAll('[data-mode]').forEach(b => b.classList.toggle('active', b.dataset.mode === j.mode)); } catch (e) { $('ledStatus').textContent = '连接失败'; } }
document.querySelectorAll('[data-mode]').forEach(b => b.addEventListener('click', () => setLed(b.dataset.mode).catch(() => refreshLed(true))));
$('ledColor').addEventListener('input', () => { colorDirty = true; ++ledRequestSerial; const [r, g, b] = selectedRgb(); $('rgbValue').textContent = 'RGB(' + r + ', ' + g + ', ' + b + ')'; });
$('ledColor').addEventListener('change', () => setLed(ledMode).catch(() => refreshLed(true)));
fetch('/api/scan', { method: 'POST' }).catch(() => {}); refresh(); refreshLed(); setInterval(refresh, 2000); setInterval(refreshLed, 2000);
</script>
</body>
</html>)HTML";

esp_err_t indexHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief 返回系统资源快照，由 HTTP 服务任务串行访问采集器。
 * @param req 包含 SystemMonitor 上下文的 HTTP 请求。
 * @return HTTP 响应发送结果。
 */
esp_err_t systemHandler(httpd_req_t *req) {
    auto *monitor = static_cast<SystemMonitor *>(req->user_ctx);
    const std::string json = monitor->statusJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t devicesHandler(httpd_req_t *req) {
    auto *scanner = static_cast<NetworkScanner *>(req->user_ctx);
    std::string json = scanner->devicesJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

esp_err_t scanHandler(httpd_req_t *req) {
    auto *scanner = static_cast<NetworkScanner *>(req->user_ctx);
    scanner->startAsync();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

const char *ledModeName(Ws2812Mode mode) {
    switch (mode) {
    case Ws2812Mode::Off:
        return "off";
    case Ws2812Mode::On:
        return "on";
    case Ws2812Mode::Blink:
        return "blink";
    }
    return "off";
}

bool readColorParameter(const char *query, const char *key, uint8_t &value) {
    char text[4] = {};
    if (httpd_query_key_value(query, key, text, sizeof(text)) != ESP_OK) {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 255) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

bool readRequestedColor(const char *query, Ws2812Color &color) {
    const bool hasRed = strstr(query, "r=") != nullptr;
    const bool hasGreen = strstr(query, "g=") != nullptr;
    const bool hasBlue = strstr(query, "b=") != nullptr;
    if (!hasRed && !hasGreen && !hasBlue) {
        return true;
    }
    return hasRed && hasGreen && hasBlue && readColorParameter(query, "r", color.red) &&
           readColorParameter(query, "g", color.green) && readColorParameter(query, "b", color.blue);
}

esp_err_t ledStatusHandler(httpd_req_t *req) {
    auto *led = static_cast<Ws2812Controller *>(req->user_ctx);
    const Ws2812Color color = led->color();
    const std::string response = std::string("{\"mode\":\"") + ledModeName(led->mode()) +
                                 "\",\"r\":" + std::to_string(color.red) +
                                 ",\"g\":" + std::to_string(color.green) +
                                 ",\"b\":" + std::to_string(color.blue) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, response.c_str(), response.size());
    return ESP_OK;
}

esp_err_t ledControlHandler(httpd_req_t *req) {
    char query[64] = {};
    char mode[16] = {};
    auto *led = static_cast<Ws2812Controller *>(req->user_ctx);
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mode", mode, sizeof(mode)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 mode 参数");
        return ESP_FAIL;
    }

    Ws2812Color color = led->color();
    if (!readRequestedColor(query, color)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "RGB 参数必须为 0 至 255 的整数");
        return ESP_FAIL;
    }
    led->setColor(color);

    if (strcmp(mode, "off") == 0) {
        led->turnOff();
    } else if (strcmp(mode, "on") == 0) {
        led->turnOn(color);
    } else if (strcmp(mode, "blink") == 0) {
        led->startBlinking(color, 500, 500);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "不支持的 mode 参数");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

}  // namespace

void WebServer::start(const std::string &deviceIp) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    auto *scanner = &scanner_;
    auto *led = &led_;

    const httpd_uri_t indexUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = indexHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t devicesUri = {
        .uri = "/api/devices",
        .method = HTTP_GET,
        .handler = devicesHandler,
        .user_ctx = scanner,
    };
    const httpd_uri_t scanUri = {
        .uri = "/api/scan",
        .method = HTTP_POST,
        .handler = scanHandler,
        .user_ctx = scanner,
    };
    const httpd_uri_t ledStatusUri = {
        .uri = "/api/led",
        .method = HTTP_GET,
        .handler = ledStatusHandler,
        .user_ctx = led,
    };
    const httpd_uri_t ledControlUri = {
        .uri = "/api/led",
        .method = HTTP_POST,
        .handler = ledControlHandler,
        .user_ctx = led,
    };

    const httpd_uri_t systemUri = {
        .uri = "/api/system",
        .method = HTTP_GET,
        .handler = systemHandler,
        .user_ctx = &monitor_,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &systemUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &indexUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devicesUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scanUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ledStatusUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ledControlUri));

    ESP_LOGI(TAG, "HTTP 服务已启动: http://%s/", deviceIp.c_str());
}
