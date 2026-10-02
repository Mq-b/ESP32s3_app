const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');

const source = fs.readFileSync(path.join(__dirname, '../main/web_server.cpp'), 'utf8');
const html = source.match(/R"HTML\(([\s\S]*?)\)HTML"/)[1];
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const monitorScript = script.slice(0, script.indexOf('refreshSystem();'));

function setup(data, ok = true) {
  const elements = Object.fromEntries([...html.matchAll(/id="([^"]+)"/g)].map(m => [m[1], { textContent: '', style: {} }]));
  const timers = [];
  let requests = 0;
  const document = { hidden: false, getElementById: id => elements[id] };
  const context = vm.createContext({ document, AbortController, Date, Math,
    setTimeout: (callback, delay) => { timers.push({ callback, delay }); return timers.length; },
    clearTimeout: () => {},
    fetch: async () => { requests++; return { ok, json: async () => data }; }
  });
  vm.runInContext(monitorScript, context);
  return { context, elements, document, timers, requests: () => requests };
}

const heap = { total_bytes: 200000, free_bytes: 100000, minimum_free_bytes: 80000, largest_free_block_bytes: 50000 };
const fixture = () => ({ uptime_ms: 10000, task_count: 12, flash_bytes: 8388608,
  memory: { internal: heap, psram: { total_bytes: 0 } },
  storage: { available: true, total_bytes: 1000000, used_bytes: 100000 },
  cpu: { available: true, usage_percent: 35, cores_percent: [20, 50], sample_window_ms: 2000 } });

test('正常资源、双核负载与未启用 PSRAM', async () => {
  const s = setup(fixture());
  await vm.runInContext('refreshSystem()', s.context);
  assert.equal(s.elements.cpuValue.textContent, '35.0%');
  assert.match(s.elements.cpuDetail.textContent, /核 0：20.0% · 核 1：50.0%/);
  assert.equal(s.elements.ramValue.textContent, '50.0%');
  assert.equal(s.elements.psramValue.textContent, '未启用');
  assert.equal(s.elements.storageValue.textContent, '10.0%');
  assert.match(s.elements.systemInfo.textContent, /8.00 MiB/);
  assert.equal(s.timers.at(-1).delay, 2000);
});

test('首次 CPU 和未挂载存储不显示为零负载', async () => {
  const data = fixture();
  data.cpu = { available: false, usage_percent: null, cores_percent: [null, null] };
  data.storage.available = false;
  const s = setup(data);
  await vm.runInContext('refreshSystem()', s.context);
  assert.equal(s.elements.cpuValue.textContent, '采样中 / 未启用');
  assert.equal(s.elements.cpuMeter.style.width, '0%');
  assert.equal(s.elements.storageValue.textContent, '未挂载');
});

test('HTTP 失败清除过期数据并继续轮询', async () => {
  const s = setup(fixture(), false);
  s.elements.cpuValue.textContent = '旧负载';
  await vm.runInContext('refreshSystem()', s.context);
  assert.equal(s.elements.cpuValue.textContent, '—');
  assert.match(s.elements.systemStatus.textContent, /数据已失效/);
  assert.equal(s.timers.at(-1).delay, 2000);
});

test('隐藏页面暂停资源请求', async () => {
  const s = setup(fixture());
  s.document.hidden = true;
  await vm.runInContext('refreshSystem()', s.context);
  assert.equal(s.requests(), 0);
  assert.equal(s.timers.at(-1).delay, 2000);
});
