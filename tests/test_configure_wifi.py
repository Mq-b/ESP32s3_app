"""配网客户端离线测试，不需要安装 bleak 或连接真实设备。"""

import importlib.util
from importlib.machinery import SourceFileLoader
import json
import unittest
from unittest.mock import AsyncMock, Mock, patch
from types import SimpleNamespace
import queue
from pathlib import Path

script = Path(__file__).resolve().parents[1] / "tools" / "configure_wifi.pyw"
spec = importlib.util.spec_from_loader("configure_wifi", SourceFileLoader("configure_wifi", str(script)))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def config(**changes):
    """构造不含真实凭据的测试配置。"""
    result = dict(ssid="测试网络", password='test"\\pass', static_ip="192.168.0.10",
                  gateway="192.168.0.1", netmask="255.255.255.0")
    result.update(changes)
    return result


class ValidationTests(unittest.TestCase):
    """验证完整配置、地址边界、转义及敏感信息隔离。"""

    def test_round_trip(self):
        self.assertEqual(json.loads(module.encode_config(config())), config())

    def test_supported_passwords(self):
        for password in ("", "12345678", "a" * 63, "a" * 64):
            module.encode_config(config(password=password))

    def test_invalid_fields(self):
        cases = [dict(ssid=""), dict(ssid="中" * 11), dict(password="short"),
                 dict(password="g" * 64), dict(password="a" * 65), dict(ssid="bad\0ssid"),
                 dict(gateway="192.168.1.1"), dict(static_ip="192.168.0.0"),
                 dict(static_ip="192.168.0.255"), dict(static_ip="192.168.0.1"),
                 dict(static_ip="127.0.0.2", gateway="127.0.0.1"),
                 dict(netmask="255.0.255.0"), dict(netmask="255.255.255.254"),
                 dict(netmask="0.0.0.0"), dict(static_ip="1.2.3")]
        for changes in cases:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                module.encode_config(config(**changes))

    def test_missing_field(self):
        data = config()
        del data["password"]
        with self.assertRaises(ValueError):
            module.encode_config(data)


class FakeClient:
    """记录 GATT 请求，模拟保存结果与写入异常。"""

    def __init__(self, status=b"saved", fail_data=False):
        self.status = status
        self.fail_data = fail_data
        self.writes = []

    async def write_gatt_char(self, uuid, data, response):
        if self.fail_data and uuid == module.DATA_UUID:
            raise RuntimeError("模拟断线")
        self.writes.append((uuid, data, response))

    async def read_gatt_char(self, uuid):
        return self.status


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    """验证带响应分包、保存确认以及失败时禁止自动重启。"""

    async def test_upload_and_reboot(self):
        client = FakeClient()
        payload = module.encode_config(config())
        await module.send_config(client, payload)
        self.assertEqual(client.writes[0][1], b"reset")
        chunks = [data for uuid, data, response in client.writes if uuid == module.DATA_UUID]
        self.assertEqual(b"".join(chunks), payload)
        self.assertTrue(all(len(chunk) <= 20 for chunk in chunks))
        self.assertTrue(all(response for _, _, response in client.writes))
        self.assertEqual([w[1] for w in client.writes[-2:]], [b"save", b"reboot"])

    async def test_no_reboot(self):
        client = FakeClient()
        await module.send_config(client, module.encode_config(config()), reboot=False)
        self.assertNotIn(b"reboot", [w[1] for w in client.writes])

    async def test_save_failure(self):
        client = FakeClient(status=b"error:storage")
        with self.assertRaises(RuntimeError):
            await module.send_config(client, module.encode_config(config()))
        self.assertNotIn(b"reboot", [w[1] for w in client.writes])

    async def test_disconnect(self):
        client = FakeClient(fail_data=True)
        with self.assertRaises(RuntimeError):
            await module.send_config(client, module.encode_config(config()))
        self.assertNotIn(b"save", [w[1] for w in client.writes])
        self.assertNotIn(b"reboot", [w[1] for w in client.writes])


class ReadClient:
    """模拟显式偏移分块读取，默认 ATT MTU 下也支持完整配置。"""

    def __init__(self, payload):
        self.payload = payload
        self.offset = 0
        self.offsets = []

    async def write_gatt_char(self, uuid, data, response):
        assert uuid == module.CONTROL_UUID and response
        self.offset = int(data.decode("ascii").split(":")[1])
        self.offsets.append(self.offset)

    async def read_gatt_char(self, uuid):
        assert uuid == module.DATA_UUID
        return self.payload[self.offset:self.offset + 128]


class ReadTests(unittest.IsolatedAsyncioTestCase):
    """验证配置回读、空 SSID、UTF-8 分包和畸形数据拒绝。"""

    async def test_read_round_trip(self):
        expected = config(ssid="中" * 10, password="a" * 63)
        client = ReadClient(module.encode_config(expected))
        self.assertEqual(await module.read_config(client), expected)
        self.assertGreater(len(client.offsets), 1)

    async def test_unconfigured(self):
        expected = config(ssid="", password="")
        payload = json.dumps(expected).encode("utf-8")
        self.assertEqual(await module.read_config(ReadClient(payload)), expected)

    async def test_exact_chunk_boundary(self):
        payload = json.dumps(config()).encode("utf-8")
        payload += b" " * ((-len(payload)) % 128)
        client = ReadClient(payload)
        self.assertEqual(await module.read_config(client), config())
        self.assertEqual(client.offsets[-1], len(payload))

    async def test_invalid_read(self):
        for payload in (b"invalid", b"{}", b"[]", b"\xff", b"a" * 1025):
            with self.subTest(payload_size=len(payload)), self.assertRaises(RuntimeError):
                await module.read_config(ReadClient(payload))


class AuthenticationTests(unittest.IsolatedAsyncioTestCase):
    """验证连接必须通过认证探测，失败时清理连接且不开放读写。"""

    async def test_connected_only_after_probe(self):
        client = SimpleNamespace(connect=AsyncMock(), disconnect=AsyncMock(),
                                 read_gatt_char=AsyncMock(return_value=b"idle"))
        worker = object.__new__(module.BleWorker)
        worker.client = None
        worker.events = queue.Queue()
        pair = AsyncMock()
        with patch.object(module.sys, "platform", "win32"), \
                patch.object(module, "pair_windows", pair), \
                patch.dict("sys.modules", {"bleak": SimpleNamespace(BleakClient=Mock(return_value=client))}):
            await worker.connect(SimpleNamespace(address="AA:BB:CC:DD:EE:FF"))
        pair.assert_awaited_once()
        client.read_gatt_char.assert_awaited_once_with(module.CONTROL_UUID)
        self.assertIs(worker.client, client)

    async def test_authentication_failure_disconnects(self):
        client = SimpleNamespace(connect=AsyncMock(), disconnect=AsyncMock(),
                                 read_gatt_char=AsyncMock(side_effect=RuntimeError(
                                     "GATT Protocol Error: Insufficient Authentication")))
        worker = object.__new__(module.BleWorker)
        worker.client = None
        worker.events = queue.Queue()
        with patch.object(module.sys, "platform", "win32"), \
                patch.object(module, "pair_windows", AsyncMock()), \
                patch.dict("sys.modules", {"bleak": SimpleNamespace(BleakClient=Mock(return_value=client))}):
            with self.assertRaises(RuntimeError):
                await worker.connect(SimpleNamespace(address="AA:BB:CC:DD:EE:FF"))
        self.assertIsNone(worker.client)
        client.disconnect.assert_awaited_once()

    def test_authentication_error_message(self):
        message = module.operation_error(RuntimeError("Insufficient Authentication"))
        self.assertIn("123456", message)
        self.assertIn("删除", message)
        self.assertEqual(module.operation_error(RuntimeError("模拟普通错误")), "模拟普通错误")

    async def test_windows_pin_and_no_security_downgrade(self):
        # Windows 本机 WinRT 仅验证协议调用，不执行真实配对。
        try:
            from winrt.windows.devices.enumeration import (
                DevicePairingKinds, DevicePairingProtectionLevel, DevicePairingResultStatus)
            from winrt.windows.devices import bluetooth
        except ImportError:
            self.skipTest("当前环境没有 Windows WinRT 依赖")
        custom = SimpleNamespace(add_pairing_requested=Mock(return_value=7),
                                 remove_pairing_requested=Mock(),
                                 pair_with_protection_level_async=AsyncMock(return_value=SimpleNamespace(
                                     status=DevicePairingResultStatus.PAIRED)))
        target = SimpleNamespace(device_information=SimpleNamespace(
            pairing=SimpleNamespace(custom=custom)), close=Mock())
        factory = SimpleNamespace(from_bluetooth_address_async=AsyncMock(return_value=target))
        with patch.object(bluetooth, "BluetoothLEDevice", factory):
            await module.pair_windows(SimpleNamespace(address="AA:BB:CC:DD:EE:FF"))
        custom.pair_with_protection_level_async.assert_awaited_once_with(
            DevicePairingKinds.PROVIDE_PIN | DevicePairingKinds.CONFIRM_ONLY,
            DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
        handler = custom.add_pairing_requested.call_args.args[0]
        args = SimpleNamespace(pairing_kind=DevicePairingKinds.PROVIDE_PIN, accept_with_pin=Mock())
        handler(None, args)
        args.accept_with_pin.assert_called_once_with("123456")
        custom.remove_pairing_requested.assert_called_once_with(7)
        target.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
