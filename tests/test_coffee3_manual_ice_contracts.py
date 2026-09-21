"""Source-level regression checks for the Coffee3 manual ice entry path."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Application/UserAPP/Coffee3CloseApp"


class ManualIceContracts(unittest.TestCase):
    def test_host_register_and_workflow_use_grams(self):
        server = (APP / "Modbus_Tcp_Server/coffee3_server.c").read_text(
            encoding="utf-8"
        )
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        header = (APP / "WorkFlow/coffee3_workflow.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("Target weight in grams from register 0x0071", header)
        self.assertIn('"target_g"', server)
        self.assertNotIn("usTargetGrams > (uint16_t)(0xFFFFU / 10U)", workflow)
        self.assertIn("s_usManualIceWeight = usTargetGrams", workflow)
        self.assertIn("prvDispenseIce(usManualIceWeight,", workflow)
        self.assertIn("prvDispenseIce(usIceAmount,", workflow)
        self.assertNotIn("prvDispenseIce((uint16_t)(usIceAmount * 10U))", workflow)

    def test_coffee3_scale_samples_use_whole_grams(self):
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        scale_header = (
            ROOT / "Application/DeviceLibrary/Scale/BSQ_DG_V2/scale_bsq_dg_v2.h"
        ).read_text(encoding="utf-8")
        self.assertIn("int32_t lWeightGram;", scale_header)
        self.assertIn("g_xCoffee3ScaleImage.lWeightGram", workflow)
        self.assertIn("Ice weight: try=%u/%u target=%u g actual=%ld g", workflow)
        self.assertIn("total=%ld g last=%d", workflow)

    def test_successful_rtu_refresh_publishes_ready(self):
        bus = (APP / "Modbus_Rtu_Bus/coffee3_rtu_bus.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("xCommand.usAction == (uint16_t)COFFEE3_ACTION_REFRESH", bus)
        self.assertIn("vCoffee3DeviceSetReady(\n\t\t\t\t(Coffee3DeviceId_e)xCommand.ucDeviceId, 1U)", bus)
        self.assertIn("prvPublishPollHealth(pxContext, pxConfig, &xCommand, xResult)", bus)

    def test_manual_ice_checks_cup_instead_of_assuming_it(self):
        server = (APP / "Modbus_Tcp_Server/coffee3_server.c").read_text(
            encoding="utf-8"
        )
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        self.assertIn('"ice_not_ready"', workflow)
        self.assertIn('"scale_not_ready"', workflow)
        self.assertIn('"Manual ice rejected: %s, ready=0x%04X, target=%u g"', workflow)
        self.assertNotIn('"MANUAL_CUP_ASSUMED"', server)
        self.assertIn("prvConfirmIceCup(\n\t\t\t\t\ts_lIceScaleEmptyBaselineGram", workflow)
        cup_check = workflow.index("prvConfirmIceCup(\n\t\t\t\t\ts_lIceScaleEmptyBaselineGram")
        dispense = workflow.index("prvDispenseIce(usManualIceWeight,")
        self.assertLess(cup_check, dispense)
        self.assertIn("if (lResult == 0) {", workflow[cup_check:dispense])

    def test_manual_cup_detection_matches_coffee1_limits(self):
        config = (APP / "Config/coffee3_app_config.h").read_text(
            encoding="utf-8"
        )
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("COFFEE3_ICE_CUP_DETECT_GRAM           6L", config)
        self.assertIn("COFFEE3_ICE_CUP_MAX_GRAM              100L", config)
        self.assertIn("COFFEE3_ICE_CUP_DETECT_ATTEMPTS       3U", config)
        self.assertIn("COFFEE3_ICE_CUP_COMM_RETRIES          10U", config)
        self.assertIn('"Cup confirmed: cup=%ld g total=%ld g empty=%ld g"', workflow)
        self.assertIn('"Cup check failed: reason=no cup', workflow)
        self.assertIn("ucManualIceDispenseStarted == 0U", workflow)

    def test_scale_baseline_and_software_difference_flow(self):
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        clear_tare = workflow.index("COFFEE3_ACTION_SCALE_CLEAR_TARE")
        zero = workflow.index("COFFEE3_ACTION_SCALE_ZERO", clear_tare)
        stable = workflow.index("prvReadStableScale(0xFC12U", zero)
        self.assertLess(clear_tare, zero)
        self.assertLess(zero, stable)
        self.assertIn("s_ucIceScaleBaselineValid = 1U", workflow)
        self.assertIn(
            "lCupWeightGram = lTotalWeightGram - lEmptyBaselineGram", workflow
        )
        self.assertIn(
            "lWeightGram = lTotalWeightGram - lCupBaselineGram", workflow
        )
        dispense_body = workflow[
            workflow.index("static int32_t prvDispenseIce(uint16_t usTargetGram,", 1000):
            workflow.index("static uint32_t prvCalculateIcePulseMs", 1000)
        ]
        self.assertNotIn("COFFEE3_ACTION_SCALE_TARE", dispense_body)

    def test_order_samples_empty_then_confirms_cup(self):
        workflow = (APP / "WorkFlow/coffee3_workflow.c").read_text(
            encoding="utf-8"
        )
        run_order = workflow.index("static int32_t prvRunOrder(const Coffee3Order_t *pxOrder)\n{")
        empty_read = workflow.index("prvReadStableScale(56U", run_order)
        robot_to_ice = workflow.index("COFFEE3_ACTION_ROBOT_TO_ICE", empty_read)
        cup_check = workflow.index("prvConfirmIceCup(lIceEmptyBaselineGram", robot_to_ice)
        dispense = workflow.index("prvDispenseIce(usIceAmount,", cup_check)
        self.assertLess(empty_read, robot_to_ice)
        self.assertLess(robot_to_ice, cup_check)
        self.assertLess(cup_check, dispense)


if __name__ == "__main__":
    unittest.main()
