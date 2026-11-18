#!/usr/bin/env python3
"""Analyze packet QoS and DSME events in a raw GTSFinalRunner log."""
import argparse,csv,json,math,re
from collections import defaultdict
from pathlib import Path
P=re.compile(r'MOTE_LOG time_us=(\d+) id=(\d+) (.*)'); F=re.compile(r'([A-Za-z_][A-Za-z_0-9]*)=([^ ]+)')
def analyze(log,out,duration=None):
 lines=Path(log).read_text(errors='replace').splitlines(); tx={}; rx=defaultdict(list); ev=[]; malformed=0
 for line in lines:
  m=P.match(line)
  if not m:
   if 'APP_PACKET_TX' in line or 'APP_PACKET_RX' in line: malformed+=1
   continue
  t,mote,msg=int(m[1]),int(m[2]),m[3]; f=dict(F.findall(msg))
  if msg.startswith('APP_PACKET_TX '):
   try:
    k=(int(f['source_mote_id']),int(f['sequence']))
    if k in tx: malformed+=1
    else: tx[k]={'t':t,'class':f['traffic_class'],'bytes':int(f['payload_length']),'deadline':int(f.get('deadline_ms',0))}
   except (ValueError,KeyError): malformed+=1
  elif msg.startswith('APP_PACKET_RX '):
   try: rx[(int(f['source_mote_id']),int(f['sequence']))].append(t)
   except (ValueError,KeyError): malformed+=1
  elif 'APP_PACKET_RX_MALFORMED' in msg: malformed+=1
  ev.append((msg,f))
 if duration is None:
  m=re.search(r'SIM_DURATION_START time_us=\d+ duration_us=(\d+)', '\n'.join(lines)); duration=int(m[1])/1e6 if m else None
 sim_end=int(duration*1e6) if duration else None
 mw=re.search(r'APP_MEASUREMENT_WINDOW start_us=(\d+) end_us=(\d+)', '\n'.join(lines))
 start,end=(map(int,mw.groups()) if mw else (0,sim_end))
 duration=(end-start)/1e6 if end is not None else None
 complete=bool(sim_end and end and any('durationReached=true' in x for x in lines) and sim_end>=end)
 measured={k:v for k,v in tx.items() if complete and start<=v['t']<end}
 rows=[]; lat=[]; good=0; deadline_due=deadline_miss=0; duplicates=0
 for k,v in measured.items():
  arrivals=sorted(x for x in rx[k] if x>=v['t']); r=arrivals[0] if arrivals else None; duplicates+=max(0,len(arrivals)-1)
  if r is not None: lat.append(r-v['t']); good+=v['bytes']
  due=v['t']+v['deadline']*1000 if v['deadline'] else None
  if due is not None and due<=sim_end:
   deadline_due+=1; deadline_miss+=int(r is None or r>due)
  rows.append(dict(source_mote_id=k[0],sequence_number=k[1],traffic_class=v['class'],tx_time_us=v['t'],rx_time_us='' if r is None else r,payload_bytes=v['bytes'],deadline_ms=v['deadline'] or '',latency_us='' if r is None else r-v['t'],delivered=int(r is not None),duplicate_receptions=max(0,len(arrivals)-1),deadline_miss='' if due is None or due>end else int(r is None or r>due)))
 out=Path(out); out.mkdir(parents=True,exist_ok=True); cols=list(rows[0]) if rows else ['source_mote_id','sequence_number','traffic_class','tx_time_us','rx_time_us','payload_bytes','deadline_ms','latency_us','delivered','duplicate_receptions','deadline_miss']
 with (out/'packet_metrics.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=cols); w.writeheader(); w.writerows(rows)
 lat.sort(); p95=lat[math.ceil(.95*len(lat))-1]/1000 if lat else None
 s={'input_log':str(Path(log).resolve()),'measurement_window':{'start_us':start,'end_us':end,'duration_seconds':duration,'complete':complete},'generated_packets':len(measured),'unique_received_packets':len(lat),'packet_delivery_ratio':len(lat)/len(measured) if measured else None,'average_latency_ms':sum(lat)/len(lat)/1000 if lat else None,'p95_latency_ms_nearest_rank':p95,'application_goodput_bps':good*8/duration if duration and complete else None,'application_goodput_kbps':good*8/duration/1000 if duration and complete else None,'deadline_metrics':'CONFIGURED' if any(v['deadline'] for v in measured.values()) else 'NOT CONFIGURED','deadline_miss_ratio':deadline_miss/deadline_due if deadline_due else None,'deadline_misses':deadline_miss,'deadline_expired_packets':deadline_due,'duplicate_receptions':duplicates,'malformed_or_duplicate_tx_events':malformed,'gts_payload_confirmations_success':0,'gts_payload_confirmations_failed':0,'cfp_receive_frames':0,'radio_tx_callbacks':None,'radio_rx_callbacks':None,'mo_reconfigurations':0,'mo_proposals':0,'mo_prepare_failures':0,'deferred_proposals':0,'mo_send_failures':0,'percentile_method':'nearest rank: sorted[ceil(0.95*n)-1]','radio_callback_note':'Radio event counts, not direct energy measurements.'}
 for msg,f in ev:
  if msg.startswith('DSME_DATA_CONFIRM ') and f.get('gts_tx')=='1': s['gts_payload_confirmations_success' if f.get('status')=='0' else 'gts_payload_confirmations_failed']+=1
  if msg.startswith('DSME_CFP_RX_FRAME '): s['cfp_receive_frames']+=1
  if msg.startswith('MO_NETWORK_ACTIVE '): s['mo_reconfigurations']+=1
  if msg.startswith('MO_CHANGE_PROPOSED '): s['mo_proposals']+=1
  if msg.startswith('MO_PREPARE_FAILED '): s['mo_prepare_failures']+=1
  if msg.startswith('MO_PROPOSAL_DEFERRED '): s['deferred_proposals']+=1
  if msg.startswith('MO_SEND_FAILED '): s['mo_send_failures']+=1
 for line in lines:
  m=re.search(r'CALLBACK_COUNTS .*radio_tx=(\d+) radio_rx=(\d+)',line)
  if m: s['radio_tx_callbacks'],s['radio_rx_callbacks']=map(int,m.groups())
 pats={'association_failures':r'Association failed|ASSOCIATION_FAILED','gts_management_rejections':r'GTS_FINAL_CONFIRM.*status=(?!SUCCESS\b)[A-Z_]+.*management=1','transaction_overflow':r'TRANSACTION.*OVERFLOW','mo_prepare_failures':r'MO_PREPARE_FAILED','deferred_proposals':r'MO_PROPOSAL_DEFERRED','mo_send_failures':r'MO_SEND_FAILED','stale_mismatched_coordinator_messages':r'MO_COORD_COMMAND_IGNORED.*reason=.*(STALE|MISMATCH)|MO_COORD.*(STALE|MISMATCH)','degraded_quarantined_states':r'MO_NETWORK_DEGRADED|MO_[A-Z_]*QUARANTINED|quarantined=1|degraded=1'}
 mote_lines=[x for x in lines if x.startswith('MOTE_LOG ')]
 s['anomalies']={k:sum(bool(re.search(p,x,re.I)) for x in mote_lines) for k,p in pats.items()}
 s['anomalies']['mo_prepare_failures']=s['mo_prepare_failures']
 s['anomalies']['deferred_proposals']=s['deferred_proposals']
 s['anomalies']['mo_send_failures']=s['mo_send_failures']
 s['anomalies']['proposals_without_network_completion']=max(0,s['mo_proposals']-s['mo_reconfigurations'])
 s['by_traffic_class']={}
 for cls in ('H','L'):
  subset=[r for r in rows if r['traffic_class']==cls]; delivered=[r for r in subset if r['delivered']]
  s['by_traffic_class'][cls]={'generated':len(subset),'unique_received':len(delivered),'packet_delivery_ratio':len(delivered)/len(subset) if subset else None,'deadline_misses':sum(int(r['deadline_miss']) for r in subset if r['deadline_miss']!=''),'deadline_expired_packets':sum(r['deadline_miss']!='' for r in subset)}
 (out/'summary_metrics.json').write_text(json.dumps(s,indent=2)+'\n')
 (out/'metrics_report.txt').write_text('\n'.join(['QoS metrics report',f"Input: {log}",f"Measurement window complete: {complete}",f"Generated/unique received: {len(measured)}/{len(lat)}",f"PDR: {s['packet_delivery_ratio']}",f"Average/P95 latency ms: {s['average_latency_ms']}/{p95}",f"Application goodput bps: {s['application_goodput_bps']}",f"Deadline metrics: {s['deadline_metrics']}",f"Deadline miss ratio: {s['deadline_miss_ratio']}",f"GTS success/failure: {s['gts_payload_confirmations_success']}/{s['gts_payload_confirmations_failed']}",f"CFP RX frames: {s['cfp_receive_frames']}",f"Radio callbacks TX/RX: {s['radio_tx_callbacks']}/{s['radio_rx_callbacks']} (not energy)",f"MO reconfigurations/proposals: {s['mo_reconfigurations']}/{s['mo_proposals']}",f"High-priority generated/received: {s['by_traffic_class']['H']['generated']}/{s['by_traffic_class']['H']['unique_received']}",f"Low-priority generated/received: {s['by_traffic_class']['L']['generated']}/{s['by_traffic_class']['L']['unique_received']}",f"Anomalies: {s['anomalies']}"])+'\n')
 return s
if __name__=='__main__':
 a=argparse.ArgumentParser(); a.add_argument('log'); a.add_argument('--outdir'); a.add_argument('--duration-seconds',type=float); x=a.parse_args(); dest=x.outdir or str(Path(x.log).resolve().parent/(Path(x.log).stem+'-metrics')); s=analyze(x.log,dest,x.duration_seconds); print(f"METRICS_WRITTEN {dest} generated={s['generated_packets']} received={s['unique_received_packets']}")
