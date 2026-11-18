#!/usr/bin/env python3
import csv,json,subprocess
from pathlib import Path
BASE=Path(__file__).resolve().parents[3]; R=BASE/'simulation-results'; rows=[]
for label,mode in [('Fixed MO 5','fixed-mo5'),('Fixed MO 6','fixed-mo6'),('Adaptive MO 5–6','adaptive')]:
 logs=sorted(R.glob(f'*-{mode}.log'))
 if not logs: continue
 log=logs[-1]; folder=log.with_name(log.stem+'-metrics'); data=folder/'summary_metrics.json'
 if not data.exists(): subprocess.run(['python3',str(Path(__file__).with_name('analyze_qos_metrics.py')),str(log)],check=True); data=log.with_name(log.stem+'-metrics')/'summary_metrics.json'
 m=json.loads(data.read_text()); rows.append({'scenario':label,'average_latency_ms':m['average_latency_ms'],'p95_latency_ms':m['p95_latency_ms_nearest_rank'],'application_goodput_kbps':m['application_goodput_kbps'],'packet_delivery_ratio':m['packet_delivery_ratio'],'deadline_miss_ratio':m['deadline_miss_ratio'],'gts_success':m['gts_payload_confirmations_success'],'gts_failure':m['gts_payload_confirmations_failed'],'radio_tx_callbacks':m['radio_tx_callbacks'],'radio_rx_callbacks':m['radio_rx_callbacks'],'mo_reconfigurations':m['mo_reconfigurations'],'measurement_complete':m['measurement_window']['complete']})
R.mkdir(exist_ok=True); cols=['scenario','average_latency_ms','p95_latency_ms','application_goodput_kbps','packet_delivery_ratio','deadline_miss_ratio','gts_success','gts_failure','radio_tx_callbacks','radio_rx_callbacks','mo_reconfigurations','measurement_complete']
with (R/'comparison_results.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=cols); w.writeheader(); w.writerows(rows)
from xml.sax.saxutils import escape

def svg_chart(filename,title,ylabel,series):
 data=[(name,[(label,value) for label,value in values if value is not None]) for name,values in series]
 data=[(name,values) for name,values in data if values]
 if not data: return
 width,height=780,450; left,right,top,bottom=85,25,55,100; plot_h=height-top-bottom; plot_w=width-left-right
 vals=[v for _,items in data for _,v in items]; maximum=max(vals) if vals else 1; maximum=maximum or 1
 n=max(len(items) for _,items in data); group=plot_w/max(n,1); colors=['#2563eb','#f97316','#16a34a']
 chunks=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">','<rect width="100%" height="100%" fill="white"/>',f'<text x="{width/2}" y="30" text-anchor="middle" font-family="sans-serif" font-size="18">{escape(title)}</text>',f'<text x="20" y="{height/2}" transform="rotate(-90 20 {height/2})" text-anchor="middle" font-family="sans-serif" font-size="13">{escape(ylabel)}</text>',f'<line x1="{left}" y1="{top}" x2="{left}" y2="{height-bottom}" stroke="#333"/>',f'<line x1="{left}" y1="{height-bottom}" x2="{width-right}" y2="{height-bottom}" stroke="#333"/>',f'<text x="{width/2}" y="{height-18}" text-anchor="middle" font-family="sans-serif" font-size="13">Experiment</text>']
 for tick in range(6):
  value=maximum*tick/5; y=height-bottom-plot_h*tick/5; chunks.extend([f'<line x1="{left-4}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}" stroke="#ddd"/>',f'<text x="{left-8}" y="{y+4:.1f}" text-anchor="end" font-family="sans-serif" font-size="11">{value:.3g}</text>'])
 for si,(label,items) in enumerate(data):
  for i,(name,value) in enumerate(items):
   barw=min(32,group/(len(data)+1)); x=left+i*group+group*(si+0.5)/len(data)-barw/2; bh=plot_h*value/maximum; y=height-bottom-bh
   chunks.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{barw:.1f}" height="{bh:.1f}" fill="{colors[si%len(colors)]}"/>')
   if si==0: chunks.append(f'<text x="{left+i*group+group/2:.1f}" y="{height-bottom+18}" text-anchor="middle" font-family="sans-serif" font-size="10">{escape(name)}</text>')
  chunks.append(f'<rect x="{left+si*155}" y="{height-48}" width="12" height="12" fill="{colors[si%len(colors)]}"/><text x="{left+si*155+18}" y="{height-38}" font-family="sans-serif" font-size="11">{escape(label)}</text>')
 chunks.append('</svg>'); (R/filename).write_text(''.join(chunks)+'\n')

for key,title in [('average_latency_ms','Average latency (ms)'),('p95_latency_ms','P95 latency (ms)'),('application_goodput_kbps','Application goodput (kbps)'),('packet_delivery_ratio','Packet delivery ratio'),('deadline_miss_ratio','Deadline miss ratio'),('mo_reconfigurations','MO reconfigurations (count)')]:
 svg_chart(f'comparison-{key}.svg',title,title,[(title,[(r['scenario'],r[key]) for r in rows])])
svg_chart('comparison-gts-confirmations.svg','GTS payload confirmations','Confirmations (count)',[('Successful',[(r['scenario'],r['gts_success']) for r in rows]),('Failed',[(r['scenario'],r['gts_failure']) for r in rows])])
svg_chart('comparison-radio-activity.svg','Radio activity callbacks','Callbacks (count)',[('TX callbacks',[(r['scenario'],r['radio_tx_callbacks']) for r in rows]),('RX callbacks',[(r['scenario'],r['radio_rx_callbacks']) for r in rows])])

print(f"COMPARISON_WRITTEN {R/'comparison_results.csv'} experiments={len(rows)}")
