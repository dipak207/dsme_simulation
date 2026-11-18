#!/usr/bin/env python3
"""Validate the observable evidence in a headless Cooja run log."""

import argparse
import collections
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REQUIRED_SYMBOLS = {
    "simLoggedLength", "simLoggedFlag", "simLoggedData", "simOutSize",
    "simOutDataBuffer", "simInSize", "simInDataBuffer", "simRadioHWOn",
    "simSerialReceivingLength", "simSerialReceivingData",
}
EXPECTED_TRANSITIONS = [(5, 6), (6, 5), (5, 6), (6, 5), (5, 6), (6, 5), (5, 6), (6, 5)]
EXPECTED_DURATION = {"minimal": 120_000_000, "adaptive": 300_000_000, "fixed-mo5": 300_000_000, "fixed-mo6": 300_000_000}


def fields(line):
    return dict(re.findall(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", line))


def event_lines(lines, token):
    return [line for line in lines if line.startswith("MOTE_LOG ") and f" {token} " in line]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("minimal", "adaptive", "fixed-mo5", "fixed-mo6"), required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    errors, notes = [], []
    text = args.log.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    duration = 120_000_000 if args.mode == "minimal" else 300_000_000

    def require(condition, message):
        if not condition:
            errors.append(message)

    require(f"RUN_CONFIG mode={args.mode} seed=123456 duration_us={duration}" in text,
            "launcher configuration record missing or incorrect")

    try:
        root = ET.parse(args.scenario).getroot()
        sim = root.find("simulation")
        seed = int(sim.findtext("randomseed", "-1")) if sim is not None else -1
        scripts = [node.text or "" for node in root.iter("script")]
        commands = "\n".join((node.text or "") for node in root.iter("commands"))
        timeout_match = re.search(r"TIMEOUT\s*\(\s*(\d+)\s*,", "\n".join(scripts))
        scenario_timeout = int(timeout_match.group(1)) * 1000 if timeout_match else -1
        require(seed == 123456, f"scenario seed is {seed}, expected 123456")
        require(scenario_timeout == duration, f"scenario timeout is {scenario_timeout} us, expected {duration}")
        for definition in ("DSME=1", "PAN_COORDINATOR=1", "DSME_CAP_REDUCTION=1", "DSME_MO=" + ("6" if args.mode == "fixed-mo6" else "5")):
            require(definition in commands, f"scenario build definition missing: {definition}")
        if args.mode in ("adaptive", "fixed-mo5", "fixed-mo6"):
            workload_definitions = ("ADAPTIVE_MO_DEMO=1", "ADAPTIVE_MO_MIN=5", "ADAPTIVE_MO_MAX=6",
                                   "ADAPTIVE_MO_PHASE_SECONDS=30", "ADAPTIVE_MO_DEMAND_MS=400",
                                   "ADAPTIVE_MO_DEMAND_SLOTS=1")
            for definition in workload_definitions:
                require(definition in commands, f"workload build definition missing: {definition}")
        if args.mode.startswith("fixed-"):
            require("DSME_ADAPTIVE_MO_ENABLE=0" in commands, "fixed-MO controller is not disabled")
        require(not re.search(r"(?i)(ADAPTIVE_MO_FAULT|FAIL_APPLY|FAIL_ACT_ROLLBACK|FAULT_INJECT)", commands),
                "fault-injection definition found in scenario")
    except (ET.ParseError, OSError, AttributeError, ValueError) as error:
        seed, scenario_timeout, commands = -1, -1, ""
        require(False, f"cannot inspect scenario configuration: {error}")

    initialized = set()
    for line in lines:
        if "MOTE_INITIALIZED " in line:
            item = fields(line)
            if item.get("type") == "ContikiMote" and item.get("requiredSymbols") == "10":
                initialized.add(int(item["id"]))
    require(initialized == {1, 2, 3}, f"initialized mote IDs are {sorted(initialized)}, expected [1, 2, 3]")
    symbol_records = collections.defaultdict(set)
    symbol_sizes = collections.Counter()
    for line in lines:
        if line.startswith("MEMORY_SYMBOL "):
            item = fields(line)
            if item.get("symbolExists") == "true" and item.get("addressValid") == "true":
                symbol_records[int(item["mote"])].add(item["name"])
            symbol_sizes[item.get("sizeStatus", "missing_metadata")] += 1
            require(item.get("sizeKnown") in ("true", "false"), "symbol size status was not explicit")
            require(not (item.get("sizeKnown") == "true" and int(item.get("size", "0")) <= 0),
                    f"invalid known size for {item.get('name')}")
            require(not (item.get("sizeKnown") == "false" and item.get("sizeStatus") != "unknown"),
                    f"unknown size not identified for {item.get('name')}")
    require(all(symbol_records.get(mote, set()) == REQUIRED_SYMBOLS for mote in (1, 2, 3)),
            "required symbol/address validation did not pass for every mote")

    config = next((fields(line) for line in lines if line.startswith("CONFIG_LOADED ")), {})
    require(config.get("motes") == "3" and config.get("seed") == "123456", "Cooja loaded mote count or seed is wrong")
    result = next((fields(line) for line in lines if line.startswith("RUN_RESULT ")), {})
    require(result.get("motes") == "3", "runner final mote count is not three")
    try:
        final_sim_time = int(result.get("simTime", "-1"))
    except ValueError:
        final_sim_time = -1
    require(result.get("durationReached") == "true" and final_sim_time >= duration,
            f"actual final simulation time {final_sim_time} us did not reach {duration} us")
    require(result.get("pass") == "true", "Java runner did not report pass=true")
    termination = next((fields(line) for line in lines if line.startswith("TERMINATION ")), {})
    require(termination.get("source") == "scenario_timeout", "scenario timeout was not the terminating event")
    require(termination.get("scenario_timeout_at_us") == str(duration), "scenario timeout time does not match target")
    require(termination.get("runner_safety_stop") == "false" and termination.get("wall_watchdog") == "false",
            "a safety watchdog ended the run")
    callback_line = next((line for line in lines if line.startswith("CALLBACK_COUNTS ")), "")
    callbacks = fields(callback_line)
    require(bool(callback_line), "callback counts missing")
    for key in ("logCallbacks", "radio_tx", "radio_rx"):
        require(int(callbacks.get(key, "0")) > 0, f"{key} callbacks missing")

    runtime_patterns = (
        r"^FATAL\b",
        r"^SEVERE\b",
        r"^ERROR\s+-",
        r"^Exception in thread ",
        r"^Caused by:\s+[^ ]*(?:Exception|Error)\b",
        r"\b(?:java|javax|org\.contikios\.cooja)\.[A-Za-z0-9_.$]*(?:Exception|Error)\b",
        r"^\s*at\s+[A-Za-z0-9_.$]+\([^)]*\.java(?::\d+)?\)\s*$",
        r"^No JavaScript engine\b",
        r"^MOTE_LOG .*\b(?:DSME_ASSERT|SIM ASSERT|ASSERT)\b",
    )
    runtime_errors = [
        f"line {index}: {line}"
        for index, line in enumerate(lines, 1)
        if any(re.search(pattern, line) for pattern in runtime_patterns)
    ]
    require(not runtime_errors,
            "Java exception/stack trace, Cooja fatal error, or runtime assertion present: "
            + " | ".join(runtime_errors[:5]))

    gts_payload = [fields(line) for line in event_lines(lines, "DSME_DATA_CONFIRM") if re.search(r"\bgts_tx=1\b", line)]
    gts_data_ok = sum(item.get("status") == "0" for item in gts_payload)
    gts_data_failed = sum(item.get("status") != "0" for item in gts_payload)
    gts_payload_statuses = collections.Counter(item.get("status", "unknown") for item in gts_payload)
    cfp_rx = len(event_lines(lines, "DSME_CFP_RX_FRAME"))
    data_indications = len(event_lines(lines, "DSME_DATA_INDICATION"))
    allocation_lines = [line for line in event_lines(lines, "GTS_FINAL_CONFIRM") if re.search(r"\bmanagement=1\b", line)]
    allocation_status = collections.Counter(fields(line).get("status", "unknown") for line in allocation_lines)
    radio_tx, radio_rx = int(callbacks.get("radio_tx", "0")), int(callbacks.get("radio_rx", "0"))

    mote_lines = [line for line in lines if line.startswith("MOTE_LOG ")]
    association_success_by_mote = collections.Counter()
    for line in mote_lines:
        if "Association completed successfully" in line:
            item = fields(line)
            try:
                association_success_by_mote[int(item["id"])] += 1
            except (KeyError, ValueError):
                errors.append(f"association-success record has no valid mote ID: {line}")
    association_success = sum(association_success_by_mote.values())
    association_failures = sum(bool(re.search(r"(?i)association.{0,30}(failed|failure|reject)", line)) for line in mote_lines)
    gts_rejections = sum("APP_GTS_DEMAND_INVOKE" in line and "accepted=0" in line for line in mote_lines)
    overflow_events = sum("status=TRANSACTION_OVERFLOW" in line for line in mote_lines)
    ignored = [line for line in event_lines(lines, "MO_COORD_COMMAND_IGNORED")]
    ignored_reasons = collections.Counter(fields(line).get("reason", "unspecified") for line in ignored)
    ignored_control_types = collections.Counter()
    for line in mote_lines:
        payload_match = re.match(r"MOTE_LOG time_us=\d+ id=\d+ (.*)", line)
        if payload_match:
            payload = payload_match.group(1).split(maxsplit=1)
            if payload and payload[0].endswith("COORD_COMMAND_IGNORED"):
                ignored_control_types[payload[0]] += 1
    ignored_control_count = sum(ignored_control_types.values())
    prepare_failed = sum("MO_PREPARE_FAILED " in line for line in mote_lines)
    proposals_deferred = sum("MO_PROPOSAL_DEFERRED " in line for line in mote_lines)
    send_failed = sum("MO_SEND_FAILED " in line for line in mote_lines)
    notes.extend([
        f"TRAFFIC gts_payload_success={gts_data_ok} gts_payload_failed={gts_data_failed} gts_payload_statuses={dict(gts_payload_statuses)} cfp_rx_frames={cfp_rx}",
        f"TRAFFIC application_data_indications={data_indications} gts_allocation_success={allocation_status.get('SUCCESS', 0)} gts_allocation_failed={sum(v for k, v in allocation_status.items() if k != 'SUCCESS')}",
        f"RADIO_CALLBACKS tx={radio_tx} rx={radio_rx}; log_callbacks={callbacks.get('logCallbacks', '0')}",
        f"RECOVERABLE_EVENTS association_success={association_success} association_failures={association_failures} gts_demand_rejections={gts_rejections} gts_transaction_overflow={overflow_events} mo_prepare_failed={prepare_failed} mo_proposals_deferred={proposals_deferred} mo_send_failed={send_failed}",
        f"ASSOCIATION_SUCCESS_BY_MOTE {dict(sorted(association_success_by_mote.items()))}",
        f"STALE_OR_MISMATCHED_MO_MESSAGES count={len(ignored)} reasons={dict(ignored_reasons)}",
        f"IGNORED_COORDINATOR_CONTROL_MESSAGES count={ignored_control_count} types={dict(ignored_control_types)}",
        f"SYMBOL_METADATA known={symbol_sizes.get('known', 0)} unknown={symbol_sizes.get('unknown', 0)}; unknown sizes are reported, never accepted as a size",
    ])

    # The Cooja log observer prefixes every firmware line with the Cooja mote
    # ID and simulation time. These snapshots are emitted by the firmware
    # shortly before the fixed ScriptRunner timeout, so they expose the actual
    # protocol state rather than a conclusion inferred from old lifecycle logs.
    final_states = collections.defaultdict(list)
    seen_snapshots = set()
    for line in lines:
        # Cooja's serial log interface can coalesce several firmware log
        # writes into one callback. A later event can consequently follow a
        # non-whitespace character (for example ``sf=MO_FINAL...``), so find
        # the exact record token inside each callback and parse only its tail.
        for match in re.finditer(r"\bMO_FINAL_STATE_SNAPSHOT\b", line):
            item = fields(line[match.end():])
            prefix = fields(line[:match.start()])
            item["id"] = prefix.get("id", "")
            try:
                mote_id = int(item["id"])
                snapshot_time = int(prefix["time_us"])
            except (KeyError, ValueError):
                errors.append(f"malformed final-state snapshot: {line}")
                continue
            item["_time_us"] = str(snapshot_time)
            snapshot_key = (mote_id, snapshot_time)
            if snapshot_key in seen_snapshots:
                continue
            seen_snapshots.add(snapshot_key)
            final_states[mote_id].append(item)

    require(set(final_states) == {1, 2, 3},
            f"final-state snapshots are present for motes {sorted(final_states)}, expected [1, 2, 3]")
    final_snapshot_times = []
    expected_final_state = (
        {
            1: {"role": "coordinator", "state": "IDLE", "local_applied": "0",
                "commit_decision": "0", "target_mo": "0", "epoch_sequence": "0",
                "prepared": "0", "committed": "0", "applied": "0"},
            2: {"role": "participant", "state": "IDLE", "local_applied": "0",
                "commit_decision": "0", "target_mo": "0", "epoch_sequence": "0",
                "prepared": "0", "committed": "0", "applied": "0"},
            3: {"role": "participant", "state": "IDLE", "local_applied": "0",
                "commit_decision": "0", "target_mo": "0", "epoch_sequence": "0",
                "prepared": "0", "committed": "0", "applied": "0"},
        }
        if args.mode == "minimal" else
        {
            1: {"role": "coordinator", "state": "IDLE", "local_applied": "0",
                "commit_decision": "0", "target_mo": "0", "epoch_sequence": "0",
                "prepared": "0", "committed": "0", "applied": "0"},
            2: {"role": "participant", "state": "CLIENT_ACTIVE", "local_applied": "1",
                "commit_decision": "1", "target_mo": "5", "epoch_sequence": "8",
                "prepared": "1", "committed": "1", "applied": "1"},
            3: {"role": "participant", "state": "CLIENT_ACTIVE", "local_applied": "1",
                "commit_decision": "1", "target_mo": "5", "epoch_sequence": "8",
                "prepared": "1", "committed": "1", "applied": "1"},
        }
    )
    for mote_id in (1, 2, 3):
        records = final_states.get(mote_id, [])
        require(len(records) == 1,
                f"mote {mote_id}: expected exactly one final-state snapshot, found {len(records)}")
        if len(records) != 1:
            continue
        item = records[0]
        final_snapshot_times.append(int(item["_time_us"]))
        require(item.get("node") == str(mote_id),
                f"mote {mote_id}: snapshot short address is {item.get('node', 'missing')}, expected {mote_id}")
        expected_fields = () if args.mode in ("fixed-mo5", "fixed-mo6") else expected_final_state[mote_id].items()
        expected_mo = "6" if args.mode == "fixed-mo6" else "5"
        for key, expected in (
            *expected_fields,
            ("active_mo", expected_mo), ("so", "4"), ("bo", "7"), ("transaction_pending", "0"),
            ("expected_peers", "0"), ("unreleased_peers", "0"),
            ("recovery", "0"), ("terminal_ok", "1"),
            ("pending_session", "0"), ("pending_sequence", "0"),
            ("activation_pending", "0"), ("release_pending", "0"),
            ("resources_released", "1"), ("quarantined", "0"), ("degraded", "0"),
        ):
            require(item.get(key) == expected,
                    f"mote {mote_id}: final-state {key}={item.get(key, 'missing')}, expected {expected}")

    if final_snapshot_times:
        earliest_allowed = duration - 5_000_000
        latest_allowed = duration
        require(all(earliest_allowed <= timestamp < latest_allowed for timestamp in final_snapshot_times),
                f"final-state snapshot times {final_snapshot_times} must be in [{earliest_allowed}, {latest_allowed}) us")
        last_snapshot_time = max(final_snapshot_times)
        state_changing_tokens = (
            "MO_PREPARE_STARTED", "MO_CHANGE_PROPOSED", "MO_ACTIVATION_BOUNDARY",
            "MO_APPLY_SUCCEEDED", "MO_NETWORK_ACTIVE", "MO_NETWORK_DEGRADED",
            "MO_PARTICIPANT_QUARANTINED", "MO_RECOVERY_QUARANTINED",
            "MO_RECONCILIATION_STARTED", "MO_RECONCILIATION_COMPLETED",
        )
        later_state_changes = []
        for line in mote_lines:
            if not any(f" {token} " in line for token in state_changing_tokens):
                continue
            try:
                event_time = int(fields(line).get("time_us", "-1"))
            except ValueError:
                event_time = -1
            if event_time > last_snapshot_time:
                later_state_changes.append(line)
        require(not later_state_changes,
                "MO protocol state-changing events occurred after all final-state snapshots")
        notes.append("FINAL_PROTOCOL_STATE " + ", ".join(
            f"mote{mote_id}={expected_final_state[mote_id]['state']}"
            for mote_id in (1, 2, 3) if len(final_states.get(mote_id, [])) == 1
        ) + f" snapshot_window_us={min(final_snapshot_times)}..{max(final_snapshot_times)}")
    if association_failures or gts_rejections or overflow_events or gts_data_failed or ignored_control_count or prepare_failed or proposals_deferred or send_failed:
        notes.append("ANOMALIES are reported above; protocol completion checks below determine whether they left an unresolved transition.")
    if gts_data_ok != 499 or gts_data_failed != 4 or cfp_rx != 497:
        notes.append(f"REFERENCE_COMPARISON uploaded_run=(gts_success=499,gts_failed=4,cfp_rx=497) current=({gts_data_ok},{gts_data_failed},{cfp_rx}); differences are informational")

    if args.mode in ("minimal", "fixed-mo5", "fixed-mo6"):
        for participant_id in (2, 3):
            require(association_success_by_mote.get(participant_id, 0) > 0,
                    f"participant mote {participant_id} has no successful association record")
        require(allocation_status.get("SUCCESS", 0) > 0, "successful GTS allocation is absent")
        require(gts_data_ok > 0, "successful GTS payload confirmation is absent")
        require(cfp_rx > 0, "no CFP receive frames observed")
        if args.mode.startswith("fixed-"):
            require(not event_lines(lines, "MO_CHANGE_PROPOSED"), "fixed-MO run proposed a reconfiguration")
    else:
        require("APP_LOAD_PHASE" in text and "demand=LOW" in text and "demand=HIGH" in text,
                "normal LOW/HIGH workload phases are not both present")
        boundaries = collections.defaultdict(list)
        applies = collections.defaultdict(list)
        verified = collections.defaultdict(list)
        networks = collections.defaultdict(list)
        preparations = collections.defaultdict(list)
        proposals = collections.defaultdict(list)
        for line in lines:
            if "MO_PREPARE_STARTED " in line:
                item = fields(line)
                preparations[int(item["epoch_sequence"])].append(int(item["id"]))
            elif "MO_CHANGE_PROPOSED " in line:
                item = fields(line)
                proposals[int(item["epoch_sequence"])].append(int(item["id"]))
            elif "MO_ACTIVATION_BOUNDARY " in line:
                item = fields(line)
                seq = int(item["epoch_sequence"])
                boundaries[seq].append((int(line.split("time_us=", 1)[1].split()[0]), int(item["id"]),
                                        int(item["old_mo"]), int(item["target_mo"]),
                                        int(item["activation_symbols"]), int(item["actual_boundary"])))
            elif "MO_APPLY_SUCCEEDED " in line:
                item = fields(line)
                applies[int(item["epoch_sequence"])].append((int(item["id"]), int(item["actual_mo"])))
            elif "MO_ACTIVE_STATE_VERIFIED " in line:
                item = fields(line)
                time_us = int(line.split("time_us=", 1)[1].split()[0])
                verified[time_us].append((int(item["id"]), int(item["active_mo"]), int(item["so"]), int(item["bo"])))
            elif "MO_NETWORK_ACTIVE " in line:
                item = fields(line)
                networks[int(item["epoch_sequence"])].append((int(item["id"]), int(item["active_mo"]), int(item.get("released_peers", "-1"))))
        expected_sequences = set(range(1, 9))
        require(set(boundaries) == expected_sequences, f"completed transition sequence IDs are {sorted(boundaries)}, expected 1..8")
        require(set(networks) == expected_sequences, f"network-active sequence IDs are {sorted(networks)}, expected 1..8")
        require(set(preparations) == expected_sequences and all(len(nodes) == 1 and nodes[0] == 1 for nodes in preparations.values()),
                "coordinator prepare-start records do not match the eight expected sequences")
        require(set(proposals) == expected_sequences and all(len(nodes) == 1 and nodes[0] == 1 for nodes in proposals.values()),
                "coordinator change-proposal records do not match the eight expected sequences")
        require(sum(map(len, boundaries.values())) == 24, "expected exactly 24 activation-boundary records")
        require(sum(map(len, applies.values())) == 24, "expected exactly 24 successful apply records")
        require(sum(map(len, verified.values())) == 24, "expected exactly 24 active-state-verification records")
        require(sum(map(len, networks.values())) == 8, "expected exactly eight network-active records")
        transition_summary = []
        for seq, (old, target) in enumerate(EXPECTED_TRANSITIONS, 1):
            records = boundaries.get(seq, [])
            require(len(records) == 3 and {row[1] for row in records} == {1, 2, 3},
                    f"sequence {seq}: each mote must emit exactly one boundary record")
            if len(records) == 3:
                require(all(row[2] == old and row[3] == target for row in records),
                        f"sequence {seq}: old/target MO does not match {old}->{target}")
                require(all(row[4] == row[5] for row in records), f"sequence {seq}: planned and actual boundaries differ")
                require(len({(row[0], row[2], row[3], row[4], row[5]) for row in records}) == 1,
                        f"sequence {seq}: motes disagree on activation boundary or MO")
                time_us = records[0][0]
                require(sorted(applies.get(seq, [])) == [(1, target), (2, target), (3, target)],
                        f"sequence {seq}: every mote must successfully apply target MO {target}")
                require(sorted(verified.get(time_us, [])) == [(1, target, 4, 7), (2, target, 4, 7), (3, target, 4, 7)],
                        f"sequence {seq}: active-state verification must agree on MO/SO/BO")
                transition_summary.append(f"{seq}:{old}->{target}@{records[0][4]}")
            net = networks.get(seq, [])
            require(len(net) == 1 and net[0][0] == 1 and net[0][1] == target and net[0][2] == 2,
                    f"sequence {seq}: network-active coordinator completion must release two peers")
        notes.append("ADAPTIVE_TRANSITIONS " + ", ".join(transition_summary))
        stuck_patterns = (
            r"\bMO_QUIESCENT\b", r"\bMO_NETWORK_DEGRADED\b", r"\bMO_PARTICIPANT_QUARANTINED\b",
            r"\bMO_RECOVERY_QUARANTINED\b",
        )
        stuck = [line for line in lines if line.startswith("MOTE_LOG ") and any(re.search(pattern, line) for pattern in stuck_patterns)]
        require(not stuck, f"stuck/degraded/quarantined/aborted protocol state recorded ({len(stuck)} events)")
        require(cfp_rx > 0, "no CFP receive frames observed")
        require(gts_data_ok > 0, "no successful GTS payload transmissions observed")

    status = "PASS" if not errors else "FAIL"
    report = [f"VALIDATION_{status} mode={args.mode} log={args.log}",
              f"CONFIG seed={seed} duration_us={duration} scenario_timeout_us={scenario_timeout}",
              f"MOTES initialized={sorted(initialized)} symbols_per_mote=" + ",".join(str(len(symbol_records.get(i, set()))) for i in (1, 2, 3)),
              f"TERMINATION source={termination.get('source', 'missing')} final_us={result.get('simTime', 'missing')}",
              *notes]
    if allocation_status:
        report.append("GTS_ALLOCATION_STATUSES " + str(dict(allocation_status)))
    if errors:
        report.extend("FAILURE " + error for error in errors)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
