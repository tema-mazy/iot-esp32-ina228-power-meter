#!/usr/bin/env python3
"""Live discharge dashboard: static HTML shell, charts drawn in the browser.

    ./plot_discharge.py --serve 8080 discharge*.jsonl      # live, self-serving
    ./plot_discharge.py -o discharge.html discharge*.jsonl # write the shell only

The page fetches the JSONL logs and redraws in place every refresh interval.
Nothing is regenerated server-side, so hover state, scroll position and open
sections survive an update - which a whole-page reload destroys.

Note fetch() is blocked on file:// URLs, so --serve exists to hand the page and
the logs off the same origin. Requires no third-party packages.

Panels:
  1  Pack voltage, measured against IR-compensated
  2  Load current
  3  State of charge, coulomb count against OCV lookup
  4  Measured OCV curve against the firmware's lookup table
  5  Predicted time-of-empty, as recorded, against what actually happened

Panel 4 grades the firmware's table; panel 5 grades the predictor.
"""

import argparse
import json
import os
import sys

# Must match main/gauge.c OCV_LIION exactly - this is the thing under test.
OCV_LIION = [[4.20, 100], [4.10, 90], [4.00, 80], [3.90, 70], [3.82, 60],
             [3.72, 50], [3.65, 40], [3.58, 30], [3.50, 20], [3.40, 10],
             [3.20, 5], [3.00, 0]]

# Validated categorical slots 1 and 2 in both modes (blue, orange).
LIGHT = {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e",
         "grid": "#e6e5e1", "s1": "#2a78d6", "s2": "#eb6834"}
DARK = {"surface": "#1a1a19", "ink": "#ffffff", "ink2": "#c3c2b7",
        "grid": "#333331", "s1": "#3987e5", "s2": "#d95926"}


def shell(cfg):
    c = json.dumps(cfg)
    light = ";".join(f"--{k.replace('s1','series-1').replace('s2','series-2')}:{v}"
                     for k, v in LIGHT.items())
    dark = ";".join(f"--{k.replace('s1','series-1').replace('s2','series-2')}:{v}"
                    for k, v in DARK.items())
    return """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Battery discharge</title>
<style>
.viz{__LIGHT__}
@media (prefers-color-scheme: dark){.viz{__DARK__}}
.viz{background:var(--surface);color:var(--ink);
 font:14px/1.5 system-ui,-apple-system,sans-serif;margin:0;padding:24px}
h1{font-size:18px;margin:0 0 4px}
.sub{color:var(--ink2);font-size:13px;margin:0 0 18px}
.stats{display:flex;flex-wrap:wrap;gap:20px;margin:0 0 24px;max-width:680px}
.stat{min-width:96px}
.stat b{display:block;font-size:22px;font-weight:650;line-height:1.15}
.stat span{color:var(--ink2);font-size:12px}
figure{margin:0 0 26px;max-width:680px}
figcaption{font-weight:600;margin-bottom:2px}
.note{color:var(--ink2);font-size:12px;margin:0 0 4px}
svg{width:100%;height:auto;overflow:visible;display:block}
.grid,.axis{stroke:var(--grid);stroke-width:1}
.tick,.axlabel{fill:var(--ink2);font-size:11px}
.line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.s1{stroke:var(--series-1)}.s2{stroke:var(--series-2)}
.s1f{fill:var(--series-1)}.s2f{fill:var(--series-2)}
.dlabel{font-size:11px;font-weight:600}
.legend{display:flex;gap:16px;margin-top:4px;color:var(--ink2);font-size:12px}
.legend i{display:inline-block;width:10px;height:10px;border-radius:2px;
 margin-right:5px;vertical-align:-1px}
.legend i.s1{background:var(--series-1)}.legend i.s2{background:var(--series-2)}
.cross{stroke:var(--ink2);stroke-width:1;stroke-dasharray:3 3}
#tip{position:fixed;pointer-events:none;background:var(--surface);
 border:1px solid var(--grid);border-radius:6px;padding:6px 9px;font-size:12px;
 box-shadow:0 2px 10px rgba(0,0,0,.18);display:none;white-space:pre;z-index:9}
details{max-width:680px;margin:8px 0 24px}
table{border-collapse:collapse;font-size:12px;width:100%}
th,td{text-align:right;padding:2px 8px;border-bottom:1px solid var(--grid)}
th{color:var(--ink2);font-weight:600}
#err{color:#d03b3b;font-size:13px}
</style></head>
<body class="viz">
<h1>Battery discharge</h1>
<p class="sub" id="sub">loading...</p>
<div class="stats" id="stats"></div>
<div id="panels"></div>
<details><summary>Data table (every Nth row)</summary><div id="table"></div></details>
<div id="tip"></div>
<p id="err"></p>
<script>
const CFG = __CFG__;
const tip = document.getElementById('tip');
const W = 620, H = 260, PAD = {l:62, r:100, t:20, b:40};
const pw = W - PAD.l - PAD.r, ph = H - PAD.t - PAD.b;

function socFromOcv(v){
  const t = CFG.ocv;
  if (v >= t[0][0]) return 100;
  if (v <= t[t.length-1][0]) return 0;
  for (let i=0;i<t.length-1;i++){
    const [vh,sh]=t[i],[vl,sl]=t[i+1];
    if (v<=vh && v>=vl) return sl + (v-vl)/(vh-vl)*(sh-sl);
  }
  return 0;
}

// Decimals derived from the tick step. A fixed format repeats labels when the
// range is narrow: 0.8, 0.9, 0.9, 1.0, 1.0.
function autoFmt(step){
  const d = Math.max(0, Math.min(4, Math.ceil(-Math.log10(Math.abs(step)||1))));
  return v => v.toFixed(d);
}

function ticks(lo, hi, n){
  if (hi<=lo) hi = lo+1;
  const raw=(hi-lo)/n;
  let mag=Math.pow(10, Math.floor(Math.log10(Math.abs(raw)||1)));
  let step=[1,2,2.5,5,10].map(m=>mag*m).find(s=>s>=raw) || mag*10;
  const out=[]; let v=Math.floor(lo/step)*step;
  while (v<=hi+step*1e-3){ if (v>=lo-step*1e-3) out.push(v); v+=step; }
  return out;
}

// One panel. series = [{label, pts:[[x,y]...], slot, dashed}]
function draw(el, spec){
  const S = spec.series.filter(s=>s.pts.length);
  if (!S.length){ el.innerHTML=''; return; }
  const xs=S.flatMap(s=>s.pts.map(p=>p[0])), ys=S.flatMap(s=>s.pts.map(p=>p[1]));
  const x0=Math.min(...xs), x1=Math.max(...xs);
  let y0=Math.min(...ys), y1=Math.max(...ys);
  const span=(y1-y0)||1; y0-=span*0.08; y1+=span*0.08;
  const px=x=>PAD.l+(x1===x0?0:(x-x0)/(x1-x0)*pw);
  const py=y=>PAD.t+ph-(y-y0)/(y1-y0)*ph;
  const o=[`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${spec.title}">`];
  const yt=ticks(y0,y1,5), xt=ticks(x0,x1,6);
  const fyT = yt.length>1 ? autoFmt(yt[1]-yt[0]) : spec.fy;
  const fxT = xt.length>1 ? autoFmt(xt[1]-xt[0]) : spec.fx;
  for (const t of yt){
    const y=py(t);
    o.push(`<line class="grid" x1="${PAD.l}" y1="${y.toFixed(1)}" x2="${PAD.l+pw}" y2="${y.toFixed(1)}"/>`);
    o.push(`<text class="tick" x="${PAD.l-8}" y="${(y+4).toFixed(1)}" text-anchor="end">${fyT(t)}</text>`);
  }
  for (const t of xt){
    if (t<x0||t>x1) continue;
    o.push(`<text class="tick" x="${px(t).toFixed(1)}" y="${PAD.t+ph+20}" text-anchor="middle">${fxT(t)}</text>`);
  }
  o.push(`<line class="axis" x1="${PAD.l}" y1="${PAD.t+ph}" x2="${PAD.l+pw}" y2="${PAD.t+ph}"/>`);
  o.push(`<text class="axlabel" x="${PAD.l+pw/2}" y="${H-6}" text-anchor="middle">${spec.xlabel}</text>`);
  o.push(`<text class="axlabel" transform="translate(14,${PAD.t+ph/2}) rotate(-90)" text-anchor="middle">${spec.ylabel}</text>`);
  // Break the path wherever samples are far apart in x. Drawing a straight
  // line across a ten-hour gap between runs would read as measured data.
  const GAP = spec.gapX || 0;
  for (const s of S){
    let d='', prev=null;
    for (const p of s.pts){
      const cmd = (prev===null || (GAP && p[0]-prev > GAP)) ? 'M' : 'L';
      d += cmd + px(p[0]).toFixed(1) + ',' + py(p[1]).toFixed(1) + ' ';
      prev = p[0];
    }
    o.push(`<path class="line s${s.slot}" d="${d.trim()}"${s.dashed?' stroke-dasharray="6 4"':''}/>`);
  }
  // Direct labels last, nudged apart where series end close together -
  // otherwise they overprint into an unreadable smear. Identity is never
  // colour-alone, and the sub-3:1 slots need visible labels as relief.
  const LBL_H=13;
  const lab=S.map(s=>{const p=s.pts[s.pts.length-1];
    return {t:s.label, slot:s.slot, x:px(p[0]), y:py(p[1])};});
  lab.sort((a,b)=>a.y-b.y);
  for (let i=1;i<lab.length;i++)
    if (lab[i].y-lab[i-1].y < LBL_H) lab[i].y = lab[i-1].y + LBL_H;
  const over = lab.length ? lab[lab.length-1].y-(PAD.t+ph) : 0;
  if (over>0) for (const l of lab) l.y -= over;   // keep them inside the plot
  for (const l of lab)
    o.push(`<text class="dlabel s${l.slot}f" x="${(l.x+8).toFixed(1)}" y="${(l.y+4).toFixed(1)}">${l.t}</text>`);
  if (spec.gapX && spec.gaps){
    for (const g of spec.gaps){
      if (g[1]<x0||g[0]>x1) continue;
      o.push(`<rect x="${px(g[0]).toFixed(1)}" y="${PAD.t}" width="${(px(g[1])-px(g[0])).toFixed(1)}" height="${ph}" fill="var(--ink2)" opacity="0.08"/>`);
    }
  }
  o.push(`<line class="cross" x1="0" y1="${PAD.t}" x2="0" y2="${PAD.t+ph}" style="display:none"/>`);
  o.push(`<rect class="hit" x="${PAD.l}" y="${PAD.t}" width="${pw}" height="${ph}" fill="transparent"/>`);
  o.push('</svg>');
  el.innerHTML=o.join('');

  const svg=el.querySelector('svg'), cross=el.querySelector('.cross');
  el.querySelector('.hit').addEventListener('mousemove',e=>{
    const b=svg.getBoundingClientRect(), sx=(e.clientX-b.left)/b.width*W;
    const rows=[];
    for (const s of S){
      let best=null,bd=1e9;
      for (const p of s.pts){ const d=Math.abs(px(p[0])-sx); if(d<bd){bd=d;best=p;} }
      if (best) rows.push(s.label+': '+spec.fy(best[1]));
    }
    if (!rows.length) return;
    cross.style.display=''; cross.setAttribute('x1',sx); cross.setAttribute('x2',sx);
    tip.style.display='block';
    tip.style.left=(e.clientX+14)+'px'; tip.style.top=(e.clientY+14)+'px';
    tip.textContent=rows.join('\\n');
  });
  el.querySelector('.hit').addEventListener('mouseleave',()=>{
    cross.style.display='none'; tip.style.display='none';
  });
}

async function fetchRows(){
  const all=[];
  for (const f of CFG.logs){
    let txt;
    try { txt = await (await fetch(f+'?t='+Date.now())).text(); }
    catch(e){ continue; }
    for (const line of txt.split('\\n')){
      const s=line.trim(); if(!s) continue;
      try { const r=JSON.parse(s); if('v' in r && 'ts' in r) all.push(r); } catch(e){}
    }
  }
  all.sort((a,b)=>a.ts-b.ts);
  return all;
}

function panelSpecs(rows){
  const N=CFG.series, t0=rows[0].ts, hrs=rows.map(r=>(r.ts-t0)/3600);
  // Any interval far larger than the sampling period is a gap: the device was
  // disconnected, or this is a separate run. Shaded, and the line is broken.
  const GAPH = 10/60;
  const gaps=[];
  for (let i=1;i<hrs.length;i++)
    if (hrs[i]-hrs[i-1] > GAPH) gaps.push([hrs[i-1],hrs[i]]);
  const f2=v=>v.toFixed(2), f0=v=>v.toFixed(0), f1=v=>v.toFixed(1), f3=v=>v.toFixed(3);
  const P=[];
  P.push({key:'v', title:'Pack voltage', ylabel:'V', xlabel:'hours', fy:f2, fx:f1,
    note:'IR-corrected removes the sag from load current, using the resistance the firmware learned.',
    gapX:GAPH, gaps, series:[
      {label:'measured', slot:1, pts:rows.map((r,i)=>[hrs[i],r.v])},
      {label:'IR-corrected', slot:2, pts:rows.filter(r=>r.v_ocv!==undefined).map(r=>[(r.ts-t0)/3600,r.v_ocv])},
    ]});
  P.push({key:'i', title:'Load current', ylabel:'mA', xlabel:'hours', fy:f0, fx:f1,
    note:'', gapX:GAPH, gaps, series:[{label:'current', slot:1, pts:rows.map((r,i)=>[hrs[i],r.i*1000])}]});
  if (rows.some(r=>r.soc!==undefined)){
    P.push({key:'soc', title:'State of charge', ylabel:'%', xlabel:'hours', fy:f1, fx:f1,
      note:'Coulomb count is the gauge. OCV lookup is what voltage alone would say - the gap is IR sag plus curve flatness.',
      gapX:GAPH, gaps, series:[
        {label:'coulomb count', slot:1, pts:rows.filter(r=>r.soc!==undefined).map(r=>[(r.ts-t0)/3600,r.soc])},
        {label:'OCV lookup', slot:2, dashed:true, pts:rows.map((r,i)=>[hrs[i],socFromOcv((r.v_ocv!==undefined?r.v_ocv:r.v)/N)])},
      ]});
    P.push({key:'ocv', title:'OCV curve vs firmware table', ylabel:'V/cell', xlabel:'SoC %', fy:f3, fx:f0,
      note:'If measured departs from the table, the table in gauge.c is what needs correcting.',
      series:[
        {label:'measured', slot:1, pts:rows.filter(r=>r.soc!==undefined).map(r=>[r.soc,(r.v_ocv!==undefined?r.v_ocv:r.v)/N])},
        {label:'table', slot:2, dashed:true, pts:CFG.ocv.slice().reverse().map(p=>[p[1],p[0]])},
      ]});
  }
  const pr=rows.filter(r=>r.pred&&r.pred.empty_ts);
  if (pr.length){
    const last=rows[rows.length-1], vpc=last.v/N;
    const done = vpc<=3.05 || (last.soc!==undefined&&last.soc<=2);
    const s=[{label:'predicted', slot:1, pts:pr.map(r=>[(r.ts-t0)/3600,(r.pred.empty_ts-t0)/3600])}];
    if (done){
      const e=(last.ts-t0)/3600;
      s.push({label:'actual', slot:2, dashed:true,
              pts:[[(pr[0].ts-t0)/3600,e],[(pr[pr.length-1].ts-t0)/3600,e]]});
    }
    P.push({key:'pred', title:'Predicted time of empty (recorded as made)',
      ylabel:'hours from start', xlabel:'hours', fy:f1, fx:f1,
      note:done?'Converging on the dashed line means the prediction was right.'
                :'Flattening out means the prediction is settling. The actual line appears once the run reaches empty.',
      gapX:GAPH, gaps, series:s});
  }
  return P;
}

function stats(rows){
  const N=CFG.series, last=rows[rows.length-1], first=rows[0];
  let dq=0;
  for (let i=1;i<rows.length;i++){
    const step=rows[i].q_c-rows[i-1].q_c;
    if (step>=-1) dq+=step;              // skip CHARGE resets across reboots
  }
  const mah=dq/3.6, hrs=(last.ts-first.ts)/3600;
  const p=last.pred||{};
  return [
    ['SoC', last.soc!==undefined?last.soc.toFixed(1)+'%':'-'],
    ['V/cell', (last.v/N).toFixed(3)],
    ['current', (last.i*1000).toFixed(0)+' mA'],
    ['consumed', mah.toFixed(0)+' mAh'],
    ['elapsed', hrs.toFixed(2)+' h'],
    ['predicted left', p.h_to_empty!=null?p.h_to_empty.toFixed(1)+' h':'-'],
    ['capacity factor', p.factor!=null?'x'+p.factor.toFixed(3):'-'],
  ];
}

async function update(){
  const err=document.getElementById('err');
  let rows;
  try { rows = await fetchRows(); }
  catch(e){ err.textContent='fetch failed: '+e; return; }
  if (rows.length<2){ err.textContent='waiting for samples...'; return; }
  err.textContent='';

  const last=rows[rows.length-1], age=(Date.now()/1000-last.ts);
  document.getElementById('sub').textContent =
    rows.length+' samples over '+((last.ts-rows[0].ts)/3600).toFixed(2)+' h'
    +' - '+CFG.series+'S pack - last sample '+age.toFixed(0)+'s ago'
    +(age>150?'  [LOGGER STALLED]':'');

  document.getElementById('stats').innerHTML =
    stats(rows).map(([k,v])=>`<div class="stat"><b>${v}</b><span>${k}</span></div>`).join('');

  const host=document.getElementById('panels');
  for (const spec of panelSpecs(rows)){
    let fig=host.querySelector('[data-key="'+spec.key+'"]');
    if (!fig){
      fig=document.createElement('figure');
      fig.dataset.key=spec.key;
      fig.innerHTML='<figcaption></figcaption><p class="note"></p><div class="plot"></div><div class="legend"></div>';
      host.appendChild(fig);
    }
    fig.querySelector('figcaption').textContent=spec.title;
    const note=fig.querySelector('.note');
    note.textContent=spec.note||''; note.style.display=spec.note?'':'none';
    draw(fig.querySelector('.plot'), spec);
    const S=spec.series.filter(s=>s.pts.length);
    fig.querySelector('.legend').innerHTML =
      S.length>1 ? S.map(s=>`<span><i class="s${s.slot}"></i>${s.label}</span>`).join('') : '';
  }

  const step=Math.max(1,Math.floor(rows.length/60));
  const head=['h','V','V_ocv','mA','SoC %','mAh left','pred h'];
  document.getElementById('table').innerHTML =
    '<table><thead><tr>'+head.map(h=>'<th>'+h+'</th>').join('')+'</tr></thead><tbody>'
    + rows.filter((_,i)=>i%step===0).map(r=>'<tr>'+[
        ((r.ts-rows[0].ts)/3600).toFixed(2), r.v.toFixed(4),
        (r.v_ocv||0).toFixed(4), (r.i*1000).toFixed(0),
        (r.soc||0).toFixed(1), (r.mah_left||0).toFixed(0),
        ((r.pred||{}).h_to_empty||0).toFixed(2),
      ].map(c=>'<td>'+c+'</td>').join('')+'</tr>').join('')
    + '</tbody></table>';
}

update();
setInterval(update, CFG.refresh_s*1000);
</script>
</body></html>
""".replace("__CFG__", c).replace("__LIGHT__", light).replace("__DARK__", dark)


def main():
    ap = argparse.ArgumentParser(description="Live battery discharge dashboard")
    ap.add_argument("logs", nargs="+", help="JSONL logs from monitor.py --json")
    ap.add_argument("-o", "--out", default="discharge.html")
    ap.add_argument("--series", type=int, default=5)
    ap.add_argument("--refresh", type=int, default=60,
                    help="seconds between data refetches (default 60)")
    ap.add_argument("--serve", type=int, metavar="PORT",
                    help="serve the page and logs on this port")
    args = ap.parse_args()

    cfg = {"logs": [os.path.basename(p) for p in args.logs],
           "series": args.series, "refresh_s": args.refresh, "ocv": OCV_LIION}
    html = shell(cfg)
    with open(args.out, "w") as f:
        f.write(html)
    print(f"wrote {args.out}")

    if not args.serve:
        print("note: fetch() is blocked on file:// - use --serve to view it")
        return 0

    import http.server
    import socketserver
    socketserver.TCPServer.allow_reuse_address = True

    # Serve ONLY the page and the named logs. SimpleHTTPRequestHandler would
    # expose the whole working directory - .git included - plus directory
    # listings, to anything that can reach the port. A dashboard needs three
    # files, so it should serve three files.
    allowed = {os.path.basename(args.out)} | set(cfg["logs"])

    class Restricted(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            name = self.path.lstrip("/").split("?")[0]
            if name not in allowed:
                self.send_error(404)
                return
            try:
                with open(name, "rb") as fh:
                    body = fh.read()
            except OSError:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type",
                             "text/html; charset=utf-8" if name.endswith(".html")
                             else "application/x-ndjson; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            # Logs are appended continuously; a cached copy looks frozen.
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    with socketserver.TCPServer(("127.0.0.1", args.serve), Restricted) as httpd:
        print(f"http://127.0.0.1:{args.serve}/{os.path.basename(args.out)}"
              f"   (refreshes every {args.refresh}s, Ctrl-C to stop)")
        print(f"serving only: {', '.join(sorted(allowed))}"
              f"   (loopback only, no directory listing)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
