#pragma once

#include <Arduino.h>

// UI is embedded rather than served from a filesystem so the whole device is
// one .bin: no separate `pio run -t uploadfs` step, and OTA cannot leave the
// firmware and the UI out of step. No external references - the device is
// usually on an isolated network with no route to a CDN.

const char kStyleCss[] PROGMEM = R"CSS(
:root{--bg:#12141a;--card:#1b1e26;--card2:#232734;--line:#2e3342;--fg:#e8eaf0;
--muted:#9aa1b4;--accent:#4a9eff;--ok:#3aa655;--bad:#e8342e;--radius:12px}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,
-apple-system,Segoe UI,Roboto,sans-serif;-webkit-text-size-adjust:100%}
a{color:var(--accent)}
.wrap{max-width:1100px;margin:0 auto;padding:16px}
header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;
justify-content:space-between;padding:16px;background:var(--card);
border:1px solid var(--line);border-radius:var(--radius);margin-bottom:16px}
h1{font-size:19px;margin:0;letter-spacing:.2px}
.host{font-size:13px;color:var(--muted);margin-top:2px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;
margin-right:6px;vertical-align:1px}
.dot.up{background:var(--ok)}.dot.down{background:var(--bad)}
nav{display:flex;gap:8px}
.btn{background:var(--card2);color:var(--fg);border:1px solid var(--line);
padding:8px 14px;border-radius:8px;cursor:pointer;font-size:14px;
text-decoration:none;
/* .btn is used on both <a> and <button>. Form controls do not inherit the page
font or line-height, and an inline-block anchor lays its text out on the line
box while a button centres its own content, so the two disagree vertically.
Inheriting the font and centring both as flex containers makes them identical. */
font-family:inherit;line-height:1.4;
display:inline-flex;align-items:center;justify-content:center}
.btn:hover{border-color:var(--accent)}
.btn.danger:hover{border-color:var(--bad);color:var(--bad)}
.master{display:flex;align-items:center;gap:12px}
.grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(300px,1fr))}
/* The inset highlight keeps the Black tilt's #2b2b2b edge legible against the
dark card without misrepresenting its colour. */
.card{background:var(--card);border:1px solid var(--line);
border-left:5px solid var(--sw);border-radius:var(--radius);padding:14px;
box-shadow:inset 1px 0 0 rgba(255,255,255,.22)}
.card.off{opacity:.5}
.card h2{font-size:16px;margin:0;display:flex;align-items:center;gap:9px}
.sw{width:14px;height:14px;border-radius:50%;background:var(--sw);
box-shadow:0 0 0 1px rgba(255,255,255,.35)}
.top{display:flex;justify-content:space-between;align-items:center;
margin-bottom:12px;gap:8px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
label{display:block;font-size:12px;color:var(--muted);margin-bottom:4px}
input[type=number]{width:100%;background:var(--card2);color:var(--fg);
border:1px solid var(--line);border-radius:8px;padding:8px;font-size:15px;
font-variant-numeric:tabular-nums}
input[type=number]:focus{outline:none;border-color:var(--accent)}
.wire{font-size:12px;color:var(--muted);font-variant-numeric:tabular-nums;
border-top:1px dashed var(--line);padding-top:9px;margin-top:2px}
.wire b{color:var(--fg);font-weight:600}
.tog{position:relative;display:inline-block;width:42px;height:24px;flex:none}
.tog input{opacity:0;width:0;height:0}
.sl{position:absolute;inset:0;background:#3a3f4f;border-radius:24px;
cursor:pointer;transition:.15s}
.sl:before{content:"";position:absolute;height:18px;width:18px;left:3px;
top:3px;background:#fff;border-radius:50%;transition:.15s}
input:checked+.sl{background:var(--ok)}
input:checked+.sl:before{transform:translateX(18px)}
.pro{display:flex;align-items:center;gap:7px;font-size:12px;color:var(--muted)}
.tog.sm{width:34px;height:20px}
.tog.sm .sl:before{height:14px;width:14px}
.tog.sm input:checked+.sl:before{transform:translateX(14px)}
footer{margin:20px 0 8px;text-align:center;font-size:12px;color:var(--muted)}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%)
translateY(70px);background:var(--ok);color:#fff;padding:9px 20px;
border-radius:20px;font-size:14px;opacity:0;transition:.2s;pointer-events:none}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
#toast.err{background:var(--bad)}
@media(max-width:520px){header{flex-direction:column;align-items:stretch}
nav{justify-content:stretch}.btn{flex:1;text-align:center}}
)CSS";

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tilt Simulator</title><link rel="stylesheet" href="/style.css">
</head><body><div class="wrap">
<header>
  <div>
    <h1>Tilt Simulator</h1>
    <div class="host"><span id="dot" class="dot down"></span><span id="host">connecting…</span></div>
  </div>
  <div class="master">
    <span>All advertising</span>
    <label class="tog"><input type="checkbox" id="master"><span class="sl"></span></label>
  </div>
  <nav><a class="btn" href="/ota">Firmware</a>
  <button class="btn danger" id="forget">Forget WiFi</button></nav>
</header>
<div class="grid" id="grid"></div>
<footer id="foot"></footer>
</div><div id="toast"></div>
<script src="/app.js"></script></body></html>
)HTML";

const char kOtaHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware · Tilt Simulator</title><link rel="stylesheet" href="/style.css">
<style>
.panel{background:var(--card);border:1px solid var(--line);
border-radius:var(--radius);padding:20px;max-width:540px;margin:0 auto}
.panel p{color:var(--muted);font-size:14px}
#bar{height:10px;background:var(--card2);border-radius:6px;overflow:hidden;
margin:16px 0 8px;display:none}
#fill{height:100%;width:0;background:var(--accent);transition:width .2s}
#pct{font-size:13px;color:var(--muted);text-align:center;min-height:20px}
input[type=file]{width:100%;background:var(--card2);border:1px solid var(--line);
border-radius:8px;padding:10px;color:var(--fg);margin-bottom:14px}
#go{width:100%;padding:11px;font-size:15px}
.msg{margin-top:14px;padding:11px;border-radius:8px;font-size:14px;display:none}
.msg.ok{display:block;background:rgba(58,166,85,.15);color:#8ee0a1;
border:1px solid var(--ok)}
.msg.bad{display:block;background:rgba(232,52,46,.15);color:#ffa8a4;
border:1px solid var(--bad)}
</style></head><body><div class="wrap">
<header>
  <div><h1>Firmware update</h1><div class="host" id="host">…</div></div>
  <nav><a class="btn" href="/">Back to tilts</a></nav>
</header>
<div class="panel">
  <p>Upload a <code>firmware.bin</code> built for this board. Advertising stops
  during the upload and the device reboots when it finishes. Do not remove
  power while it is writing.</p>
  <input type="file" id="file" accept=".bin">
  <button class="btn" id="go">Upload and install</button>
  <div id="bar"><div id="fill"></div></div>
  <div id="pct"></div>
  <div class="msg" id="msg"></div>
</div>
<footer id="foot"></footer>
</div>
<script>
const $=i=>document.getElementById(i);
fetch('/api/state').then(r=>r.json()).then(s=>{
  $('host').textContent=s.hostname+'.local · '+s.ip;
  $('foot').textContent=s.name+' v'+s.version+' · built '+s.built
    +' · slot '+s.partition+' · '+s.sketchMd5.slice(0,8);
}).catch(()=>{});

function say(t,bad){const m=$('msg');m.textContent=t;m.className='msg '+(bad?'bad':'ok');}

$('go').addEventListener('click',()=>{
  const f=$('file').files[0];
  if(!f){say('Choose a .bin file first',1);return;}
  $('go').disabled=true;$('bar').style.display='block';
  const fd=new FormData();fd.append('update',f);
  const x=new XMLHttpRequest();
  x.upload.onprogress=e=>{
    if(!e.lengthComputable)return;
    const p=Math.round(e.loaded/e.total*100);
    $('fill').style.width=p+'%';$('pct').textContent=p+'%';
  };
  x.onload=()=>{
    if(x.status===200){
      say('Installed. Rebooting…');
      let n=12;
      const t=setInterval(()=>{
        say('Installed. Reloading in '+(--n)+'s…');
        if(n<=0){clearInterval(t);location.href='/';}
      },1000);
    } else {
      say(x.responseText||'Update failed',1);$('go').disabled=false;
    }
  };
  x.onerror=()=>{say('Connection lost during upload',1);$('go').disabled=false;};
  x.open('POST','/update');x.send(fd);
});
</script></body></html>
)HTML";

const char kAppJs[] PROGMEM = R"JS(
let state=null,timer=null;
const $=id=>document.getElementById(id);

function toast(msg,err){const t=$('toast');t.textContent=msg;
  t.className='show'+(err?' err':'');clearTimeout(timer);
  timer=setTimeout(()=>t.className='',1600);}

// Mirrors the firmware: pro Tilts advertise 10x so they carry one more decimal.
const dp=(pro,g)=>pro?(g?4:1):(g?3:0);
const enc=(v,pro,g)=>Math.min(65535,Math.max(0,
  Math.round(v*(g?(pro?10000:1000):(pro?10:1)))));

function card(t,i){
  const c=document.createElement('div');
  c.className='card'+(t.enabled?'':' off');
  c.style.setProperty('--sw',t.swatch);
  c.innerHTML=`<div class="top">
    <h2><span class="sw"></span>${t.name}</h2>
    <div class="master">
      <span class="pro"><label class="tog sm"><input type="checkbox" data-f="pro"
        ${t.pro?'checked':''}><span class="sl"></span></label>Pro</span>
      <label class="tog"><input type="checkbox" data-f="enabled"
        ${t.enabled?'checked':''}><span class="sl"></span></label>
    </div></div>
  <div class="row">
    <div><label>Temperature °F</label><input type="number" data-f="tempF"
      step="${t.pro?0.1:1}" min="-40" max="250" value="${t.tempF.toFixed(dp(t.pro,0))}"></div>
    <div><label>Variance ±°F</label><input type="number" data-f="tempVarianceF"
      step="0.1" min="0" max="20" value="${t.tempVarianceF.toFixed(1)}"></div>
  </div>
  <div class="row">
    <div><label>Gravity SG</label><input type="number" data-f="gravity"
      step="${t.pro?0.0001:0.001}" min="0.9" max="2" value="${t.gravity.toFixed(dp(t.pro,1))}"></div>
    <div><label>Variance ±SG</label><input type="number" data-f="gravityVariance"
      step="0.0001" min="0" max="0.1" value="${t.gravityVariance.toFixed(4)}"></div>
  </div>
  <div class="wire">on air: major <b>${enc(t.tempF,t.pro,0)}</b>
    · minor <b>${enc(t.gravity,t.pro,1)}</b></div>`;

  c.querySelectorAll('[data-f]').forEach(el=>{
    el.addEventListener('change',()=>{
      const f=el.dataset.f;
      const v=el.type==='checkbox'?el.checked:parseFloat(el.value);
      if(el.type!=='checkbox'&&isNaN(v)){toast('Not a number',1);return;}
      send(i,{[f]:v});
    });
  });
  return c;
}

function render(){
  $('host').textContent=state.hostname+'.local · '+state.ip;
  $('dot').className='dot '+(state.connected?'up':'down');
  $('master').checked=state.masterEnabled;
  $('foot').textContent=state.name+' v'+state.version+' · built '+state.built;
  const g=$('grid');g.innerHTML='';
  state.tilts.forEach((t,i)=>g.appendChild(card(t,i)));
}

async function load(){
  try{const r=await fetch('/api/state');state=await r.json();render();}
  catch(e){toast('Cannot reach device',1);}
}

async function send(i,patch){
  try{
    const r=await fetch('/api/tilt/'+i,{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(patch)});
    if(!r.ok)throw new Error(await r.text());
    state.tilts[i]=await r.json();
    render();toast('Saved');
  }catch(e){toast(e.message||'Save failed',1);load();}
}

$('master').addEventListener('change',async e=>{
  try{
    const r=await fetch('/api/master',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({enabled:e.target.checked})});
    if(!r.ok)throw new Error();
    state.masterEnabled=e.target.checked;toast('Saved');
  }catch(err){toast('Save failed',1);load();}
});

$('forget').addEventListener('click',()=>{
  if(!confirm('Forget the saved WiFi network and reboot into the setup portal?'))return;
  fetch('/api/reset-wifi',{method:'POST'});
  toast('Rebooting into setup portal');
});

load();
)JS";
