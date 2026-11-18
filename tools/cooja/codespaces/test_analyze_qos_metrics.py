import importlib.util,tempfile,unittest,json
from pathlib import Path
SCRIPT=Path(__file__).with_name('analyze_qos_metrics.py')
spec=importlib.util.spec_from_file_location('analyzer',SCRIPT); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
class AnalyzerTests(unittest.TestCase):
 def test_packet_matching_anomalies_deadlines_and_incomplete_window(self):
  lines=['SIM_DURATION_START time_us=0 duration_us=1000000','APP_PLACEHOLDER','MOTE_LOG time_us=0 id=1 APP_MEASUREMENT_WINDOW start_us=0 end_us=900000',
   'MOTE_LOG time_us=100 id=2 APP_PACKET_TX source_mote_id=2 sequence=1 traffic_class=H payload_length=10 deadline_ms=1',
   'MOTE_LOG time_us=100 id=3 APP_PACKET_TX source_mote_id=3 sequence=1 traffic_class=L payload_length=8 deadline_ms=0',
   'MOTE_LOG time_us=150 id=2 APP_PACKET_TX source_mote_id=2 sequence=2 traffic_class=H payload_length=10 deadline_ms=0',
   'MOTE_LOG time_us=100 id=2 APP_PACKET_TX source_mote_id=2 sequence=bad traffic_class=L payload_length=5 deadline_ms=0',
   'MOTE_LOG time_us=2000 id=1 APP_PACKET_RX source_mote_id=2 sequence=1 traffic_class=H payload_length=10',
   'MOTE_LOG time_us=3000 id=1 APP_PACKET_RX source_mote_id=2 sequence=1 traffic_class=H payload_length=10',
   'MOTE_LOG time_us=400 id=1 APP_PACKET_RX source_mote_id=3 sequence=1 traffic_class=L payload_length=8',
   'MOTE_LOG time_us=500 id=2 DSME_DATA_CONFIRM source=2 destination=1 status=7 gts_tx=1',
   'MOTE_LOG time_us=600 id=1 MO_CHANGE_PROPOSED node=1',
   'CALLBACK_COUNTS logCallbacks=1 radio_tx=4 radio_rx=5','RUN_RESULT durationReached=true pass=true']
  with tempfile.TemporaryDirectory() as d:
   log=Path(d)/'in.log'; log.write_text('\n'.join(lines)+'\n'); s=mod.analyze(log,Path(d)/'out')
   self.assertEqual(s['generated_packets'],3); self.assertEqual(s['unique_received_packets'],2); self.assertAlmostEqual(s['packet_delivery_ratio'],2/3); self.assertEqual(s['duplicate_receptions'],1)
   self.assertEqual(s['deadline_misses'],1); self.assertEqual(s['gts_payload_confirmations_failed'],1); self.assertEqual(s['radio_tx_callbacks'],4)
   self.assertEqual(s['anomalies']['proposals_without_network_completion'],1)
 def test_invalid_timestamp_and_missing_fields_are_anomalies(self):
  with tempfile.TemporaryDirectory() as d:
   log=Path(d)/'bad.log'; log.write_text('MOTE_LOG time_us=x id=1 APP_PACKET_TX source_mote_id=1 sequence=1\nMOTE_LOG time_us=2 id=1 APP_PACKET_RX sequence=nope\n')
   s=mod.analyze(log,Path(d)/'out',1); self.assertGreaterEqual(s['malformed_or_duplicate_tx_events'],2)
   self.assertFalse(s['measurement_window']['complete'])
if __name__=='__main__': unittest.main()
