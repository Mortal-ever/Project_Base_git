"""Source/build contracts only; these do not simulate Flash or a robot."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'Application/UserAPP/Coffee3CloseApp'


def read(path):
    return path.read_text(encoding='utf-8-sig')


def body(text, signature):
    start = text.index(signature + '\n{')
    end = text.index('\n}', start)
    return text[start:end]


class ConfigContracts(unittest.TestCase):
    def test_host_write_support_boundaries(self):
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        validator = body(server,
            'static nmbs_error prvValidateHostWrite(uint16_t usAddress, uint16_t usValue,\n\tuint8_t ucOrderPadding)')
        supported = {int(x, 16) for x in re.findall(r'case 0x([0-9A-F]+)U:', validator)}
        self.assertTrue({146, 157, 0x1A, 0x33, 520, 521}.issubset(supported))
        self.assertTrue({145, 147, 148, 153, 154, 155, 156}.isdisjoint(supported))
        commit = body(server,
            'static nmbs_error prvCommitWrite(uint16_t usAddress,\n\tuint16_t usQuantity, const uint16_t *pusRegisters)')
        self.assertLess(commit.index('prvValidateHostWrite('),
                        commit.index('xCoffee3WorkflowSubmitDoorDebug('))
        self.assertLess(commit.index('prvValidateHostWrite('),
                        commit.index('memcpy('))
        self.assertIn('Command rejected: address=%u value=%u; %s', server)

    def test_single_outlet_manual_and_protocol(self):
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        code = body(server,
                    'static uint8_t prvSubmitRobotPosition(uint16_t usPosition)')
        cases = {int(x, 16) for x in re.findall(r'case 0x([0-9A-F]+)U:', code)}
        self.assertIn(17, cases)
        self.assertTrue({10, 14, 18, 19, 20}.isdisjoint(cases))
        robot = read(APP / 'Robot_Tcp/coffee3_robot_tcp.c')
        self.assertIn('COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, DOBOT_ROBOT_P1_COMMAND_PUT_OUTPUT, DOBOT_ROBOT_P1_RESULT_PUT_OUTPUT', robot)
        self.assertNotIn('{ COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2,', robot)
        self.assertIn('s_ausCommandRegisters[COFFEE3_REG_ONLINE_OUTPUT] = 1U;', server)
        self.assertIn('s_ausCommandRegisters[0x0033U] = 1U;', server)
        self.assertNotIn('(usAddress <= 0x0033U) && (ulEndAddress > 0x0032U)', server)
        selector = robot.index('vCoffee3ServerSelectOutlet();')
        self.assertIn('ROBOT_OUTPUT_ROUTE_PUBLISHED', robot[selector:selector + 400])
        self.assertGreater(robot.index('xResult = prvClearActionCoils', selector), selector)

    def test_exact_manual_positions(self):
        code = body(read(APP / 'Modbus_Tcp_Server/coffee3_server.c'),
                    'static uint8_t prvSubmitRobotPosition(uint16_t usPosition)')
        cases = {int(x, 16) for x in re.findall(r'case 0x([0-9A-F]+)U:', code)}
        self.assertEqual({x for x in cases if x >= 21}, {21, 22})
        self.assertIn('COFFEE3_ACTION_ROBOT_PUT_STORAGE', code)
        self.assertIn('usPosition - 0x0014U', code)
        self.assertIn('MANUAL_ROBOT_POSITION_UNSUPPORTED', code)
        self.assertNotIn('ConfigStorageMask', code)

    def test_startup_before_business(self):
        code = read(APP / 'Task_Manager/coffee3_manager.c')
        self.assertLess(code.index('xCoffee3ConfigInitialize()'),
                        code.index('xCoffee3DeviceInitialize()'))
        self.assertIn('configuration initialization failed; startup blocked', code)

    def test_full_mask_and_same_value(self):
        code = read(APP / 'Config/coffee3_config.c')
        self.assertIn('aulWords[1] <= 0xFFFFUL', code)
        self.assertIn('xCoffee3ConfigSetStorageMask(0x0003U)', code)
        self.assertIn('s_xConfig.ulStorageEnabledMask == usMask', code)
        self.assertIn('aulWords[1] = usMask;', code)
        self.assertIn('vTaskSuspendAll()', code)
        self.assertIn('ucAppOtaFlashIsActive()', code)

    def test_persist_before_register_publication(self):
        code = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        code = code[code.index('/* Persist before publishing'):]
        self.assertLess(code.index('xCoffee3ConfigSetStorageMask('),
                        code.index('memcpy(&s_ausCommandRegisters'))
        self.assertLess(code.index('NMBS_EXCEPTION_SERVER_DEVICE_FAILURE'),
                        code.index('prvEvaluateManualCommands('))
        self.assertIn('pusRegisters[\n\t\t\t\tCOFFEE3_CONFIG_STORAGE_REGISTER - usAddress]', code)

    def test_automatic_mask_only_two_installed_slots(self):
        code = body(read(APP / 'WorkFlow/coffee3_workflow.c'),
                    'static int32_t prvSelectStorage(void)')
        self.assertIn('COFFEE3_STORAGE_INSTALLED_MASK', code)
        self.assertIn('usEnabled & (1U << ucIndex)', code)
        self.assertIn('aucMB1XPin[ucIndex] == 0U', code)
        self.assertIn('ucIndex < 2U', code)

    def test_flash_bounds_and_commit_last(self):
        header = read(ROOT / 'Application/Common/ConfigStore/config_store.h')
        code = read(ROOT / 'Application/Common/ConfigStore/config_store.c')
        self.assertIn('0x08008000UL', header)
        self.assertIn('0x00004000UL', header)
        self.assertIn('xErase.Sector = FLASH_SECTOR_2;', code)
        self.assertIn('xErase.NbSectors = 1U;', code)
        self.assertLess(code.index('CONFIG_STORE_ADDRESS + ulIndex * 4U'),
                        code.index('CONFIG_STORE_ADDRESS, pulWords[0]'))

    def test_build_scope(self):
        project = ET.parse(ROOT / 'MDK-ARM/STM32F407_Base.uvprojx')
        for target in project.findall('.//Targets/Target'):
            files = [f.text for f in target.findall('./Groups/Group/Files/File/FileName')]
            expected = 1 if target.findtext('TargetName') == 'Coffee3Close' else 0
            self.assertEqual(files.count('config_store.c'), expected)
            self.assertEqual(files.count('coffee3_config.c'), expected)
        cmake = read(ROOT / 'Application/CMakeLists.txt')
        self.assertIn('Common/ConfigStore/config_store.c', cmake)
        self.assertIn('${COFFEE3_APP_ROOT}/Config/coffee3_config.c', cmake)


if __name__ == '__main__':
    unittest.main()
