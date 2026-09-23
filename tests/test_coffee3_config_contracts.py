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

    def test_final_approved_host_writes(self):
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        validator = body(server,
            'static nmbs_error prvValidateHostWrite(uint16_t usAddress, uint16_t usValue,\n\tuint8_t ucOrderPadding)')
        self.assertIn('case 0x001FU:', validator)
        for name in ('COFFEE3_REG_WATER_VALVE_DEBUG',
                     'COFFEE3_REG_UV_LAMP', 'COFFEE3_REG_STORAGE_SELECTOR',
                     'COFFEE3_REG_HOT_CUP_HEIGHT_OFFSET',
                     'COFFEE3_REG_COLD_CUP_HEIGHT_OFFSET',
                     'COFFEE3_REG_ROBOT_TYPE',
                     'COFFEE3_REG_COFFEE_WATER_PUMP',
                     'COFFEE3_REG_COFFEE_PICKUP_TIME',
                     'COFFEE3_REG_AUXILIARY_TANK_PUMP',
                     'COFFEE3_REG_COFFEE_MACHINE_TYPE',
                     'COFFEE3_REG_ICE_COEFFICIENT',
                     'COFFEE3_REG_ICE_MACHINE_TYPE'):
            self.assertIn('case ' + name + ':', validator)
        self.assertTrue({0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF}.issubset(
            {int(x, 16) for x in re.findall(r'case 0x([0-9A-F]+)U:', validator)}))
        self.assertIn('case 0x0023U:', validator)
        self.assertNotIn('storage selector is read-only', server)
        self.assertNotIn('(usAddress <= 0x0032U) && (ulEndAddress > 0x0032U)', server)
        self.assertIn('COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP', server)
        self.assertIn('Coffee water pump debug accepted', server)

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
        self.assertIn('s_xConfig.ulStorageEnabledMask = 0x0003U', code)
        self.assertIn('s_xConfig.ulStorageEnabledMask == usMask', code)
        self.assertIn('xNext.ulStorageEnabledMask = usMask;', code)
        self.assertIn('vTaskSuspendAll()', code)
        self.assertIn('ucAppOtaFlashIsActive()', code)

    def test_single_config_structure_and_fruit_coefficients(self):
        header = read(APP / 'Config/coffee3_config.h')
        code = read(APP / 'Config/coffee3_config.c')
        workflow = read(APP / 'WorkFlow/coffee3_workflow.c')
        self.assertEqual(header.count('} Coffee3Config_t;'), 1)
        self.assertIn('#define COFFEE3_CONFIG_MARK', header)
        self.assertNotIn('CONFIG_V1_MARK', header + code)
        self.assertNotIn('CONFIG_V2', header + code)
        self.assertIn('aulFruitCoefficient[COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT]', header)
        self.assertIn('COFFEE3_CONFIG_FRUIT_COEFFICIENT_DEFAULT 100U', header)
        self.assertIn('COFFEE3_CONFIG_FRUIT_COEFFICIENT_MIN 10U', header)
        self.assertIn('COFFEE3_CONFIG_FRUIT_COEFFICIENT_MAX 1000U', header)
        self.assertIn('xCoffee3ConfigSetFruitCoefficients(', code)
        self.assertIn('xConfigStoreWrite(aulWords, COFFEE3_CONFIG_WORD_COUNT)', code)
        self.assertIn('usCoffee3ConfigFruitCoefficient(ucChannel)', workflow)
        self.assertNotIn('COFFEE3_FRUIT_MILK_MS_PER_ML', workflow)

    def test_ice_slope_is_persisted_and_applied(self):
        header = read(APP / 'Config/coffee3_config.h')
        config = read(APP / 'Config/coffee3_config.c')
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        workflow = read(APP / 'WorkFlow/coffee3_workflow.c')
        self.assertIn('ulIceSlopeMsPerGram;', header)
        self.assertIn('COFFEE3_CONFIG_ICE_SLOPE_DEFAULT_MS_PER_GRAM 10U', header)
        self.assertIn('(3U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT)', config)
        self.assertIn('pulWords[2U + COFFEE3_CONFIG_FRUIT_CHANNEL_COUNT] =', config)
        self.assertIn('xLoaded.ulIceSlopeMsPerGram =', config)
        self.assertIn('xCoffee3ConfigSetIceSlopeMsPerGram(', config)
        self.assertIn('usCoffee3ConfigIceSlopeMsPerGram()', server)
        self.assertIn('xCoffee3ConfigSetIceSlopeMsPerGram(\n\t\t\t\t\tusIceSlope)', server)
        self.assertLess(server.index('xCoffee3WorkflowAcquireManual()'),
                        server.index('xCoffee3ConfigSetIceSlopeMsPerGram('))
        self.assertLess(server.index('xCoffee3ConfigSetIceSlopeMsPerGram('),
                        server.index('vCoffee3WorkflowReleaseManual();'))
        self.assertIn('usSlopeMsPerGram = usCoffee3ConfigIceSlopeMsPerGram();',
                      workflow)
        self.assertIn('(int32_t)usTargetGram * (int32_t)usSlopeMsPerGram;',
                      workflow)
        self.assertNotIn('ice coefficient is accepted but not applied', server)

    def test_ice_pulses_use_coffee1_baseline_and_three_corrections(self):
        settings = read(APP / 'Config/coffee3_app_config.h')
        workflow = read(APP / 'WorkFlow/coffee3_workflow.c')
        for setting in ('COFFEE3_ICE_INITIAL_SPLIT_GRAM        120U',
                        'COFFEE3_ICE_INITIAL_SMALL_MS          800U',
                        'COFFEE3_ICE_INITIAL_LARGE_MS          1200U',
                        'COFFEE3_ICE_CORRECTION_OFFSET_MS      100L',
                        'COFFEE3_ICE_MIN_PULSE_MS              200U',
                        'COFFEE3_ICE_MAX_PULSE_MS              2000U',
                        'COFFEE3_ICE_SETTLE_MS                 1600U',
                        'COFFEE3_ICE_BASELINE_TOLERANCE_GRAM    2L',
                        'COFFEE3_ICE_TOLERANCE_GRAM            15L',
                        'COFFEE3_ICE_MAX_CORRECTIONS           4U'):
            self.assertIn(setting, settings)
        self.assertIn('ucAttempt <= COFFEE3_ICE_MAX_CORRECTIONS', workflow)
        self.assertIn('COFFEE3_ICE_INITIAL_SPLIT_GRAM', workflow)
        self.assertIn('COFFEE3_ICE_CORRECTION_OFFSET_MS', workflow)
        self.assertIn('COFFEE3_ICE_BASELINE_TOLERANCE_GRAM', workflow)
        self.assertIn('COFFEE3_ICE_PULSE_STEP_MS', workflow)
        self.assertIn('Ice done: reason=target reached target=%u g actual=%ld g',
                      workflow)
        self.assertIn('Ice correction: target=%u g actual=%ld g deficit=%ld g',
                      workflow)
        self.assertNotIn('DECIGRAM', settings)

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

    def test_m50_foreground_make_owns_status_and_shared_timeout(self):
        settings = read(APP / 'Config/coffee3_app_config.h')
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        bus = read(APP / 'Modbus_Rtu_Bus/coffee3_rtu_bus.c')
        device = read(APP / 'Device/coffee3_device.c')
        driver = read(
            ROOT / 'Application/DeviceLibrary/CoffeeMachine/coffee_machine_m50.c')
        driver_header = read(
            ROOT / 'Application/DeviceLibrary/CoffeeMachine/coffee_machine_m50.h')
        make = driver[driver.index(
            'if (xAction == COFFEE_MACHINE_M50_ACTION_MAKE)'):
            driver.index('if (xAction == COFFEE_MACHINE_M50_ACTION_CLEAN)')]
        self.assertIn('COFFEE3_COFFEE_ACTION_TIMEOUT_MS      180000U', settings)
        self.assertIn('xCommand.ulTimeoutMs = COFFEE3_COFFEE_ACTION_TIMEOUT_MS;',
                      server)
        self.assertIn('COFFEE_MACHINE_M50_POLL_MISS_LIMIT 3U', driver_header)
        self.assertLess(make.index('prvReadMakeStatus('),
                        make.index('xModbusPortWriteRegister('))
        self.assertEqual(make.count('pxConfig->usMakeRegister'), 1)
        self.assertIn('vCoffee3DeviceImageCommitM50(pxImage)', bus)
        self.assertIn('Coffee state changed: device=%s', bus)
        self.assertIn(
            'pxCommand->ucDeviceId != (uint8_t)COFFEE3_DEVICE_ROBOT',
            device)

    def test_closed_pickup_uses_host_gate_and_physical_cup(self):
        server = read(APP / 'Modbus_Tcp_Server/coffee3_server.c')
        workflow = read(APP / 'WorkFlow/coffee3_workflow.c')
        evaluate = server[server.index(
            'static void prvEvaluateManualCommands(uint16_t usAddress,\n'
            '\tuint16_t usQuantity)\n{'):
            server.index('static void prvLogCompatibilityWrites',
                         server.index(
                             'static void prvEvaluateManualCommands(uint16_t usAddress,\n'
                             '\tuint16_t usQuantity)\n{'))]
        pickup = workflow[workflow.index(
            'static int32_t prvRunStoragePickup(uint16_t usStorage)\n{'):]
        self.assertIn('COFFEE3_REG_ORDER_PRESENT', evaluate)
        self.assertIn('usOutput != 1U', evaluate)
        self.assertIn('xCoffee3WorkflowSubmitStoragePickup(usStorage,',
                      evaluate)
        self.assertLess(pickup.index('usCoffee3ServerGetCommandRegister('),
                        pickup.index('prvRunStep(800U'))
        self.assertIn('aucMB1XPin[usStorage - 1U] == 0U', pickup)
        self.assertNotIn('s_ausStoredOrderId[usStorage - 1U] == 0U', pickup)
        self.assertLess(pickup.index('COFFEE3_ACTION_ROBOT_TAKE_STORAGE'),
                        pickup.index('COFFEE3_ACTION_ROBOT_PUT_OUTPUT'))
        self.assertIn('prvStartDoor(2U)', pickup)

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
            files = []
            for group in target.findall('./Groups/Group'):
                include = group.findtext('./GroupOption/CommonProperty/IncludeInBuild')
                if include == '0':
                    continue
                files.extend(f.text for f in
                    group.findall('./Files/File/FileName'))
            private_expected = 1 if target.findtext('TargetName') == 'Coffee3Close' else 0
            self.assertEqual(files.count('config_store.c'), 1)
            self.assertEqual(files.count('coffee3_config.c'), private_expected)
        cmake = read(ROOT / 'Application/CMakeLists.txt')
        self.assertIn('Common/ConfigStore/config_store.c', cmake)
        self.assertIn('${COFFEE3_APP_ROOT}/Config/coffee3_config.c', cmake)


if __name__ == '__main__':
    unittest.main()
