#pragma once
#include <Arduino.h>

// Self-contained: works without internet, fonts, CDNs or build tooling.
const char DASHBOARD_HTML[] PROGMEM = R"AIRLUME(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>AirLume · Daily air</title><style>
:root{color-scheme:light;--ink:#193d35;--muted:#5b7069;--line:#dce6de;--green:#24664d;--paper:#f4f7f1}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.55 system-ui,sans-serif}main{max-width:940px;margin:auto;padding:38px 24px}header{display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid var(--line);padding-bottom:22px}.brand{font-weight:750;letter-spacing:-1px;font-size:26px}.connection{font-size:13px;color:var(--muted)}h1{font-size:clamp(30px,6vw,46px);letter-spacing:-1.5px;line-height:1.15;margin:34px 0 10px}p{color:var(--muted);margin:8px 0}.period{margin:24px 0;font-size:14px}.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:16px}article{background:white;border:1px solid var(--line);border-radius:16px;padding:24px}h2{font-size:17px;margin:0;font-weight:650}.reading{font-size:40px;line-height:1.2;font-weight:650;letter-spacing:-1px;margin:18px 0}.unit{font-size:15px;font-weight:400;letter-spacing:0;color:var(--muted)}.badge{display:inline-block;font-size:13px;padding:4px 10px;border-radius:6px;background:#edf2ee;color:#52645d}.badge.ok{background:#e6f2e9;color:#24664d}.badge.high{background:#fff0dc;color:#895000}.detail{font-size:13px;margin-top:16px}.coverage{font-size:12px;margin-top:6px}.notice{padding:16px 20px;background:#e7eee6;border-radius:10px;margin:24px 0;font-size:14px}footer{border-top:1px solid var(--line);padding-top:20px;margin-top:30px;font-size:12px;color:var(--muted)}a{color:var(--green)}@media(max-width:540px){main{padding:24px 16px}.grid{grid-template-columns:1fr}article{padding:22px}header{align-items:flex-start;gap:15px}.connection{max-width:150px;text-align:right}}
</style></head><body><main>
<header><div class="brand">AirLume<span style="color:#73a486">.</span></div><div class="connection" id="connection" role="status">Connecting to your monitor…</div></header>
<h1>A little clarity about your air.</h1><p>Daily averages, with simple references for your room.</p>
<div class="period" id="period">Waiting for the current monitoring period…</div>
<section class="grid" aria-label="Daily averages">
<article id="pm25"><h2>PM2.5</h2><div class="reading"><span class="value">—</span> <span class="unit">µg/m³</span></div><span class="badge">Waiting for data</span><p class="detail">Daily reference: 15 µg/m³ · WHO 24-hour guideline</p><p class="coverage">No readings yet</p></article>
<article id="pm10"><h2>PM10</h2><div class="reading"><span class="value">—</span> <span class="unit">µg/m³</span></div><span class="badge">Waiting for data</span><p class="detail">Daily reference: 45 µg/m³ · WHO 24-hour guideline</p><p class="coverage">No readings yet</p></article>
<article id="co2"><h2>Carbon dioxide</h2><div class="reading"><span class="value">—</span> <span class="unit">ppm</span></div><span class="badge">Waiting for data</span><p class="detail">Ventilation reference: 1,500 ppm · not a health exposure limit</p><p class="coverage">No readings yet</p></article>
<article id="voc"><h2>VOC estimate</h2><div class="reading"><span class="value">—</span> <span class="unit">ppb</span></div><span class="badge">Waiting for valid sensor data</span><p class="detail">Trend only · no generic long-term TVOC limit</p><p class="coverage">No readings yet</p></article>
</section>
<p class="notice" id="notice" role="status">Collecting data. A partial day cannot establish a full-day exposure level.</p>
<footer>Each period is 24 hours from device startup; averages restart after each period or reboot. Missing readings are excluded. Temperature and humidity do not affect air-quality decisions.<br><br>Below a reference does not establish that air is safe for long-term exposure. WHO annual PM guidelines are stricter: PM2.5 5 µg/m³ and PM10 15 µg/m³; a single day cannot assess annual exposure. Daily averages can also hide short spikes; the lamp's live pollutant alerts remain active.<br><br><a href="https://www.who.int/teams/environment-climate-change-and-health/air-quality-and-health/health-impacts/types-of-pollutants">WHO particulate guidelines</a> · <a href="https://www.gov.wales/carbon-dioxide-monitors-education-settings">CO₂ ventilation guidance</a> (links require internet)</footer>
</main><script>
const limits={pm25:15,pm10:45,co2:1500};
function duration(s){return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
function render(d){
 document.getElementById('connection').textContent='Connected · refreshes every 5 seconds';
 document.getElementById('period').textContent='Period '+d.period+' · '+duration(d.elapsed_seconds)+' of 24h · average so far';
 for(const key of ['pm25','pm10','co2','voc']){
  const card=document.getElementById(key),m=d[key],valid=Number.isFinite(m.average),badge=card.querySelector('.badge');
  card.querySelector('.value').textContent=valid?m.average.toLocaleString(undefined,{maximumFractionDigits:key==='co2'||key==='voc'?0:1}):'—';
  badge.className='badge';
  if(!valid)badge.textContent='Waiting for valid sensor data';
  else if(!m.fresh)badge.textContent='Sensor unavailable · earlier average';
  else if(key==='voc')badge.textContent='Trend only';
  else {const high=key==='co2'?m.average>=limits[key]:m.average>limits[key];badge.classList.add(high?'high':'ok');badge.textContent=high?(key==='co2'?'Improve ventilation':'Above daily reference so far'):'Below reference so far';}
  card.querySelector('.coverage').textContent=duration(m.covered_seconds)+' measured · '+Math.round(d.elapsed_seconds?100*m.covered_seconds/d.elapsed_seconds:0)+'% coverage';
 }
 const alerts=[];
 if(d.live_alerts.co2)alerts.push('CO₂ is elevated: improve ventilation.');
 if(d.live_alerts.pm)alerts.push('Particles are elevated: reduce sources or filter the air.');
 if(d.live_alerts.voc)alerts.push('VOC sensor reports poor air: remove sources or ventilate.');
 document.getElementById('notice').textContent=alerts.length?'Live pollutant alert · '+alerts.join(' '):'Averages cover available measurements only. Keep collecting data; this is a partial-day comparison, not a full-day exposure assessment.';
}
async function refresh(){try{const r=await fetch('/api/daily',{cache:'no-store',signal:AbortSignal.timeout(4000)});if(!r.ok)throw Error('HTTP');render(await r.json());}catch(e){document.getElementById('connection').textContent='Disconnected · reconnect to AirLume-AP';document.querySelectorAll('.badge').forEach(b=>{b.className='badge';b.textContent='Offline · readings may be outdated';});document.getElementById('notice').textContent='Connection lost. Displayed averages are from the last successful update.';}finally{setTimeout(refresh,5000);}}
refresh();
</script></body></html>)AIRLUME";
