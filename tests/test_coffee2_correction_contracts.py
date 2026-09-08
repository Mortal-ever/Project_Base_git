"""Static regression contracts for Coffee2; not a hardware/RTOS simulation.

Run: python -m unittest discover -s tests -p test_coffee2_correction_contracts.py -v
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Application/UserAPP/Coffee2App"


def read(path):
    return path.read_text(encoding="utf-8-sig")


def body(source, name):
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError("Definition missing: " + name)
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise AssertionError("Unbalanced function: " + name)


class Coffee2Contracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = read(APP / "WorkFlow/coffee2_workflow.c")
        cls.server = read(APP / "Modbus_Tcp_Server/coffee2_server.c")
        cls.device = read(APP / "Device/coffee2_device.c")
        cls.bus = read(APP / "Modbus_Rtu_Bus/coffee2_rtu_bus.c")

    def test_outlet_state_is_not_server_owned(self):
        self.assertNotIn("s_ausOutputState", self.server)
        self.assertNotIn("ausOutputState", body(self.server, "prvEvaluateOrder"))
        self.assertNotIn("[0x000BU] = 0U", body(self.server, "vCoffee2ServerPublishOrder"))
        self.assertNotIn("[0x000CU] = 0U", body(self.server, "vCoffee2ServerPublishOrder"))

    def test_pickup_ack_is_only_accepted_after_placement(self):
        ack = body(self.workflow, "vCoffee2WorkflowConfirmPickup")
        self.assertIn("ausOutputState[usOutput - 1U] == 5U", ack)
        self.assertIn("s_ucPickupPending", ack)
        self.assertNotIn("= 0x10U", ack)
        self.assertIn("= 0x10U", body(self.workflow, "prvServicePickup"))

    def test_outlet_preflight_and_final_recheck(self):
        order = body(self.workflow, "prvRunOrder")
        self.assertEqual(order.count("prvCheckOutputEmpty(usOutput)"), 2)
        self.assertIn("prvRefreshDeviceQuiet", body(self.workflow, "prvCheckOutputEmpty"))
        self.assertEqual(order.count("prvPublishOutput(usOutput, 3U)"), 2)

    def test_order_projection_is_owned_by_workflow(self):
        self.assertNotIn("s_ausStatusRegisters", body(self.server, "prvEvaluateOrder"))
        task = body(self.workflow, "vCoffee2WorkflowTask")
        self.assertLess(task.index("vCoffee2ServerPublishOrder(&xOrder)"),
                        task.index("prvRunOrder(&xOrder)"))

    def test_safety_requires_correlated_zero_result(self):
        stop = body(self.workflow, "prvAbortDevices")
        self.assertIn("lCoffee2DeviceGetTerminalResult", stop)
        self.assertIn("lStopResult != 0", stop)
        self.assertIn("ucValid == 0U", stop)
        self.assertIn("COFFEE2_COMMAND_FLAG_SAFETY_STOP", stop)

    def test_cancel_exemption_does_not_allow_turn_on(self):
        canceled = body(self.device, "ucCoffee2CommandIsCanceled")
        self.assertIn("COFFEE2_COMMAND_FLAG_SAFETY_STOP", canceled)
        self.assertIn("ausParameter[0] == 0U", canceled)
        self.assertIn("ausParameter[1] == 0U", canceled)
        self.assertNotIn("IO_WRITE_MASK", canceled)
        self.assertIn("ulOrderEpoch == s_ulCanceledOrderEpoch", canceled)

    def test_bus_checks_cancellation_at_retry_and_send(self):
        self.assertGreaterEqual(self.bus.count("ucCoffee2CommandIsCanceled(&xCommand)"), 2)

    def test_terminal_event_is_only_a_wakeup(self):
        wait = body(self.device, "xCoffee2DeviceWaitCommand")
        self.assertNotIn("return xBits", wait)
        self.assertIn("vTaskDelay(1U)", wait)
        self.assertIn("ulOrderEpoch", wait)
        self.assertIn("ulCommandId", wait)

    def test_manual_reservation_covers_queue_lifetime(self):
        submit = body(self.device, "prvSubmit")
        self.assertIn("xCoffee2WorkflowAcquireManual", submit)
        self.assertIn("vCoffee2WorkflowReleaseManual", submit)
        self.assertIn("vCoffee2WorkflowReleaseManual",
                      body(self.device, "vCoffee2DeviceCommandCompleted"))
        self.assertIn("s_usManualReservations != 0U",
                      body(self.workflow, "xCoffee2WorkflowSubmitOrder"))

    def test_debug_bitmap_is_one_command_not_sixteen_queue_entries(self):
        debug = body(self.server, "prvCommitIoDebugWrite")
        self.assertEqual(debug.count("prvSubmitManual("), 1)
        self.assertIn("COFFEE2_ACTION_IO_WRITE_MASK", debug)
        self.assertNotIn("usChanged", debug)
        self.assertIn("xIoModuleModbusWriteOutput", body(self.bus, "prvExecute"))

    def test_fc16_debug_prevalidation_and_ownership(self):
        debug = body(self.server, "prvCommitIoDebugWriteRange")
        self.assertLess(debug.index("0xFF00U"), debug.index("prvCommitIoDebugWrite("))
        self.assertNotIn("xCoffee2WorkflowAcquireManual", debug)
        self.assertNotIn("ucCoffee2OtaHttpIsActive", debug)
        self.assertIn("usIndex = usQuantity", debug)

    def test_io_protocol_error_does_not_publish_unread_zero_image(self):
        execute = body(self.bus, "prvExecute")
        self.assertEqual(execute.count(
            "xIoImage.ucPointCount == COFFEE2_EXTERNAL_IO_POINT_COUNT"), 2)

    def test_manual_ice_handoff_is_atomic(self):
        task = body(self.workflow, "vCoffee2WorkflowTask")
        handoff = task[task.index("if (s_ucManualIcePending != 0U)"):]
        self.assertLess(handoff.index("s_ucMaintenanceActive = 1U"),
                        handoff.index("taskEXIT_CRITICAL()"))
        self.assertIn("s_ucInitializationComplete == 0U",
                      body(self.workflow, "xCoffee2WorkflowAcquireManual"))

    def test_recovery_latch_cannot_be_cleared_by_ack(self):
        ack = body(self.workflow, "vCoffee2WorkflowAcknowledgeAlarm")
        self.assertIn("ucRecoveryRequired == 0U", ack)
        self.assertNotIn("ucRecoveryRequired = 0U", ack)
        self.assertIn("ucRecoveryRequired != 0U",
                      body(self.workflow, "xCoffee2WorkflowSubmitOrder"))
        self.assertIn("ucRecoveryRequired == 0U",
                      body(self.workflow, "xCoffee2WorkflowAcquireManual"))

    def test_maintenance_reserves_automatic_owner(self):
        submit = body(self.workflow, "xCoffee2WorkflowSubmitMaintenance")
        self.assertIn("ucOrderAdmissionOpen = 0U", submit)
        self.assertIn("s_usManualReservations == 0U", submit)
        self.assertIn("s_ucMaintenanceActive != 0U",
                      body(self.workflow, "prvWaitDeviceReportedComplete"))

    def test_syrup_without_stop_is_not_claimed_safe(self):
        self.assertIn("Syrup action unresolved", body(self.workflow, "prvAbortDevices"))
        self.assertIn("s_usActiveDevices &=", body(self.workflow, "prvWaitDeviceReportedComplete"))

    def test_public_callbacks_remain_target_neutral(self):
        for relative in ["CoffeeMachine/coffee_machine_f200.c",
                         "SyrupMachine/CurrentModbus/syrup_machine_modbus.c"]:
            source = read(ROOT / "Application/DeviceLibrary" / relative)
            self.assertIn("pxCancelCheck(pvCancelContext)", source)
            self.assertNotIn("coffee2_", source.lower())
            self.assertNotIn("UserAPP", source)

    def test_io_read_window_keeps_all_sixteen_registers(self):
        header = read(APP / "Modbus_Tcp_Server/coffee2_server.h")
        for address in range(0x10F0, 0x1100):
            self.assertIn(f"0x{address:04X}U", header)
        self.assertRegex(header, r"COFFEE2_SERVER_STATUS_COUNT\s+0x0100U")

    def test_private_device_table_is_unique(self):
        header = read(APP / "Config/coffee2_device_bindings.h")
        self.assertEqual(header.count("{ COFFEE2_DEVICE_"), 10)
        self.assertNotIn("static const Coffee2DeviceBinding_t s_axBindings", self.device)
        self.assertIn('#include "coffee2_device_bindings.h"', self.device)

    def test_keil_paths_and_only_coffee2_target(self):
        project = ROOT / "MDK-ARM/STM32F407_Base.uvprojx"
        tree = ET.parse(project)
        targets = [t.text for t in tree.findall("./Targets/Target/TargetName")]
        self.assertIn("Coffee2Open", targets)
        self.assertIn("Coffee3Close", targets)
        for entry in tree.findall(".//FilePath"):
            path = project.parent / entry.text.replace("\\", "/")
            self.assertTrue(path.is_file(), str(path))

    def test_coffee3_target_uses_m50_and_private_app(self):
        project = read(ROOT / "MDK-ARM/STM32F407_Base.uvprojx")
        self.assertIn("Coffee3Close", project)
        self.assertIn("coffee_machine_m50.c", project)
        self.assertIn("coffee3_workflow.c", project)
        self.assertIn("Coffee3Close_CCM.sct", project)
        self.assertIn("<FileName>coffee3_manager.c</FileName>",
                      project[project.index("<TargetName>Coffee3Close") :])

    def test_coffee3_workflow_owns_outlet_and_m50_completion(self):
        workflow = read(ROOT / "Application/UserAPP/Coffee3CloseApp/WorkFlow/coffee3_workflow.c")
        self.assertIn("M50 tank fill stopped", workflow)
        self.assertIn("Outlet X3 empty for 30 s", workflow)
        self.assertIn("xCoffee3WorkflowAcquireOta", workflow)
        self.assertIn("COFFEE3_COFFEE_ACTION_TIMEOUT_MS", workflow)


if __name__ == "__main__":
    unittest.main()
