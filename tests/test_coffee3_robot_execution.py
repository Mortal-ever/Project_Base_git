"""Execute the built ARM robot functions with mocked IO, not a Python FSM.

Build GCC Coffee3Close-Debug first. Test dependencies:
python -m pip install --target tmp/robot-test-deps unicorn pyelftools
python -m unittest discover -s tests -p test_coffee3_robot_execution.py -v

This exercises compiled decision code; it does not simulate lwIP, FreeRTOS
scheduling, the PHY, or the mechanical robot.
"""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tmp/robot-test-deps"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                              UC_ARM_REG_PC)

ELF = ROOT / "GCC-ARM/build/Coffee3Close-Debug/Coffee3CloseTarget.elf"
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


class Firmware:
    def __init__(self):
        built = ELF.stat().st_mtime_ns
        app = ROOT / "Application/UserAPP/Coffee3CloseApp"
        stale = [str(p.relative_to(ROOT)) for p in app.rglob("*")
                 if p.suffix in (".c", ".h") and p.stat().st_mtime_ns > built]
        if stale:
            raise RuntimeError("Rebuild Coffee3Close-Debug before execution tests: "
                               + ", ".join(stale))
        self.stream = ELF.open("rb")
        self.elf = ELFFile(self.stream)
        self.symbols = {s.name: s.entry.st_value for s in
                        self.elf.get_section_by_name(".symtab").iter_symbols()
                        if s.entry.st_value}
        self.types = {}
        for cu in self.elf.get_dwarf_info().iter_CUs():
            name = cu.get_top_DIE().attributes.get("DW_AT_name")
            if name and name.value.decode().replace("\\", "/").endswith(
                    ("/coffee3_robot_tcp.c", "/coffee3_workflow.c",
                     "/coffee3_server.c")):
                for die in cu.iter_DIEs():
                    attr = die.attributes.get("DW_AT_name")
                    if not attr:
                        continue
                    name = attr.value.decode()
                    if die.tag == "DW_TAG_typedef":
                        self.types[name] = die
                    pc = die.attributes.get("DW_AT_low_pc")
                    if pc:
                        self.symbols[name] = pc.value

    def members(self, name):
        die = self.types[name]
        while die.tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                          "DW_TAG_volatile_type"):
            die = die.get_DIE_from_attribute("DW_AT_type")
        return {child.attributes["DW_AT_name"].value.decode():
                child.attributes["DW_AT_data_member_location"].value
                for child in die.iter_children() if child.tag == "DW_TAG_member"}


class RobotExecution(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fw = Firmware()
        cls.tx = cls.fw.members("Coffee3RobotTransaction_t")
        cls.data = cls.fw.members("Coffee3RobotData_t")
        cls.command = cls.fw.members("Coffee3Command_t")
        cls.workflow = cls.fw.members("Coffee3WorkflowStatus_t")

    @classmethod
    def tearDownClass(cls):
        cls.fw.stream.close()

    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for address, size in ((0x08000000, 0x100000), (0x10000000, 0x10000),
                              (0x20000000, 0x20000), (0x30000000, 0x20000)):
            self.cpu.mem_map(address, size)
        for seg in self.fw.elf.iter_segments():
            if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
                self.cpu.mem_write(seg["p_vaddr"], seg.data())
        self.transaction = 0x30000000
        self.done = 0x30001000
        self.stop = 0x080FFFF0
        self.tick = 1000
        self.writes = []
        self.write_result = 0
        self.read_result = 0
        self.read_value = 0
        self.read_values = {}
        self.hooks = {}
        for name in self.fw.symbols:
            if name.startswith(("xCoffee3Log", "vCoffee3Log", "vCoffee3Device")):
                self.stub(name, lambda: 0)
        self.stub("xTaskGetTickCount", lambda: self.tick)
        self.stub("ucCoffee3CommandIsCanceled", lambda: 0)
        self.stub("vPortEnterCritical", lambda: 0)
        self.stub("vPortExitCritical", lambda: 0)
        self.stub("prvRefresh", lambda: 0)
        self.stub("xModbusPortWriteCoil", self.write_coil)
        self.stub("xModbusPortReadCoils", self.read_coils)
        self.cpu.hook_add(UC_HOOK_CODE, self.on_instruction)
        self.put("ucActive", 1, 1)
        self.put("ucCommandWriteAttempted", 1, 1)
        self.put("usCommandCoil", 3136, 2)
        self.put("usResultCoil", 3126, 2)

    def stub(self, name, function):
        if name in self.fw.symbols:
            self.hooks[self.fw.symbols[name] & ~1] = function

    def on_instruction(self, cpu, address, size, unused):
        if address == self.stop:
            cpu.emu_stop()
        elif address in self.hooks:
            result = self.hooks[address]()
            cpu.reg_write(UC_ARM_REG_R0, result & 0xFFFFFFFF)
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def write_coil(self):
        self.writes.append((self.cpu.reg_read(UC_ARM_REG_R2),
                            self.cpu.reg_read(UC_ARM_REG_R3)))
        return self.write_result

    def read_coils(self):
        if self.read_result == 0:
            sp = self.cpu.reg_read(UC_ARM_REG_SP)
            pointer = int.from_bytes(self.cpu.mem_read(sp, 4), "little")
            self.cpu.mem_write(pointer, bytes([self.read_values.get(
                self.cpu.reg_read(UC_ARM_REG_R2), self.read_value)]))
        return self.read_result

    def put(self, member, value, size=4):
        self.cpu.mem_write(self.transaction + self.tx[member],
                           value.to_bytes(size, "little"))

    def get(self, member, size=4):
        return int.from_bytes(self.cpu.mem_read(
            self.transaction + self.tx[member], size), "little")

    def snapshot(self, command, result):
        base = self.fw.symbols["g_xCoffee3RobotData"] + self.data["aucControlCoils"]
        self.cpu.mem_write(base + 36, bytes([command]))
        self.cpu.mem_write(base + 26, bytes([result]))

    def call(self, name, *arguments):
        self.cpu.reg_write(UC_ARM_REG_SP, 0x3001F000)
        self.cpu.reg_write(UC_ARM_REG_LR, self.stop | 1)
        for register, value in zip(REGS, arguments):
            self.cpu.reg_write(register, value)
        self.cpu.emu_start(self.fw.symbols[name] | 1, self.stop,
                           count=200000)
        self.assertEqual(self.cpu.reg_read(UC_ARM_REG_PC), self.stop,
                         "Function did not return within the instruction budget")
        return struct.unpack("<i", struct.pack("<I", self.cpu.reg_read(UC_ARM_REG_R0)))[0]

    def reconcile(self):
        return self.call("prvReconcile", 0, self.transaction, self.done)

    def global_value(self, name, value, size=1):
        self.cpu.mem_write(self.fw.symbols[name], value.to_bytes(size, "little"))

    def workflow_value(self, member, value, size=4):
        self.cpu.mem_write(self.fw.symbols["g_xCoffee3WorkflowStatus"] +
                           self.workflow[member], value.to_bytes(size, "little"))

    def manual_command(self, identity):
        address = 0x30002000 + identity * 64
        for member, value, size in (("ulCommandId", identity, 4),
                                    ("usAction", 119, 2),
                                    ("ausParameter", 1, 2),
                                    ("ucDeviceId", 1, 1),
                                    ("ucSource", 1, 1),
                                    ("ucFlags", 4, 1)):
            self.cpu.mem_write(address + self.command[member],
                               value.to_bytes(size, "little"))
        return address

    def test_pending_debug_waits_for_whole_order_and_preserves_first_request(self):
        self.global_value("s_ucInitializationComplete", 1)
        self.workflow_value("xState", 1)
        first = self.manual_command(1)
        second = self.manual_command(2)
        self.assertEqual(self.call("xCoffee3RobotTcpSubmitManualMotion", first), 1)
        self.assertEqual(self.call("xCoffee3RobotTcpSubmitManualMotion", second), 0)
        self.assertEqual(self.call("prvTakePendingManualMotion", self.done), 0)
        self.workflow_value("xState", 2)
        self.assertEqual(self.call("prvTakePendingManualMotion", self.done), 1)
        self.assertEqual(int.from_bytes(self.cpu.mem_read(self.done, 4), "little"), 1)
        self.assertEqual(self.call("prvTakePendingManualMotion", self.done), 0)

    def test_pickup_and_pending_maintenance_keep_dispatch_closed(self):
        self.global_value("s_ucInitializationComplete", 1)
        self.assertEqual(self.call("xCoffee3RobotTcpSubmitManualMotion",
                                  self.manual_command(1)), 1)
        for name in ("s_ucStoragePickupPending", "s_ucMaintenanceActive",
                     "s_ucManualIcePending", "s_ucOutletPhase"):
            with self.subTest(owner=name):
                self.global_value(name, 1)
                self.assertEqual(self.call("prvTakePendingManualMotion", self.done), 0)
                self.global_value(name, 0)
        self.assertEqual(self.call("prvTakePendingManualMotion", self.done), 1)

    def test_rejected_admission_does_not_consume_pending_slot(self):
        command = self.manual_command(1)
        self.assertEqual(self.call("xCoffee3RobotTcpSubmitManualMotion", command), 0)
        self.global_value("s_ucInitializationComplete", 1)
        self.assertEqual(self.call("xCoffee3RobotTcpSubmitManualMotion", command), 1)

    def test_busy_host_write_returns_exception_without_register_commit(self):
        self.stub("prvSubmitRobotPosition", lambda: 0)
        request = 0x30003000
        self.cpu.mem_write(request, (21).to_bytes(2, "little"))
        registers = self.fw.symbols["s_ausCommandRegisters"]
        self.cpu.mem_write(registers + 0x31 * 2, (123).to_bytes(2, "little"))
        self.assertEqual(self.call("prvCommitWrite", 0x31, 1, request), 4)
        self.assertEqual(int.from_bytes(self.cpu.mem_read(registers + 0x31 * 2, 2),
                                        "little"), 123)

    def test_accepted_host_motion_is_dispatched_once_and_consumed(self):
        submissions = []
        self.stub("prvSubmitRobotPosition", lambda:
                  submissions.append(self.cpu.reg_read(UC_ARM_REG_R0)) or 1)
        self.stub("prvEvaluateOrder", lambda: 0)
        self.stub("prvEvaluateManualCommands", lambda: 0)
        request = 0x30003000
        self.cpu.mem_write(request, (21).to_bytes(2, "little"))
        self.assertEqual(self.call("prvCommitWrite", 0x31, 1, request), 0)
        self.assertEqual(submissions, [21])
        registers = self.fw.symbols["s_ausCommandRegisters"]
        self.assertEqual(bytes(self.cpu.mem_read(registers + 0x31 * 2, 2)), b"\0\0")

    def test_invalid_fc16_is_rejected_before_motion_reservation(self):
        submissions = []
        self.stub("prvSubmitRobotPosition", lambda: submissions.append(1) or 1)
        request = 0x30003000
        self.cpu.mem_write(request, struct.pack("<HH", 99, 21))
        self.assertEqual(self.call("prvCommitWrite", 0x30, 2, request), 3)
        self.assertEqual(submissions, [])

    def test_accepted_zero_zero_keeps_waiting_without_write(self):
        self.put("ucAccepted", 1, 1)
        self.put("xPhase", 3)
        self.snapshot(0, 0)
        self.assertEqual(self.reconcile(), 0)
        self.assertEqual(self.get("xPhase"), 3)
        self.assertEqual(self.writes, [])
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\0")

    def test_command_high_is_not_retriggered(self):
        self.snapshot(1, 0)
        self.assertEqual(self.reconcile(), 0)
        self.assertEqual(self.get("xPhase"), 2)
        self.assertEqual(self.get("ucAccepted", 1), 0)
        self.assertEqual(self.writes, [])

    def test_done_ack_clears_only_own_result(self):
        self.put("ucAccepted", 1, 1)
        self.snapshot(0, 1)
        self.assertEqual(self.reconcile(), 0)
        self.assertEqual(self.writes, [(3126, 0)])
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\1")

    def test_unconfirmed_zero_zero_does_not_invent_acceptance(self):
        self.put("xPhase", 2)
        self.snapshot(0, 0)
        self.reconcile()
        self.assertEqual(self.get("ucAccepted", 1), 0)
        self.assertEqual(self.writes, [])
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\0")

    def test_lost_clear_response_does_not_forget_completion(self):
        self.put("ucAccepted", 1, 1)
        self.snapshot(0, 1)
        self.write_result = -4
        self.assertNotEqual(self.reconcile(), 0)
        self.write_result = 0
        self.put("xPhase", 5)
        self.snapshot(0, 0)
        self.assertEqual(self.reconcile(), 0)
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\1")

    def test_lost_clear_readback_preserves_completion_across_recovery(self):
        self.put("ucAccepted", 1, 1)
        self.snapshot(0, 1)
        self.read_result = -4
        self.assertNotEqual(self.reconcile(), 0)
        self.read_result = 0
        self.put("xPhase", 5)
        self.snapshot(0, 0)
        self.assertEqual(self.reconcile(), 0)
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\1")

    def test_conflicting_bits_do_not_clear_evidence_or_claim_done(self):
        self.snapshot(1, 1)
        self.reconcile()
        self.assertEqual(self.writes, [])
        self.assertEqual(self.get("ucAccepted", 1), 0)
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\0")

    def test_ambiguous_reconcile_cannot_be_promoted_by_next_online_poll(self):
        self.put("ucCommandWriteAttempted", 1, 1)
        self.put("xAcceptDeadline", 10000)
        self.put("xPhase", 2)
        self.snapshot(0, 0)
        self.reconcile()
        self.call("prvAdvanceAction", 0, self.transaction, self.done)
        self.assertEqual(self.get("ucAccepted", 1), 0)
        self.assertEqual(self.writes, [])

    def test_fast_completion_latches_evidence_on_first_accept_poll(self):
        self.put("ucCommandWriteAttempted", 1, 1)
        self.put("ucCommandWriteConfirmed", 1, 1)
        self.put("xAcceptDeadline", 10000)
        self.put("xPhase", 2)
        self.read_values = {3136: 0, 3126: 1}
        self.call("prvAdvanceAction", 0, self.transaction, self.done)
        self.assertEqual(self.get("ucCompletionObserved", 1), 1)
        self.assertEqual(self.get("xPhase"), 4)

    def test_acceptance_deadline_is_bounded_without_retrigger(self):
        self.put("ucCommandWriteAttempted", 1, 1)
        self.put("ucCommandWriteConfirmed", 1, 1)
        self.put("xAcceptDeadline", 1000)
        self.put("xPhase", 2)
        self.read_values = {3136: 1, 3126: 0}
        self.assertEqual(self.call("prvAdvanceAction", 0, self.transaction, self.done), -4)
        self.assertEqual(bytes(self.cpu.mem_read(self.done, 1)), b"\1")
        self.assertEqual(self.writes, [])

    def test_motion_deadline_handles_tick_wrap(self):
        self.put("ucCommandWriteAttempted", 1, 1)
        self.put("ucAccepted", 1, 1)
        self.put("xMotionDeadline", 100)
        self.put("xPhase", 3)
        self.tick = 0xFFFFFFF0
        self.assertEqual(self.call("prvAdvanceAction", 0, self.transaction, self.done), -3)
        self.tick = 100
        self.assertEqual(self.call("prvAdvanceAction", 0, self.transaction, self.done), -4)

    def test_five_prepare_retries_have_three_second_spacing(self):
        self.put("ucCommandWriteAttempted", 0, 1)
        for retry in range(1, 6):
            self.assertEqual(self.call("prvSchedulePrepareRetry", self.transaction,
                                      0xFFFFFFFA), -3)
            self.assertEqual(self.get("ucPrepareRetryCount", 1), retry)
            self.assertEqual(self.get("xNextPrepareRetryTick"), self.tick + 3000)
            self.tick += 3000
        self.assertEqual(self.call("prvSchedulePrepareRetry", self.transaction,
                                  0xFFFFFFFA), -4)
        self.assertEqual(self.get("ucPrepareRetryCount", 1), 5)

    def test_attempted_write_is_never_scheduled_for_republication(self):
        self.put("ucCommandWriteAttempted", 1, 1)
        self.assertEqual(self.call("prvSchedulePrepareRetry", self.transaction,
                                  0xFFFFFFFA), -6)
        self.assertEqual(self.get("ucPrepareRetryCount", 1), 0)


if __name__ == "__main__":
    unittest.main()
