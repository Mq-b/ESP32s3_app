"""独立 BLE 配网 GUI：扫描、配对、配置读写及重启，双击即可启动。"""

import asyncio
import ipaddress
import json
import queue
import threading
import sys
import tkinter as tk
from tkinter import messagebox, ttk

SERVICE_UUID = "7d9a0001-6f41-4b5b-9c82-56e0438ab100"
DATA_UUID = "7d9a0002-6f41-4b5b-9c82-56e0438ab100"
CONTROL_UUID = "7d9a0003-6f41-4b5b-9c82-56e0438ab100"
CHUNK_SIZE = 20  # 兼容默认 ATT MTU，不依赖设备或操作系统的 MTU 协商。


def encode_config(config: dict) -> bytes:
    """校验配置并编码为有长度上限的 UTF-8 JSON，错误消息不包含密码。"""
    keys = ("ssid", "password", "static_ip", "gateway", "netmask")
    if not isinstance(config, dict) or any(not isinstance(config.get(k), str) for k in keys):
        raise ValueError("配置必须包含 ssid、password、static_ip、gateway、netmask 五个字符串字段")
    if any("\x00" in config[k] for k in keys):
        raise ValueError("配置不能包含空字符")
    if not 1 <= len(config["ssid"].encode("utf-8")) <= 32:
        raise ValueError("SSID 长度必须为 1 至 32 字节")
    password = config["password"]
    length = len(password.encode("utf-8"))
    hex_psk = len(password) == 64 and all(c in "0123456789abcdefABCDEF" for c in password)
    if not (length == 0 or 8 <= length <= 63 or hex_psk):
        raise ValueError("密码必须为空、8 至 63 字节或 64 位十六进制 PSK")
    try:
        ip = ipaddress.IPv4Address(config["static_ip"])
        gateway = ipaddress.IPv4Address(config["gateway"])
        mask = ipaddress.IPv4Address(config["netmask"])
        network = ipaddress.IPv4Network(f"{ip}/{mask}", strict=False)
    except ValueError:
        raise ValueError("IP、网关或子网掩码格式错误") from None
    if (int(mask) == 0 or network.prefixlen > 30 or gateway not in network or ip == gateway
            or ip in (network.network_address, network.broadcast_address)
            or gateway in (network.network_address, network.broadcast_address)
            or int(ip) >> 24 in (0, 127) or int(ip) >> 24 >= 224
            or int(gateway) >> 24 in (0, 127) or int(gateway) >> 24 >= 224):
        raise ValueError("子网掩码、主机地址或同网段网关无效")
    payload = json.dumps({k: config[k] for k in keys}, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
    if len(payload) > 1024:
        raise ValueError("配置超过 1024 字节")
    return payload


async def send_config(client, payload: bytes, reboot: bool = True) -> None:
    """以带响应的分包写入配置，确认持久保存后才请求重启。"""
    await client.write_gatt_char(CONTROL_UUID, b"reset", response=True)
    for offset in range(0, len(payload), CHUNK_SIZE):
        await client.write_gatt_char(DATA_UUID, payload[offset:offset + CHUNK_SIZE], response=True)
    await client.write_gatt_char(CONTROL_UUID, b"save", response=True)
    status = bytes(await client.read_gatt_char(CONTROL_UUID)).decode("utf-8")
    if status != "saved":
        raise RuntimeError(f"设备未确认保存成功，状态: {status}；未请求重启")
    if reboot:
        await client.write_gatt_char(CONTROL_UUID, b"reboot", response=True)


async def read_config(client) -> dict:
    """读取认证连接上的已保存配置，允许尚未配网设备的空 SSID。"""
    data = bytearray()
    # 显式分块读取，避免完整 JSON 超过单个 GATT 属性长度或默认 MTU。
    while True:
        command = f"read:{len(data)}".encode("ascii")
        await client.write_gatt_char(CONTROL_UUID, command, response=True)
        chunk = bytes(await client.read_gatt_char(DATA_UUID))
        if len(chunk) > 128 or len(data) + len(chunk) > 1024:
            raise RuntimeError("设备返回的配置过长或协议不兼容")
        data.extend(chunk)
        if len(chunk) < 128:
            break
    try:
        config = json.loads(data.decode("utf-8"))
    except (ValueError, UnicodeError):
        raise RuntimeError("设备返回的配置不是有效 JSON，请确认已烧录新版固件") from None
    keys = ("ssid", "password", "static_ip", "gateway", "netmask")
    if not isinstance(config, dict) or any(not isinstance(config.get(k), str) for k in keys):
        raise RuntimeError("设备配置字段不完整，请确认已烧录新版固件")
    return {key: config[key] for key in keys}


def operation_error(error) -> str:
    """将认证拒绝解释为可操作提示，不误报为配置文件写入失败。"""
    detail = str(error)
    if "Insufficient Authentication" in detail or "Insufficient Encryption" in detail:
        return ("蓝牙已连接，但尚未完成加密认证，设备拒绝读写配置。\n"
                "请断开连接，在 Windows 蓝牙设置中删除该设备后重新配对。\n"
                "配对码为 123456；如仍失败，请确认板卡已烧录支持重新配对的新固件。")
    return detail


async def pair_windows(device):
    """Windows 使用支持输入配对码的 WinRT 流程，强制认证加密且不降级。"""
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    from winrt.windows.devices.enumeration import (
        DevicePairingKinds, DevicePairingProtectionLevel, DevicePairingResultStatus)
    address = int(device.address.replace(":", "").replace("-", ""), 16)
    target = await BluetoothLEDevice.from_bluetooth_address_async(address)
    if target is None:
        raise RuntimeError("无法取得 Windows 蓝牙设备，请重新扫描")
    custom = target.device_information.pairing.custom

    def requested(_, args):
        """接受固定 PIN 配对或确认请求，未知配对方式不予接受。"""
        if args.pairing_kind == DevicePairingKinds.PROVIDE_PIN:
            args.accept_with_pin("123456")
        elif args.pairing_kind == DevicePairingKinds.CONFIRM_ONLY:
            args.accept()

    token = custom.add_pairing_requested(requested)
    try:
        result = await asyncio.wait_for(custom.pair_with_protection_level_async(
            DevicePairingKinds.PROVIDE_PIN | DevicePairingKinds.CONFIRM_ONLY,
            DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION), timeout=60)
        if result.status not in (DevicePairingResultStatus.PAIRED,
                                 DevicePairingResultStatus.ALREADY_PAIRED):
            raise RuntimeError("Windows 认证配对失败: " + result.status.name +
                               "；请删除旧配对记录并重试，配对码为 123456")
    finally:
        custom.remove_pairing_requested(token)
        target.close()


class BleWorker:
    """在独立线程维护一个事件循环及一个 BLE 连接，禁止调用 Tk 接口。"""

    def __init__(self, events):
        """创建工作线程，events 用于投递状态、结果与断线消息。"""
        self.events = events
        self.client = None
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, name="BLE配网", daemon=True)
        self.thread.start()

    def _run(self):
        """运行后台事件循环，退出时取消并回收剩余协程。"""
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()
        pending = asyncio.all_tasks(self.loop)
        for task in pending:
            task.cancel()
        if pending:
            self.loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
        self.loop.close()

    def submit(self, action, *args):
        """调度操作，所有结果由主线程消息泵处理。"""
        async def execute():
            try:
                result = await getattr(self, action)(*args)
                self.events.put(("done", action, result))
            except Exception as error:
                # 仅透传协议/系统错误，禁止包含配置正文或密码。
                self.events.put(("error", action, operation_error(error)))
        return asyncio.run_coroutine_threadsafe(execute(), self.loop)

    async def scan(self):
        """扫描十秒，返回广播配网服务的设备与显示名称。"""
        from bleak import BleakScanner
        results = await BleakScanner.discover(timeout=10, return_adv=True)
        return [(device, adv.local_name or device.name or "未命名设备")
                for device, adv in results.values()
                if SERVICE_UUID in [uuid.lower() for uuid in adv.service_uuids]
                or (adv.local_name or "").startswith("ESP32S3-Config-")]

    async def connect(self, device):
        """配对并建立连接，受保护特征认证探测成功后才开放配置读写。"""
        from bleak import BleakClient
        await self.disconnect()
        if sys.platform == "win32":
            await pair_windows(device)
        client = BleakClient(device, pair=sys.platform != "win32", timeout=60,
                             disconnected_callback=lambda _: self.events.put(
                                 ("disconnected", "", None)))
        try:
            await client.connect()
            # 物理连接不等于认证完成；只读探测，不修改配置和保存状态。
            await client.read_gatt_char(CONTROL_UUID)
        except Exception:
            try:
                await client.disconnect()
            except Exception:
                pass
            raise
        self.client = client

    def _connected(self):
        """返回当前有效客户端，断线时拒绝读取、保存和重启。"""
        if self.client is None or not self.client.is_connected:
            raise RuntimeError("设备未连接，请重新连接")
        return self.client

    async def read(self):
        """读取设备配置但不打印凭据。"""
        return await read_config(self._connected())

    async def save(self, payload):
        """保存配置但不重启，界面提示用户另行应用。"""
        await send_config(self._connected(), payload, reboot=False)

    async def reboot(self):
        """确认设备仍有已保存配置后请求重启，避免未确认保存就重启。"""
        client = self._connected()
        status = bytes(await client.read_gatt_char(CONTROL_UUID)).decode("utf-8")
        if status != "saved":
            raise RuntimeError("当前连接尚未确认保存，请先保存配置后再重启")
        await client.write_gatt_char(CONTROL_UUID, b"reboot", response=True)

    async def disconnect(self):
        """释放当前连接，保留系统配对记录。"""
        client, self.client = self.client, None
        if client is not None:
            await client.disconnect()

    async def shutdown(self):
        """关闭窗口时限时断开连接并停止事件循环。"""
        try:
            await asyncio.wait_for(self.disconnect(), timeout=5)
        finally:
            self.loop.call_soon(self.loop.stop)


class WifiConfigWindow:
    """提供设备扫描、连接、配置获取、编辑、保存和重启操作。"""

    def __init__(self, root):
        """构造图形界面并启动非阻塞后台蓝牙任务。"""
        self.root = root
        self.events = queue.Queue()
        self.worker = BleWorker(self.events)
        self.devices = []
        self.connected = False
        self.saved = False
        self.busy = False
        self.closing = False
        root.title("ESP32-S3 · 蓝牙 WiFi 配置")
        root.geometry("740x580")
        root.minsize(680, 550)
        root.protocol("WM_DELETE_WINDOW", self.close)
        frame = ttk.Frame(root, padding=20)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="蓝牙 WiFi 配置", font=("Microsoft YaHei UI", 18, "bold")).pack(anchor="w")
        ttk.Label(frame, text="配对码：123456  |  无需设备连接原 WiFi  |  保存后重启生效").pack(anchor="w", pady=(5, 12))
        ttk.Label(frame, text="固定配对码仅适合可信环境；读取配置包含密码，请勿向他人泄露。",
                  foreground="#a15a00").pack(anchor="w", pady=(0, 10))
        box = ttk.LabelFrame(frame, text="设备", padding=10)
        box.pack(fill="x")
        self.device_choice = ttk.Combobox(box, state="readonly")
        self.device_choice.pack(fill="x", pady=(0, 8))
        bar = ttk.Frame(box)
        bar.pack(fill="x")
        self.buttons = {}
        for key, text, callback in (("scan", "扫描设备", self.scan),
                                    ("connect", "连接 / 配对", self.connect),
                                    ("disconnect", "断开连接", self.disconnect)):
            button = ttk.Button(bar, text=text, command=callback)
            button.pack(side="left", padx=(0, 8))
            self.buttons[key] = button
        form = ttk.LabelFrame(frame, text="网络配置", padding=12)
        form.pack(fill="x", pady=12)
        form.columnconfigure(1, weight=1)
        self.fields = {}
        self.entries = []
        defaults = {"ssid": "", "password": "", "static_ip": "192.168.0.10",
                    "gateway": "192.168.0.1", "netmask": "255.255.255.0"}
        for row, (key, label) in enumerate((("ssid", "WiFi 名称（SSID）"),
                                           ("password", "WiFi 密码"),
                                           ("static_ip", "固定 IP"), ("gateway", "网关"),
                                           ("netmask", "子网掩码"))):
            ttk.Label(form, text=label).grid(row=row, column=0, sticky="w", padx=(0, 16), pady=5)
            variable = tk.StringVar(value=defaults[key])
            entry = ttk.Entry(form, textvariable=variable, show="*" if key == "password" else "")
            entry.grid(row=row, column=1, sticky="ew", pady=5)
            self.fields[key] = variable
            self.entries.append(entry)
            variable.trace_add("write", self.edited)
            if key == "password":
                self.password_entry = entry
        self.show_password = tk.BooleanVar(value=False)
        ttk.Checkbutton(form, text="显示密码", variable=self.show_password,
                        command=lambda: self.password_entry.configure(
                            show="" if self.show_password.get() else "*")).grid(row=1, column=2, padx=8)
        actions = ttk.Frame(frame)
        actions.pack(fill="x")
        for key, text, callback in (("read", "获取配置", self.read),
                                    ("save", "保存配置", self.save),
                                    ("reboot", "重启生效", self.reboot)):
            button = ttk.Button(actions, text=text, command=callback)
            button.pack(side="left", padx=(0, 8))
            self.buttons[key] = button
        self.progress = ttk.Progressbar(frame, mode="indeterminate")
        self.progress.pack(fill="x", pady=(16, 8))
        self.status = tk.StringVar(value="未连接，请先扫描设备。Windows 将使用固定码 123456 完成认证配对。")
        ttk.Label(frame, textvariable=self.status, wraplength=680).pack(anchor="w")
        self.update_controls()
        self.root.after(100, self.poll)

    def edited(self, *_):
        """编辑后禁止把未保存的新内容误认为已应用。"""
        self.saved = False
        self.update_controls()

    def update_controls(self):
        """根据连接与忙碌状态启用按钮，防止并发 BLE 操作。"""
        if not hasattr(self, "buttons"):
            return
        ready = not self.busy and not self.closing
        enabled = {"scan": ready and not self.connected,
                   "connect": ready and not self.connected and bool(self.devices),
                   "disconnect": ready and self.connected,
                   "read": ready and self.connected, "save": ready and self.connected,
                   "reboot": ready and self.connected and self.saved}
        for key, button in self.buttons.items():
            button.configure(state="normal" if enabled.get(key) else "disabled")
        for entry in self.entries:
            entry.configure(state="normal" if ready else "disabled")
        self.device_choice.configure(state="readonly" if ready and not self.connected else "disabled")

    def start_action(self, action, text, *args):
        """提交一个后台操作，界面继续响应且不会由工作线程修改 Tk。"""
        if self.busy or self.closing:
            return
        self.busy = True
        self.status.set(text)
        self.progress.start(12)
        self.update_controls()
        self.worker.submit(action, *args)

    def scan(self):
        """扫描附近设备，结果在消息泵中展示。"""
        self.start_action("scan", "正在扫描 BLE 广播，请稍候…")

    def connect(self):
        """连接所选设备，系统会负责显示配对码输入弹窗。"""
        index = self.device_choice.current()
        if index < 0:
            messagebox.showinfo("选择设备", "请先选择设备。", parent=self.root)
            return
        self.start_action("connect", "正在使用 123456 配对并验证加密认证…", self.devices[index][0])

    def disconnect(self):
        """主动断开连接。"""
        self.start_action("disconnect", "正在断开连接…")

    def read(self):
        """读取前确认覆盖本地编辑，避免无意丢失修改。"""
        if any(v.get() for k, v in self.fields.items() if k in ("ssid", "password")):
            if not messagebox.askyesno("获取配置", "获取设备配置将覆盖表单中的内容，是否继续？", parent=self.root):
                return
        self.start_action("read", "正在获取设备已保存的配置…")

    def save(self):
        """先进行离线校验，再保存到设备，失败时不自动重启。"""
        try:
            payload = encode_config({key: var.get() for key, var in self.fields.items()})
        except ValueError as error:
            messagebox.showerror("配置无效", str(error), parent=self.root)
            return
        self.saved = False
        self.start_action("save", "正在保存配置，请勿断电或断开连接…", payload)

    def reboot(self):
        """明确确认后重启设备，保存与应用分开操作。"""
        if messagebox.askyesno("重启生效", "设备将断开蓝牙并使用新配置连接 WiFi，是否重启？", parent=self.root):
            self.start_action("reboot", "正在请求重启…")

    def poll(self):
        """在 Tk 主线程处理后台结果、异常与主动断线。"""
        if self.closing:
            return
        while True:
            try:
                kind, action, result = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "disconnected":
                self.connected = False
                self.saved = False
                if not self.busy:
                    self.status.set("蓝牙连接已断开，可重新扫描或连接。")
                self.update_controls()
                continue
            self.busy = False
            self.progress.stop()
            if kind == "error":
                if action in ("connect", "disconnect"):
                    self.connected = False
                if action in ("save", "connect", "disconnect"):
                    self.saved = False
                self.status.set("操作失败；请检查设备固件版本、蓝牙状态及配对记录。")
                messagebox.showerror("操作失败", result, parent=self.root)
            elif action == "scan":
                self.devices = result
                self.device_choice["values"] = [f"{name}  [{device.address}]" for device, name in result]
                if result:
                    self.device_choice.current(0)
                else:
                    self.device_choice.set("")
                self.status.set(f"发现 {len(result)} 台设备。请选择后连接。")
            elif action == "connect":
                self.connected = True
                self.saved = False
                self.status.set("已连接并通过认证，可获取配置或编辑后保存。")
            elif action == "disconnect":
                self.connected = False
                self.saved = False
                self.status.set("已断开连接。")
            elif action == "read":
                for key, value in result.items():
                    self.fields[key].set(value)
                self.saved = False
                self.status.set("配置获取成功（含隐藏密码）。这是已保存配置，可能尚未重启应用。")
            elif action == "save":
                self.saved = self.connected
                self.status.set("配置已持久保存。点击“重启生效”应用新配置。")
            elif action == "reboot":
                self.saved = False
                self.status.set("设备将在约 1.5 秒后重启；新 WiFi 是否连通需另行确认。")
            self.update_controls()
        self.root.after(100, self.poll)

    def close(self):
        """关闭时限时清理连接，无论后台操作是否完成都不冻结窗口。"""
        if self.closing:
            return
        self.closing = True
        self.status.set("正在关闭蓝牙连接…")
        self.update_controls()
        self.worker.submit("shutdown")
        self._close_wait(0)

    def _close_wait(self, attempts):
        """最多等待六秒，退出窗口但不阻塞 Tk 事件循环。"""
        if not self.worker.thread.is_alive() or attempts >= 60:
            self.root.destroy()
            return
        self.root.after(100, self._close_wait, attempts + 1)


def run():
    """显示图形界面；依赖缺失以对话框提示，不要求命令行交互。"""
    root = tk.Tk()
    try:
        import bleak  # 只验证依赖，不在 Tk 主线程创建 BLE 客户端。
    except ImportError:
        root.withdraw()
        messagebox.showerror("缺少蓝牙依赖", "请安装 tools/requirements-ble.txt 中的 bleak 后重新启动。", parent=root)
        root.destroy()
        return
    WifiConfigWindow(root)
    root.mainloop()


if __name__ == "__main__":
    run()
