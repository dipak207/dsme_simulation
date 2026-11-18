# STREAM-E Objective 4: Adaptive DSME MO

This focused Contiki repository contains the STREAM-E Objective 4 implementation and reproducible Cooja experiment for adaptive DSME Multi-Superframe Order (MO) growth and shrinkage. The algorithm and protocol are evaluated in the final report; results do not claim QoS improvement without direct latency/deadline measurements.

## Source and experiments

- DSME and adaptive controller: `core/net/mac/dsme/` (including nested `openDSME/`).
- Three-mote application and Cooja scenarios: `examples/ipv6/rpl-udp-dsme/`.
- Deterministic tests: `tests/adaptive-mo/`.
- Cooja source/runtime: `platform/cooja/` and `tools/cooja/`.
- Consolidated research report: `reports/STREAM_E_OBJECTIVE_4_COMPLETE_FINAL_REPORT.md`.
- Final five-minute raw run: `logs/ADAPTIVE_MO_FINAL_5MIN.log`.

## Tests

```sh
make -C tests/adaptive-mo test
```

The command builds and runs the deterministic C++ policy/protocol helper suite.

## Final audit and validation report

Validated with OpenJDK 21.0.13 for Java, `javac`, and JNI headers; Ant 1.10.14; GCC/G++ 13.3.0; Python 3.14.2; JAXB compatibility jars; and Rhino. The headless runner uses `-Xint`, because JIT-enabled runs crash in `HeapByteBuffer.getInt` during active mote callbacks.

Run the standard regression from the repository root:

```sh
make -C tests/adaptive-mo test
make sim-minimal
make sim-adaptive
```

Fresh runs after final-state instrumentation and validator hardening:

| Run | Result and evidence |
| --- | --- |
| Host tests | 45/45 passed. See [the fresh host-test log](simulation-results/20261117T220300Z-host-final-audit.log). |
| Minimal | PASS; three motes, 120,000,000 µs, 2,601 log callbacks, 733 radio TX / 884 RX, 189 successful GTS payload confirmations, 189 CFP receive frames, two successful participant associations, and live terminal snapshots for all motes. [Cooja log](simulation-results/20261117T220449.687218778Z-19-minimal.log) · [Validation](simulation-results/20261117T220449.687218778Z-19-minimal-validation.txt) · [Native build](simulation-results/20261117T220449.687218778Z-19-minimal-native-build.log) · [External tools](simulation-results/20261117T220449.687218778Z-19-external-tools.config) |
| Adaptive | PASS; three motes, 300,000,000 µs, all eight transitions, and live terminal snapshots for all motes. [Cooja log](simulation-results/20261117T220657.110395064Z-4-adaptive.log) · [Validation](simulation-results/20261117T220657.110395064Z-4-adaptive-validation.txt) · [Native build](simulation-results/20261117T220657.110395064Z-4-adaptive-native-build.log) · [External tools](simulation-results/20261117T220657.110395064Z-4-external-tools.config) · [Independent validation](simulation-results/20261117T220934.240724517Z-5-adaptive-validation-only.txt) |

Each `make sim-*` run also executes 14 synthetic validator tests before building. The tests accept valid minimal/adaptive fixtures and reject missing participant activation, boundary mismatch, incorrect MO, missing coordinator completion, prepared/committed/pending terminal states, missing snapshots, Java exceptions, DSME assertions, and missing GTS or CFP traffic.

The adaptive run used seed 123456, initial MO 5, bounds 5..6, SO 4, BO 7, CAP reduction, and no fault-injection definitions. The observed workload was LOW at 0–30 s, HIGH 30–60 s, LOW 60–90 s, HIGH 90–120 s, LOW 120–150 s, HIGH 150–180 s, LOW 180–210 s, HIGH 210–240 s, then LOW 240–300 s. The controller completed `5→6, 6→5, 5→6, 6→5, 5→6, 6→5, 5→6, 6→5`; all 24 actual activation boundaries matched their planned boundaries, and all motes applied and verified each target. At terminal snapshots, mote 1 was coordinator `IDLE` at MO 5; motes 2 and 3 were released `CLIENT_ACTIVE` at MO 5. All had SO 4, BO 7, zero pending session/sequence, no pending activation or release, zero unreleased peers, resources released, and no degraded/quarantined state.

Adaptive traffic: 499 successful GTS payload confirmations, 4 failed payload confirmations (status 6), 497 CFP receive frames, 1,265 data indications, 60 successful and 540 non-success GTS allocation confirmations, and 4,658 radio TX / 7,866 radio RX callbacks. Allocation failures were `TRANSACTION_OVERFLOW` (368), `NO_DATA` (120), and `DENIED` (52). Other observed warnings were two participant association successes after one transient association failure, 368 rejected GTS demand calls, 685 transaction-overflow status records, four prepare failures, four deferred proposals, one MO send failure, and 632 coordinator messages ignored for `peer_or_epoch_mismatch` (plus eight ignored GTS coordinator-control messages). These warnings remain visible in the report; both participants were ultimately released and no terminal degraded, quarantined, or stuck state was found. Counts are run-specific, not fixed acceptance thresholds.

### Protocol and controller behavior

`AdaptiveMOController` in `core/net/mac/dsme/AdaptiveMOController.h` weights high-priority demand twice as strongly as low-priority demand. It proposes an increase after three evaluation windows at or above four weighted units, or after a rejected request; it proposes a decrease after five windows at or below one unit with no rejections. It respects configured MO bounds, a two-window cooldown, ten startup grace windows for reductions, and resets persistence when a transaction is pending. The demo's 30-second LOW/HIGH labels describe the workload phases; the controller evaluates its own configured windows and is not directly commanded by those labels.

`BeaconManager` owns network coordination. The coordinator validates its schedule and participant set, assigns session/sequence IDs and a future activation boundary, sends PREPARE, and waits for peer READY responses before making COMMIT irrevocable. Participants validate transaction identity and schedule state, then acknowledge commit. At the agreed beacon boundary, coordinator and participants quiesce GTS activity, apply and verify the new MO, and report APPLIED. The coordinator releases peers only after all apply reports; participants enter `CLIENT_ACTIVE` and acknowledge release; the coordinator records `MO_NETWORK_ACTIVE` only after both release acknowledgements. Before commit, a matching transaction may be aborted; after commit, failure handling enters degraded recovery, queries peer state with nonces and can quarantine a participant rather than silently rolling back an irrevocable decision. The normal run exercised the successful path, not injected recovery failures.

### Automated validation

Each simulation writes four independent artifacts in `simulation-results/`: a timestamped Cooja log, native firmware build log, validation report, and external-tools configuration. The log records toolchain versions, scenario/seed, and native build definitions. The validator checks mote initialization and symbols, timeout and actual final time, Java/Cooja runtime errors and stack traces, assertions, callbacks, and scenario-specific behavior before the launcher prints `SIMULATION_PASS`.

Two non-passing hardening attempts were retained as evidence: the first correctly failed validation when Cooja coalesced mote 3's snapshot into another serial callback; the parser was updated to locate and parse the snapshot within that callback. A later attempt exited during native-build failure reporting while the launcher itself was being edited; a standalone native rebuild returned zero, and subsequent fresh minimal/adaptive runs completed and validated. Neither attempt is counted as a passing run.

The firmware emits one `MO_FINAL_STATE_SNAPSHOT` per mote near termination (118 seconds for minimal; 298 seconds for adaptive) by reading the live `BeaconManager` state. Fields include role/state, active/target MO, SO/BO, transaction/session/sequence, prepared/committed/applied status, pending activation/release, peer completion/resources released, recovery/degraded/quarantine, and a firmware-computed terminal flag. The validator checks all three snapshots and rejects protocol state-changing events after the snapshot window. Cooja may coalesce multiple serial writes into one callback; snapshot extraction handles the actual event token inside those combined records. Minimal validation requires association success from both participant IDs (2 and 3), successful GTS payload, and CFP reception.

Adaptive checks additionally require the exact eight MO transitions, 24 per-mote activation/apply/active-state records, matching actual and planned boundaries, eight coordinator network-active records with two released peers, and GTS/CFP activity. `MO_GTS_QUIESCENT_FENCE` is normal fencing activity and is not treated as a stuck transaction. A historical lifecycle event such as `MO_CLIENT_COMMITTED` is not by itself considered a pending transaction; the near-termination snapshot is the source of final-state truth.

The simulator runner executes the synthetic tests before each run. To run them directly while developing the validator: `python3 tools/cooja/codespaces/test_validate_simulation.py`.

Recheck an existing adaptive log without running a new simulation:

```sh
make validate-adaptive LOG=simulation-results/path-to-adaptive.log
```

This creates a new validation-only report. The independent validation passed for [the final fresh adaptive run](simulation-results/20261117T220657.110395064Z-4-adaptive.log); see [its report](simulation-results/20261117T220934.240724517Z-5-adaptive-validation-only.txt). Validation-only runs never replace simulation logs. VarMemory reports all 30 symbol sizes as unknown (`-1`); each symbol exists and its address is verified readable. No size or native address is fabricated.

### Environment setup and GUI

Install the required tools and compatibility libraries when absent:

```sh
sudo apt-get update
sudo apt-get install -y ant build-essential make python3 perl openjdk-21-jdk-headless libjaxb-api-java librhino-java
```

The runner verifies Java/Javac version agreement, Java 21, JNI headers, Cooja runtime jars, Rhino, JAXB, GCC/G++, Make, Python, and native inspection tools. It builds the repository's legacy Cooja core with `ant jar_cooja` and builds firmware using the same JDK's JNI headers. The aggregate `ant jar` target is unnecessary for these Contiki mote scenarios.

With a display, open either `.csc` scenario in the Cooja GUI after building the core, using the same Rhino/JAXB classpath and `-Xint` shown in the GUI instructions below. In headless Codespaces, use the Make targets. The CSC's Rhino timeout is primary; the headless runner has a later simulated-time safety stop and a wall-clock watchdog.

Known limits: JIT-enabled Java is unsupported for this legacy Cooja runtime; fault-injected recovery paths are not exercised by these normal scenarios; native symbol sizes are unavailable from VarMemory; snapshots are emitted shortly before scenario stop (and checked for later MO state changes) rather than read after the Cooja thread has stopped; and these runs establish no QoS, latency, deadline, or energy improvement. Earlier successful reference logs remain preserved at [minimal](simulation-results/20261117T182617Z-minimal.log) and [adaptive](simulation-results/20261117T183755Z-adaptive.log). The historical `logs/ADAPTIVE_MO_FINAL_5MIN.log` is also preserved.

### Open Cooja with a display

From the repository root, build the Cooja core and then launch with the Rhino and JAXB compatibility jars on the classpath:

```sh
cd tools/cooja
CLASSPATH=/usr/share/java/jaxb-api.jar ant -Dbuild.sysclasspath=last jar_cooja
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
java -Xint -cp 'build-codespaces:dist/lib/*:/usr/share/java/rhino.jar:/usr/share/java/jaxb-api.jar:/usr/share/java/jaxb-runtime.jar:/usr/share/java/jaxb-core.jar:/usr/share/java/javax.activation.jar' \
  org.contikios.cooja.Cooja -contiki=../.. -external_tools_config=../../simulation-results/TIMESTAMP-external-tools.config
```

For GUI use, run from `tools/cooja` with a display, replace `TIMESTAMP` with a generated `simulation-results/*-external-tools.config` basename, then open either scenario. `-Xint` is required for this legacy Java/NIO memory path; it avoids the active-tick exception seen with JIT enabled. The scenarios' own ScriptRunner timeout is enabled by Rhino: 120 seconds for minimal and 300 seconds for adaptive. A GUI display is required; headless Codespaces should use the Make targets.

### Logs and troubleshooting

Read the newest `simulation-results/*-minimal.log` or `simulation-results/*-adaptive.log`. The log records tool versions, the Ant build, Make commands, symbol addresses from the ELF/map/table, mote initialization, mote log-observer and radio callback counts, and the final simulated time. `SYMBOL_OK` failures indicate a native link or symbol-generation problem; dependency failures report the exact missing command or JAR. Keep each run log when comparing results. Do not use `ant clean` while a simulation is running.
