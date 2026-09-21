"""Captured ice-controller wire vectors and the device-library routing contract."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = (ROOT / "Application/DeviceLibrary/IceMachine/CurrentModbus/"
          "ice_machine_modbus.c")
SERVER = (ROOT / "Application/UserAPP/Coffee3CloseApp/Modbus_Tcp_Server/"
          "coffee3_server.c")


def crc16_modbus(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def decode_ice_response(frame, start, count):
    if len(frame) != 2 + 2 + count * 2 + 2:
        raise ValueError("unexpected frame length")
    if frame[0:2] != bytes((1, 3)):
        raise ValueError("wrong unit or function")
    if crc16_modbus(frame[:-2]) != int.from_bytes(frame[-2:], "little"):
        raise ValueError("bad CRC")
    if int.from_bytes(frame[2:4], "big") != start:
        raise ValueError("wrong start-address echo")
    return [int.from_bytes(frame[4 + i * 2:6 + i * 2], "big")
            for i in range(count)]


class IceMachineNonstandardFc03(unittest.TestCase):
    def test_captured_ten_register_response(self):
        request = bytes.fromhex("01 03 00 01 00 0A 94 0D")
        response = bytes.fromhex(
            "01 03 00 01 00 01 00 01 00 00 00 00 00 00 00 00 "
            "00 01 00 00 00 00 00 00 1F FA")
        self.assertEqual(crc16_modbus(request[:-2]), 0x0D94)
        self.assertEqual(crc16_modbus(response[:-2]), 0xFA1F)
        self.assertEqual(decode_ice_response(response, 1, 10),
                         [1, 1, 0, 0, 0, 0, 1, 0, 0, 0])

    def test_captured_thirteen_register_response(self):
        request = bytes.fromhex("01 03 00 01 00 0D D5 CF")
        response = bytes.fromhex(
            "01 03 00 01 00 01 00 01 00 00 00 00 00 00 00 00 "
            "00 01 00 00 00 00 00 00 00 00 00 00 00 00 DA 7B")
        self.assertEqual(crc16_modbus(request[:-2]), 0xCFD5)
        self.assertEqual(crc16_modbus(response[:-2]), 0x7BDA)
        self.assertEqual(decode_ice_response(response, 1, 13),
                         [1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0])
        bad_echo = bytearray(response)
        bad_echo[3] = 2
        bad_echo[-2:] = crc16_modbus(bad_echo[:-2]).to_bytes(2, "little")
        with self.assertRaisesRegex(ValueError, "start-address echo"):
            decode_ice_response(bad_echo, 1, 13)

    def test_only_device_driver_uses_raw_fc03(self):
        source = DRIVER.read_text(encoding="utf-8")
        refresh = source.split("ModbusPortResult_e xIceMachineRefresh(", 1)[1]
        refresh = refresh.split("uint8_t ucIceMachineGetFaultMask(", 1)[0]
        self.assertIn("xModbusPortRawRequest(", refresh)
        self.assertIn("ICE_MACHINE_RESPONSE_DATA_LENGTH", refresh)
        self.assertNotIn("xModbusPortReadHolding(", refresh)
        self.assertIn("aucResponse[0] != aucRequest[0]", refresh)

    def test_fault_names_cover_all_combinations(self):
        source = DRIVER.read_text(encoding="utf-8")
        for reason in ("Water_Inlet", "Water_Level", "Motor_Overload",
                       "Water_Inlet,Water_Level",
                       "Water_Inlet,Motor_Overload",
                       "Water_Level,Motor_Overload",
                       "Water_Inlet,Water_Level,Motor_Overload"):
            self.assertIn('return "' + reason + '";', source)

    def test_host_ice_status_page_is_fully_refreshed(self):
        source = SERVER.read_text(encoding="utf-8")
        self.assertIn("else if (ucIceFaultMask != 0U)", source)
        self.assertIn("s_ausStatusRegisters[0x0070U] = 3U;", source)
        for offset in range(0x71, 0x7F):
            self.assertIn("s_ausStatusRegisters[0x%04XU]" % offset, source)


if __name__ == "__main__":
    unittest.main()
