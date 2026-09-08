"""Source contracts for manual dispatch; not RTOS or hardware simulation."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ManualPriorityContracts(unittest.TestCase):
    def test_both_targets_use_source_not_log_id_for_priority(self):
        for number, folder in ((2, "Coffee2App"), (3, "Coffee3CloseApp")):
            with self.subTest(target=folder):
                app = ROOT / "Application/UserAPP" / folder
                device = (app / f"Device/coffee{number}_device.c").read_text(
                    encoding="utf-8-sig")
                start = device.index("static BaseType_t prvSubmit(", device.index(
                    "uint8_t ucCoffee"))
                submit = device[start:device.index(
                    "void vCoffee", start)]
                self.assertIn(f"COFFEE{number}_COMMAND_SOURCE_SERVER", submit)
                self.assertIn(f"COFFEE{number}_ACTION_REFRESH) ?", submit)
                self.assertIn("ucUrgent =", submit)
                self.assertIn("xQueueSendToFront", submit)
                self.assertNotIn("LOG_ORDER_DEBUG", submit)
                self.assertIn("WorkflowAcquireManual()", submit)
                self.assertIn("WorkflowReleaseManual()", submit)

    def test_status_waiting_is_system_log_only(self):
        for number, folder in ((2, "Coffee2App"), (3, "Coffee3CloseApp")):
            app = ROOT / "Application/UserAPP" / folder
            robot = (app / f"Robot_Tcp/coffee{number}_robot_tcp.c").read_text(
                encoding="utf-8-sig")
            event = robot.index('"ROBOT_ACTION_ACCEPT_WAITING"')
            self.assertIn(f"COFFEE{number}_LOG_ORDER_SYSTEM", robot[event-100:event])
            header = (app / f"Modbus_Tcp_Server/coffee{number}_server.h").read_text(
                encoding="utf-8-sig")
            self.assertRegex(header, r"REG_LOCAL_IO_DEBUG\s+0x0208U")
            self.assertRegex(header, r"REG_EXTERNAL_IO_DEBUG\s+0x0209U")


if __name__ == "__main__":
    unittest.main()
