#!/usr/bin/env python3
"""Synthetic acceptance and rejection tests for validate_simulation.py."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
VALIDATOR = HERE / "validate_simulation.py"
ROOT = HERE.parents[2]
SCENARIOS = {
    "minimal": ROOT / "examples/ipv6/rpl-udp-dsme/dsme-minimal.csc",
    "adaptive": ROOT / "examples/ipv6/rpl-udp-dsme/dsme-adaptive-mo.csc",
}
DURATIONS = {"minimal": 120_000_000, "adaptive": 300_000_000}
SYMBOLS = (
    "simLoggedLength simLoggedFlag simLoggedData simOutSize simOutDataBuffer "
    "simInSize simInDataBuffer simRadioHWOn simSerialReceivingLength simSerialReceivingData"
).split()


def snapshot(mote, mode, edits=None):
    if mode == "minimal" or mote == 1:
        values = dict(role="coordinator" if mote == 1 else "participant", state="IDLE",
                      active_mo=5, target_mo=0, so=4, bo=7, transaction_pending=0,
                      pending_session=0, pending_sequence=0, expected_peers=0,
                      unreleased_peers=0, prepared=0, committed=0, applied=0,
                      activation_pending=0, activation_symbols=0, release_pending=0,
                      resources_released=1, quarantined=0, degraded=0,
                      local_applied=0, commit_decision=0, recovery=0,
                      epoch_sequence=0, terminal_ok=1)
    else:
        values = dict(role="participant", state="CLIENT_ACTIVE", active_mo=5,
                      target_mo=5, so=4, bo=7, transaction_pending=0,
                      pending_session=0, pending_sequence=0, expected_peers=0,
                      unreleased_peers=0, prepared=1, committed=1, applied=1,
                      activation_pending=0, activation_symbols=123456,
                      release_pending=0, resources_released=1, quarantined=0,
                      degraded=0, local_applied=1, commit_decision=1, recovery=0,
                      epoch_sequence=8, terminal_ok=1)
    values.update(edits or {})
    rendered = " ".join(f"{key}={value}" for key, value in values.items())
    return f"MOTE_LOG time_us={DURATIONS[mode] - 2_000_000} id={mote} MO_FINAL_STATE_SNAPSHOT node={mote} {rendered}"


def fixture(mode):
    duration = DURATIONS[mode]
    lines = [f"RUN_CONFIG mode={mode} seed=123456 duration_us={duration} motes=3"]
    lines += [f"MOTE_INITIALIZED id={mote} type=ContikiMote requiredSymbols=10" for mote in (1, 2, 3)]
    for mote in (1, 2, 3):
        lines.extend(
            f"MEMORY_SYMBOL mote={mote} name={name} symbolExists=true addressValid=true "
            "sizeKnown=false sizeStatus=unknown size=-1"
            for name in SYMBOLS
        )
    lines.extend((
        "CONFIG_LOADED motes=3 seed=123456",
        f"TERMINATION source=scenario_timeout scenario_timeout_at_us={duration} runner_safety_stop=false wall_watchdog=false",
        f"RUN_RESULT motes=3 simTime={duration} targetTime={duration} durationReached=true pass=true failure=none",
        "CALLBACK_COUNTS logCallbacks=30 radio_tx=20 radio_rx=20",
        "MOTE_LOG time_us=100 id=2 Association completed successfully",
        "MOTE_LOG time_us=100 id=3 Association completed successfully",
        "MOTE_LOG time_us=200 id=2 GTS_FINAL_CONFIRM management=1 status=SUCCESS",
        "MOTE_LOG time_us=300 id=2 DSME_DATA_CONFIRM source=2 destination=1 status=0 gts_tx=1",
        "MOTE_LOG time_us=400 id=1 DSME_CFP_RX_FRAME source=2 destination=1",
        snapshot(1, mode), snapshot(2, mode), snapshot(3, mode),
    ))
    if mode == "adaptive":
        lines += ["APP_LOAD_PHASE demand=LOW", "APP_LOAD_PHASE demand=HIGH"]
        for sequence, (old_mo, target_mo) in enumerate(
                ((5, 6), (6, 5), (5, 6), (6, 5), (5, 6), (6, 5), (5, 6), (6, 5)), 1):
            transition_time = sequence * 1_000_000
            lines.append(f"MOTE_LOG time_us={transition_time} id=1 MO_PREPARE_STARTED node=1 epoch_sequence={sequence}")
            lines.append(f"MOTE_LOG time_us={transition_time} id=1 MO_CHANGE_PROPOSED node=1 epoch_sequence={sequence}")
            for mote in (1, 2, 3):
                lines.append(
                    f"MOTE_LOG time_us={transition_time} id={mote} MO_ACTIVATION_BOUNDARY node={mote} "
                    f"epoch_sequence={sequence} old_mo={old_mo} target_mo={target_mo} "
                    "activation_symbols=123456 actual_boundary=123456"
                )
                lines.append(
                    f"MOTE_LOG time_us={transition_time} id={mote} MO_APPLY_SUCCEEDED node={mote} "
                    f"epoch_sequence={sequence} actual_mo={target_mo}"
                )
                lines.append(
                    f"MOTE_LOG time_us={transition_time} id={mote} MO_ACTIVE_STATE_VERIFIED node={mote} "
                    f"epoch_sequence={sequence} active_mo={target_mo} so=4 bo=7"
                )
            lines.append(
                f"MOTE_LOG time_us={transition_time} id=1 MO_NETWORK_ACTIVE node=1 "
                f"epoch_sequence={sequence} active_mo={target_mo} released_peers=2"
            )
    return "\n".join(lines) + "\n"


class ValidatorTests(unittest.TestCase):
    def validate(self, mode, text):
        with tempfile.TemporaryDirectory(prefix="dsme-validator-") as directory:
            root = Path(directory)
            log, report = root / "synthetic.log", root / "report.txt"
            log.write_text(text, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(VALIDATOR), "--mode", mode, "--log", str(log),
                 "--scenario", str(SCENARIOS[mode]), "--report", str(report)],
                text=True, capture_output=True, check=False,
            )
            return result.returncode, report.read_text(encoding="utf-8")

    def test_accepts_valid_minimal_fixture(self):
        code, report = self.validate("minimal", fixture("minimal"))
        self.assertEqual(code, 0, report)

    def test_accepts_valid_adaptive_fixture(self):
        code, report = self.validate("adaptive", fixture("adaptive"))
        self.assertEqual(code, 0, report)

    def test_rejects_missing_participant_activation(self):
        text = fixture("adaptive").replace(
            "MOTE_LOG time_us=1000000 id=3 MO_ACTIVATION_BOUNDARY", "MOTE_LOG time_us=1000000 id=3 OMITTED_ACTIVATION_BOUNDARY", 1)
        self.assertRejected(text)

    def test_rejects_planned_boundary_mismatch(self):
        self.assertRejected(fixture("adaptive").replace("actual_boundary=123456", "actual_boundary=123457", 1))

    def test_rejects_incorrect_actual_mo(self):
        self.assertRejected(fixture("adaptive").replace("epoch_sequence=1 actual_mo=6", "epoch_sequence=1 actual_mo=5", 1))

    def test_rejects_missing_coordinator_completion(self):
        text = "\n".join(line for line in fixture("adaptive").splitlines()
                          if "MO_NETWORK_ACTIVE" not in line or "epoch_sequence=8" not in line) + "\n"
        self.assertRejected(text)

    def test_rejects_participant_left_prepared(self):
        text = fixture("adaptive").replace("state=CLIENT_ACTIVE", "state=CLIENT_PREPARED", 1)
        self.assertRejected(text)

    def test_rejects_participant_left_committed(self):
        text = fixture("adaptive").replace("state=CLIENT_ACTIVE", "state=CLIENT_COMMITTED", 1)
        self.assertRejected(text)

    def test_rejects_pending_transaction_in_snapshot(self):
        text = fixture("adaptive").replace("transaction_pending=0", "transaction_pending=1", 1)
        self.assertRejected(text)

    def test_rejects_missing_mote_snapshot(self):
        text = "\n".join(line for line in fixture("adaptive").splitlines()
                          if "id=3 MO_FINAL_STATE_SNAPSHOT" not in line) + "\n"
        self.assertRejected(text)

    def test_rejects_java_exception(self):
        self.assertRejected(fixture("minimal") + "Exception in thread \\\"main\\\" java.lang.IllegalStateException\\n")

    def test_rejects_protocol_assertion(self):
        self.assertRejected(fixture("minimal") + "MOTE_LOG time_us=500 id=1 DSME_ASSERT\\n")

    def test_rejects_missing_gts_payload(self):
        text = "\n".join(line for line in fixture("minimal").splitlines() if "DSME_DATA_CONFIRM" not in line) + "\n"
        self.assertRejected(text)

    def test_rejects_missing_cfp_activity(self):
        text = "\n".join(line for line in fixture("minimal").splitlines() if "DSME_CFP_RX_FRAME" not in line) + "\n"
        self.assertRejected(text)

    def assertRejected(self, text):
        code, report = self.validate("adaptive" if "MO_ACTIVATION_BOUNDARY" in text else "minimal", text)
        self.assertNotEqual(code, 0, report)
        self.assertIn("VALIDATION_FAIL", report)


if __name__ == "__main__":
    unittest.main(verbosity=2)
