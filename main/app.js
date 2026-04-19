'use strict';

const MAX_PORTS    = 5;
const SPARK_WIN    = 80;
const LS_CFGS_KEY  = 'opensense_cfgs_v2';
const LS_PORTS_KEY = 'opensense_ports_v2';

const SENSOR_TYPE_I2C   = 0;
const SENSOR_TYPE_ADC   = 1;
const SENSOR_TYPE_SHT30  = 2;
const SENSOR_TYPE_BME280  = 3;
const SENSOR_TYPE_HIH6130 = 4;

const PORT_META = [
  { label:'Port 1', sub:'I2C · 3.3V · CH1',           v5:false, adc:false },
  { label:'Port 2', sub:'I2C · 3.3V · CH2',           v5:false, adc:false },
  { label:'Port 3', sub:'I2C · 3.3V · CH3',           v5:false, adc:false },
  { label:'Port 4', sub:'I2C or ADC · CH4',           v5:false, adc:true  },
  { label:'Port 5', sub:'I2C or ADC · CH5',           v5:false, adc:true  },
];

// ── State ──────────────────────────────────────────────────────────────────
let savedCfgs    = loadLS(LS_CFGS_KEY,  []);
let portAssigns  = loadLS(LS_PORTS_KEY, {});
let portRawCfgs  = {};
let liveData     = {};
let sparkData    = {};
let axisPrefs    = loadLS('opensense_axes_v1', {});
let chartMode    = loadLS('opensense_chartmode_v1', {});
let editingCfgId   = null;
let assigningPort= null;
let ws=null, wsRetry=null, wsBackoff=1000;

// ── Storage ────────────────────────────────────────────────────────────────
function loadLS(k,d){try{return JSON.parse(localStorage.getItem(k)||'null')||d;}catch(_){return d;}}
function saveLS(k,v){localStorage.setItem(k,JSON.stringify(v));}
function genId(){return 'c'+Date.now().toString(36)+Math.random().toString(36).slice(2,6);}

// ── Navigation ─────────────────────────────────────────────────────────────
function showPage(name, btn) {
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.nb').forEach(b=>b.classList.remove('active'));
  document.getElementById('page-'+name).classList.add('active');
  if (btn) btn.classList.add('active');
  if (name==='ports')    renderPortsPage();
  if (name==='savedcfg') renderSavedCfgsPage();
}

// ── WebSocket ──────────────────────────────────────────────────────────────
function connectWS() {
  if (ws) { try{ws.onclose=ws.onerror=null; ws.close();}catch(_){} ws=null; }
  if (wsRetry){ clearTimeout(wsRetry); wsRetry=null; }

  ws = new WebSocket((location.protocol==='https:'?'wss':'ws')+'://'+location.host+'/ws');

  ws.onopen = () => {
    setWsDot(true);
    wsBackoff = 1000;
    fetchDeviceCfgs();
  };
  ws.onmessage = e => { try{const d=JSON.parse(e.data);if(d.type==='data')onLive(d);}catch(_){} };
  ws.onclose = ws.onerror = () => {
    setWsDot(false);
    ws = null;
    wsRetry = setTimeout(connectWS, wsBackoff);
    wsBackoff = Math.min(wsBackoff * 2, 10000);
  };
}

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      wsBackoff = 1000;
      connectWS();
    }
  }
});

function setWsDot(on) {
  document.getElementById('ws-dot').className = 'dot'+(on?' green':'');
  document.getElementById('ws-label').textContent = on?'Live':'Reconnecting';
  if(!on) { const p=document.getElementById('rssi-pill'); if(p) p.style.display='none'; }
}

// ── Live data ──────────────────────────────────────────────────────────────
function rssiBar(dbm) {
  if(!dbm) return '<span style="color:var(--g400)">-- dBm</span>';
  const col = dbm >= -60 ? 'var(--green)' : dbm >= -75 ? 'var(--amber)' : 'var(--red)';
  return `<span style="color:${col}">${dbm} dBm</span>`;
}
function onLive(d) {
  const pill=document.getElementById('rssi-pill');
  const lbl=document.getElementById('rssi-label');
  if(pill && lbl && d.rssi) {
    pill.style.display='';
    lbl.innerHTML=rssiBar(d.rssi);
  }
  (d.sensors||[]).forEach(s=>{liveData[s.port]=s;});
  const ub=document.getElementById('uptime-badge');
  if(ub&&d.timestamp_us) ub.textContent=(d.timestamp_us/1e6).toFixed(1)+' s uptime';
  if(document.getElementById('page-dashboard').classList.contains('active'))
    updateDashboard(d.sensors||[]);
}

// ── Chart rendering ────────────────────────────────────────────────────────
const CHART_W=260, CHART_H=90, CHART_PL=36, CHART_PR=8, CHART_PT=8, CHART_PB=20;
// inner plot area
const CPW = CHART_W - CHART_PL - CHART_PR;
const CPH = CHART_H - CHART_PT - CHART_PB;

function fmtAxis(v) {
  if(Math.abs(v)>=1000) return (v/1000).toFixed(1)+'k';
  if(Math.abs(v)>=100)  return v.toFixed(0);
  if(Math.abs(v)>=10)   return v.toFixed(1);
  return v.toFixed(2);
}

function chartAxesSVG(mn, mx, nYTicks=3) {
  const range = mx - mn || 1;
  let axes = '';
  // Y axis line
  axes += `<line x1="${CHART_PL}" y1="${CHART_PT}" x2="${CHART_PL}" y2="${CHART_PT+CPH}" stroke="#d1d5db" stroke-width="0.5"/>`;
  // X axis line
  axes += `<line x1="${CHART_PL}" y1="${CHART_PT+CPH}" x2="${CHART_PL+CPW}" y2="${CHART_PT+CPH}" stroke="#d1d5db" stroke-width="0.5"/>`;
  // Y tick labels
  for(let t=0; t<=nYTicks; t++) {
    const v = mn + (range * t / nYTicks);
    const y = CHART_PT + CPH - (t/nYTicks)*CPH;
    axes += `<line x1="${CHART_PL-3}" y1="${y}" x2="${CHART_PL+CPW}" y2="${y}" stroke="#e5e7eb" stroke-width="0.5"/>`;
    axes += `<text x="${CHART_PL-5}" y="${y}" text-anchor="end" dominant-baseline="central" font-size="9" fill="#9ca3af">${fmtAxis(v)}</text>`;
  }
  return axes;
}

function sparkSVG(data, color, mode, label, unit) {
  const pts = data.filter(v=>v!=null);
  if (pts.length < 2) return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}"></svg>`;
  const mn=Math.min(...pts), mx=Math.max(...pts);
  const range=mx-mn||1;
  const validIdxs = data.map((v,i)=>v!=null?i:-1).filter(i=>i>=0);
  const xsFn = i => CHART_PL + (i/(data.length-1))*CPW;
  const ysFn = v => CHART_PT + CPH - ((v-mn)/range)*CPH;
  const encoded = JSON.stringify(data.map((v,i)=>v!=null?{x:parseFloat(xsFn(i).toFixed(1)),y:parseFloat(ysFn(v).toFixed(1)),v:v,label:label||'',unit:unit||''}:null).filter(Boolean));
  const axes = chartAxesSVG(mn, mx);
  if(mode==='dots') {
    const circles = data.map((v,i)=>v!=null?
      `<circle cx="${xsFn(i).toFixed(1)}" cy="${ysFn(v).toFixed(1)}" r="3" fill="${color}" opacity="0.85"/>`:''
    ).join('');
    return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}" preserveAspectRatio="none" data-pts='${encoded}'>
      ${axes}${circles}
      <line class="spark-cursor" x1="-10" y1="${CHART_PT}" x2="-10" y2="${CHART_PT+CPH}" stroke="${color}" stroke-width="0.8" opacity="0" style="pointer-events:none"/>
      <circle class="spark-dot" cx="-10" cy="-10" r="4" fill="${color}" stroke="#fff" stroke-width="1.5" opacity="0" style="pointer-events:none"/>
    </svg>`;
  }
  let path='', first=true;
  data.forEach((v,i)=>{
    if(v==null){first=true;return;}
    path+=(first?'M':'L')+xsFn(i).toFixed(1)+','+ysFn(v).toFixed(1)+' ';
    first=false;
  });
  return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}" preserveAspectRatio="none" data-pts='${encoded}'>
    ${axes}
    <path d="${path.trim()}" fill="none" stroke="${color}" stroke-width="1.5" stroke-linejoin="round"/>
    <line class="spark-cursor" x1="-10" y1="${CHART_PT}" x2="-10" y2="${CHART_PT+CPH}" stroke="${color}" stroke-width="0.8" opacity="0" style="pointer-events:none"/>
    <circle class="spark-dot" cx="-10" cy="-10" r="4" fill="${color}" stroke="#fff" stroke-width="1.5" opacity="0" style="pointer-events:none"/>
  </svg>`;
}

const SPARK_COLORS=['#3b82f6','#10b981','#f59e0b','#8b5cf6','#ef4444'];

function setAxis(port, axis, val) {
  if(!axisPrefs[port]) axisPrefs[port]={};
  axisPrefs[port][axis]=val==='time'?'time':parseInt(val);
  saveLS('opensense_axes_v1', axisPrefs);
  const card=document.getElementById('dcard-'+port);
  if(card) card.dataset.rc='';
}

function clearSparkData(port) {
  if(sparkData[port]) {
    Object.keys(sparkData[port]).forEach(k => {
      sparkData[port][k] = new Array(SPARK_WIN).fill(null);
    });
  }
  const sp = document.getElementById('sp-'+port);
  if(sp) sp.innerHTML = '';
}

function setChartMode(port) {
  chartMode[port] = (chartMode[port]==='dots') ? 'line' : 'dots';
  saveLS('opensense_chartmode_v1', chartMode);
  /* Re-render chart immediately without waiting for next telemetry push */
  const sp = document.getElementById('sp-'+port);
  if(!sp) return;
  const ax = axisPrefs[port]||{};
  const xAx = ax.x??'time';
  const yAx = ax.y??0;
  const mode = chartMode[port];
  const btn = document.getElementById('cmbtn-'+port);
  if(btn) btn.textContent = mode==='dots'?'Scatter':'Line';
  let svgs='';
  if(xAx==='time') {
    const portData = sparkData[port]||{};
    const numRegs = Object.keys(portData).length;
    svgs = Array.from({length:numRegs},(_,i)=>sparkSVG(portData[i]||[],SPARK_COLORS[i%SPARK_COLORS.length],mode)).join(''); /* label/unit encoded in data-pts already */
  } else {
    const xD=sparkData[port]?.[xAx]||[];
    const yD=sparkData[port]?.[yAx]||[];
    svgs=scatterSVG(xD,yD,SPARK_COLORS[xAx%SPARK_COLORS.length],SPARK_COLORS[yAx%SPARK_COLORS.length],mode);
  }
  sp.innerHTML=svgs;
  attachSparkHover(sp);
}

function getSparkTip() {
  let tip = document.getElementById('spark-tip');
  if(!tip) {
    tip = document.createElement('div');
    tip.id = 'spark-tip';
    tip.style.cssText = 'position:fixed;background:var(--white);border:1px solid var(--g200);padding:4px 10px;border-radius:6px;font-size:12px;pointer-events:none;display:none;z-index:9999;color:var(--g900);box-shadow:0 2px 8px rgba(0,0,0,0.12);white-space:nowrap';
    document.body.appendChild(tip);
  }
  return tip;
}

function attachSparkHover(container) {
  const tip = getSparkTip();
  container.querySelectorAll('svg.sparkline[data-pts]').forEach(svg => {
    svg.style.cursor = 'crosshair';
    svg.addEventListener('mousemove', e => {
      let pts; try { pts=JSON.parse(svg.dataset.pts||'[]'); } catch(e){return;}
      if(!pts.length) return;
      const rect = svg.getBoundingClientRect();
      const mx = (e.clientX - rect.left) / rect.width * 260;
      const nearest = pts.reduce((a,b)=>Math.abs(b.x-mx)<Math.abs(a.x-mx)?b:a);
      const cursor = svg.querySelector('.spark-cursor');
      const dot = svg.querySelector('.spark-dot');
      if(cursor){cursor.setAttribute('x1',nearest.x);cursor.setAttribute('x2',nearest.x);cursor.setAttribute('opacity','0.4');}
      if(dot){dot.setAttribute('cx',nearest.x);dot.setAttribute('cy',nearest.y);dot.setAttribute('opacity','1');}
      let tipTxt = (nearest.label ? nearest.label+': ' : '') + fmtNum(nearest.v) + (nearest.unit ? ' '+nearest.unit : '');
      if(nearest.vx !== undefined) tipTxt += '  (x: '+fmtNum(nearest.vx)+')';
      tip.textContent = tipTxt;
      tip.style.left = (e.clientX+10)+'px';
      tip.style.top  = (e.clientY-28)+'px';
      tip.style.display = 'block';
    });
    svg.addEventListener('mouseleave', () => {
      const cursor=svg.querySelector('.spark-cursor');
      const dot=svg.querySelector('.spark-dot');
      if(cursor) cursor.setAttribute('opacity','0');
      if(dot) dot.setAttribute('opacity','0');
      tip.style.display='none';
    });
  });
}

function scatterSVG(xData, yData, xColor, yColor, mode) {
  const pairs=xData.map((x,i)=>[x,yData[i]]).filter(([x,y])=>x!=null&&y!=null);
  if(pairs.length<2) return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}"></svg>`;
  const xs=pairs.map(p=>p[0]),ys=pairs.map(p=>p[1]);
  const mnx=Math.min(...xs),mxx=Math.max(...xs),rngx=mxx-mnx||1;
  const mny=Math.min(...ys),mxy=Math.max(...ys),rngy=mxy-mny||1;
  const px=v=>CHART_PL+(v-mnx)/rngx*CPW;
  const py=v=>CHART_PT+CPH-(v-mny)/rngy*CPH;
  const encoded=JSON.stringify(pairs.map(([x,y])=>({x:parseFloat(px(x).toFixed(1)),y:parseFloat(py(y).toFixed(1)),v:y,vx:x})));
  const axes = chartAxesSVG(mny, mxy);
  // X axis labels
  let xLabels = `<text x="${CHART_PL}" y="${CHART_PT+CPH+14}" text-anchor="middle" font-size="9" fill="#9ca3af">${fmtAxis(mnx)}</text>`;
  xLabels += `<text x="${CHART_PL+CPW}" y="${CHART_PT+CPH+14}" text-anchor="middle" font-size="9" fill="#9ca3af">${fmtAxis(mxx)}</text>`;
  if(mode==='line') {
    let path='',first=true;
    pairs.forEach(([x,y])=>{path+=(first?'M':'L')+px(x).toFixed(1)+','+py(y).toFixed(1)+' ';first=false;});
    return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}" preserveAspectRatio="none" data-pts='${encoded}'>
      ${axes}${xLabels}
      <path d="${path.trim()}" fill="none" stroke="${yColor}" stroke-width="1.5" stroke-linejoin="round"/>
      <line class="spark-cursor" x1="-10" y1="${CHART_PT}" x2="-10" y2="${CHART_PT+CPH}" stroke="${yColor}" stroke-width="0.8" opacity="0" style="pointer-events:none"/>
      <circle class="spark-dot" cx="-10" cy="-10" r="4" fill="${yColor}" stroke="#fff" stroke-width="1.5" opacity="0" style="pointer-events:none"/>
    </svg>`;
  }
  // Scatter dots
  const dots = pairs.map(([x,y])=>
    `<circle cx="${px(x).toFixed(1)}" cy="${py(y).toFixed(1)}" r="3" fill="${yColor}" opacity="0.85"/>`
  ).join('');
  return `<svg class="sparkline" viewBox="0 0 ${CHART_W} ${CHART_H}" preserveAspectRatio="none" data-pts='${encoded}'>
    ${axes}${xLabels}${dots}
    <line class="spark-cursor" x1="-10" y1="${CHART_PT}" x2="-10" y2="${CHART_PT+CPH}" stroke="${yColor}" stroke-width="0.8" opacity="0" style="pointer-events:none"/>
    <circle class="spark-dot" cx="-10" cy="-10" r="4" fill="${yColor}" stroke="#fff" stroke-width="1.5" opacity="0" style="pointer-events:none"/>
  </svg>`;
}

// ── Axis picker HTML helper ───────────────────────────────────────────────
function axisPickerHtml(port, readings) {
  if(readings.length < 2) return '';
  const ax = axisPrefs[port] || {};
  const xv = ax.x ?? 'time';
  const yv = ax.y ?? 0;
  const xOpts = '<option value="time"'+(xv==='time'?' selected':'')+'>Time</option>'+
    readings.map((r,i)=>'<option value="'+i+'"'+(xv===i?' selected':'')+'>'+(r.label||'Reg '+i)+'</option>').join('');
  const yOpts = readings.map((r,i)=>'<option value="'+i+'"'+(yv===i?' selected':'')+'>'+(r.label||'Reg '+i)+'</option>').join('');
  return '<div class="axis-picker">'+
    '<span class="axis-lbl">X</span>'+
    '<select class="axis-sel" onchange="setAxis('+port+',\'x\',this.value)">'+xOpts+'</select>'+
    '<span class="axis-lbl">Y</span>'+
    '<select class="axis-sel" onchange="setAxis('+port+',\'y\',this.value)">'+yOpts+'</select>'+
    '</div>';
}

// ── Dashboard ──────────────────────────────────────────────────────────────
function updateDashboard(sensors) {
  const grid=document.getElementById('sensor-grid');

  sensors.forEach(s => {
    const readings=s.readings||[];
    const valid=s.valid&&s.enabled;

    if(!sparkData[s.port]) sparkData[s.port]={};
    readings.forEach((_,i)=>{
      if(!sparkData[s.port][i]) sparkData[s.port][i]=new Array(SPARK_WIN).fill(null);
      sparkData[s.port][i].push(valid&&readings[i]!=null?readings[i].value:null);
      if(sparkData[s.port][i].length>SPARK_WIN) sparkData[s.port][i].shift();
    });

    let card=document.getElementById('dcard-'+s.port);
    const rebuild=!card||card.dataset.rc!==String(readings.length)||card.dataset.en!==String(s.enabled);

    if(rebuild) {
      if(!card){card=document.createElement('div');card.id='dcard-'+s.port;grid.appendChild(card);}
      card.dataset.rc=readings.length; card.dataset.en=s.enabled;
      card.className='sensor-card';

      const voltBadge='';

      let body='';
      if(s.enabled&&readings.length>0) {
        readings.forEach((r,i)=>{
          body+=`<div class="reading-row">
            <span class="reading-label">${esc(r.label||'Value '+i)}</span>
            <span><span class="reading-value" id="rv-${s.port}-${i}">—</span>
            <span class="reading-unit">${esc(r.unit||'')}</span></span>
          </div>`;
        });
      } else {
        body=`<div class="sc-disabled">${s.enabled?'Sensor not responding':'Port disabled'}</div>`;
      }

      card.innerHTML=`
        <div class="sc-header">
          <div>
            <div class="sc-name">${esc(s.name||'Port '+(s.port+1))}</div>
            <div class="sc-meta">
              <span class="sdot ${valid?'sdot-g':s.enabled?'sdot sdot-r':''}"></span>
              ${s.enabled?(valid?'Reading OK':'Read error'):'Disabled'} · CH${s.port+1}
            </div>
          </div>
          ${voltBadge}
        </div>
        <div id="rb-${s.port}">${body}</div>${s.enabled&&readings.length>0?'<div style="display:flex;justify-content:flex-end;gap:6px;margin-bottom:4px"><button class="btn secondary sm" onclick="clearSparkData('+s.port+')" title="Reset chart">Reset</button><button class="btn secondary sm" id="cmbtn-'+s.port+'" onclick="setChartMode('+s.port+')" title="Toggle chart type">Line</button></div><div id="sp-'+s.port+'"></div>'+axisPickerHtml(s.port,readings):''}
        `;
    }

    readings.forEach((r,i)=>{
      const el=document.getElementById(`rv-${s.port}-${i}`);
      if(el){const v=valid?fmtNum(r.value):'—';if(el.textContent!==v)el.textContent=v;}
    });
    if(s.enabled&&readings.length>0) {
      const sp=document.getElementById('sp-'+s.port);
      if(sp) {
        const ax=axisPrefs[s.port]||{};
        const xAx=ax.x??'time';
        const yAx=ax.y??0;
        let svgs='';
        const mode=chartMode[s.port]||'line';
        const btn=document.getElementById('cmbtn-'+s.port);
        if(btn) btn.textContent=mode==='dots'?'Scatter':'Line';
        if(xAx==='time') {
          svgs=readings.map((r,i)=>sparkSVG(sparkData[s.port]?.[i]||[],SPARK_COLORS[i%SPARK_COLORS.length],mode,r.label||'',r.unit||'')).join('');
        } else {
          const xD=sparkData[s.port]?.[xAx]||[];
          const yD=sparkData[s.port]?.[yAx]||[];
          svgs=scatterSVG(xD,yD,SPARK_COLORS[xAx%SPARK_COLORS.length],SPARK_COLORS[yAx%SPARK_COLORS.length],mode);
        }
        sp.innerHTML=svgs;
        attachSparkHover(sp);
      }
    }
  });
}

function fmtNum(v) {
  if(typeof v!=='number')return'—';
  const a=Math.abs(v);
  if(a>=10000)return v.toFixed(0);
  if(a>=1000) return v.toFixed(1);
  if(a>=100)  return v.toFixed(2);
  if(a>=10)   return v.toFixed(3);
  return v.toFixed(4).replace(/\.?0+$/,'');
}

// ── Fetch device configs ───────────────────────────────────────────────────
function fetchDeviceCfgs() {
  fetch('/api/config').then(r=>r.json()).then(data=>{
    portRawCfgs={};
    (data.sensors||[]).forEach(s=>{portRawCfgs[s.port]=s;});
    if(document.getElementById('page-ports').classList.contains('active')) renderPortsPage();
  }).catch(_=>{});
}

// ── Ports page ─────────────────────────────────────────────────────────────
function renderPortsPage() {
  const grid=document.getElementById('ports-grid');
  grid.innerHTML='';
  for(let p=0;p<MAX_PORTS;p++) {
    const meta=PORT_META[p];
    const cfgId=portAssigns[String(p)]||null;
    const cfg=savedCfgs.find(c=>c.id===cfgId)||null;
    const live=liveData[p]||null;

    let assignHtml;
    if(cfg) {
      const status=live&&live.valid?`<span class="badge green">Live</span>`
        :(live&&live.enabled?`<span class="badge red">Error</span>`:'');
      assignHtml=`
        <div class="assigned-pill">
          <div>
            <div class="ao-name">${esc(cfg.name)}</div>
            <div class="ao-sub">I2C 0x${(cfg.i2c_addr||0).toString(16).toUpperCase().padStart(2,'0')} · ${cfg.num_regs||0} reg${(cfg.num_regs||0)!==1?'s':''}</div>
          </div>
          ${status}
        </div>
        <div style="display:flex;gap:6px;margin-top:8px">
          <button class="btn sm secondary" onclick="openAssignModal(${p})">Change</button>
          <button class="btn sm danger" onclick="unassignPort(${p})">Remove</button>
        </div>`;
    } else {
      assignHtml=`
        <div class="sc-disabled">No config assigned</div>
        <button class="btn sm primary mt" onclick="openAssignModal(${p})">Assign Config</button>`;
    }

    const el=document.createElement('div');
    el.className='port-card';
    el.innerHTML=`
      <div class="port-title">${meta.label}</div>
      <div class="port-sub">${meta.sub}</div>
      <div class="assign-section">
        <div class="assign-label">Assigned Configuration</div>
        <div style="margin-top:6px">${assignHtml}</div>
      </div>`;
    grid.appendChild(el);
  }
}

function openAssignModal(port) {
  assigningPort=port;
  document.getElementById('assign-modal-title').textContent='Assign Config to '+PORT_META[port].label;
  let body='';
  if(savedCfgs.length===0) {
    body='<div class="empty"><div class="empty-title">No saved configurations</div><div class="empty-sub">Create one on the Configs tab first</div></div>';
  } else {
    const cur=portAssigns[String(port)]||null;
    const portAllowsAdc=PORT_META[port].adc;
    const visibleCfgs=savedCfgs.filter(c=>{
      const st=c.sensor_type??SENSOR_TYPE_I2C;
      if(st===SENSOR_TYPE_ADC) return portAllowsAdc;
      return true;
    });
    if(visibleCfgs.length===0){
      body=`<div class="empty"><div class="empty-title">No compatible configurations</div><div class="empty-sub">This port only supports I2C configs. Create one on the Configs tab.</div></div>`;
    } else
    body=`<div>${visibleCfgs.map(c=>`
      <div class="assign-option ${c.id===cur?'selected':''}" data-id="${c.id}" onclick="selAssign(this)">
        <div><div class="ao-name">${esc(c.name)}</div>
        <div class="ao-sub">I2C 0x${(c.i2c_addr||0).toString(16).toUpperCase().padStart(2,'0')} · ${c.num_regs||0} reg${(c.num_regs||0)!==1?'s':''}</div></div>
        ${c.id===cur?'<span class="badge blue">Current</span>':''}
      </div>`).join('')}</div>`;

  }
  document.getElementById('assign-modal-body').innerHTML=body;
  document.getElementById('assign-modal').classList.add('open');
}
function selAssign(el){document.querySelectorAll('.assign-option').forEach(e=>e.classList.remove('selected'));el.classList.add('selected');}
function closeAssignModal(){document.getElementById('assign-modal').classList.remove('open');assigningPort=null;}
function confirmAssign() {
  if(assigningPort===null)return;
  const sel=document.querySelector('.assign-option.selected');
  if(!sel){closeAssignModal();return;}
  const cfg=savedCfgs.find(c=>c.id===sel.dataset.id);
  if(!cfg)return;
  const payload={
    port:assigningPort, enabled:true,
    sensor_type: cfg.sensor_type ?? SENSOR_TYPE_I2C,
    i2c_addr:cfg.i2c_addr||0,
    voltage_5v:false,
    name:cfg.name, num_regs:cfg.num_regs||0, regs:cfg.regs||[],
  };
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(r=>r.json()).then(d=>{
      if(d.ok){portAssigns[String(assigningPort)]=cfg.id;portRawCfgs[assigningPort]=payload;saveLS(LS_PORTS_KEY,portAssigns);closeAssignModal();renderPortsPage();}
    }).catch(_=>closeAssignModal());
}
function unassignPort(port) {
  if(!confirm('Remove config from Port '+(port+1)+'?'))return;
  const payload={port,enabled:false,i2c_addr:0,voltage_5v:false,name:'',num_regs:0,regs:[]};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(()=>{delete portAssigns[String(port)];portRawCfgs[port]=payload;saveLS(LS_PORTS_KEY,portAssigns);renderPortsPage();});
}

// ── Saved configs page ─────────────────────────────────────────────────────
function renderSavedCfgsPage() {
  const list=document.getElementById('cfgs-list');
  const empty=document.getElementById('cfgs-empty');
  if(savedCfgs.length===0){list.innerHTML='';empty.style.display='block';return;}
  empty.style.display='none';
  list.innerHTML=savedCfgs.map(c=>{
    const used=Object.entries(portAssigns).filter(([,id])=>id===c.id).map(([p])=>'Port '+(parseInt(p)+1)).join(', ');
    const regsSnippet=(c.regs||[]).slice(0,c.num_regs||0).map(r=>r.label||'Reg').join(', ');
    return `<div class="cfg-row">
      <div>
        <div class="cfg-name">${esc(c.name)}</div>
        <div class="cfg-meta">${(c.sensor_type??SENSOR_TYPE_I2C)===SENSOR_TYPE_ADC?'<span class="badge amber">ADC</span>':'<span class="badge blue">I2C</span>'} 0x${(c.i2c_addr||0).toString(16).toUpperCase().padStart(2,'0')} · ${c.num_regs||0} reg${(c.num_regs||0)!==1?'s':''}${regsSnippet?' · '+esc(regsSnippet):''}${used?` · <span class="badge green">${esc(used)}</span>`:''}</div>
      </div>
      <div class="cfg-actions">
        <button class="btn sm secondary" onclick="openEditModal('${c.id}')">Edit</button>
        <button class="btn sm danger" onclick="deleteCfg('${c.id}')">Delete</button>
      </div>
    </div>`;
  }).join('');
}
function deleteCfg(id) {
  if(!confirm('Delete this configuration?'))return;
  savedCfgs=savedCfgs.filter(c=>c.id!==id);
  saveLS(LS_CFGS_KEY,savedCfgs);
  Object.keys(portAssigns).forEach(p=>{if(portAssigns[p]===id)delete portAssigns[p];});
  saveLS(LS_PORTS_KEY,portAssigns);
  renderSavedCfgsPage();
}

// ── Config modal ───────────────────────────────────────────────────────────
function openNewCfgModal(){editingCfgId=null;document.getElementById('cfg-modal-title').textContent='New Configuration';buildModalForm(null);document.getElementById('cfg-modal').classList.add('open');}
function openEditModal(id){editingCfgId=id;const cfg=savedCfgs.find(c=>c.id===id);document.getElementById('cfg-modal-title').textContent='Edit: '+(cfg?.name||'');buildModalForm(cfg||null);document.getElementById('cfg-modal').classList.add('open');}
function closeCfgModal(){document.getElementById('cfg-modal').classList.remove('open');editingCfgId=null;}

const MCP3221_REG = {
  reg_addr:          0x00,
  byte_len:          2,
  big_endian:        true,
  is_signed:         false,
  direct_read:       true,
  write_before_read: false,
  write_len:         0,
  write_buf:         [0,0,0,0],
  delay_us:          0,
  data_shift:        0,
  data_mask:         0x0FFF,
  valid_bits:        12,
};

function buildModalForm(cfg) {
  const name  = cfg?.name||'';
  const addr  = '0x'+((cfg?.i2c_addr||0x76)).toString(16).toUpperCase().padStart(2,'0');
  const nregs = cfg?.num_regs||1;
  const stype = cfg?.sensor_type ?? SENSOR_TYPE_I2C;

  document.getElementById('cfg-modal-body').innerHTML=`
    <div class="section-div">Sensor</div>
    <div class="g2" style="margin-bottom:12px">
      <div class="fg">
        <label class="fl">Config Name</label>
        <input class="fc" id="m-name" value="${esc(name)}" placeholder="e.g. LMT87 Temperature">
      </div>
      <div class="fg">
        <label class="fl">Sensor Type</label>
        <select class="fc" id="m-stype" onchange="onSensorTypeChange()">
          <option value="${SENSOR_TYPE_I2C}" ${stype===SENSOR_TYPE_I2C?'selected':''}>I2C Sensor</option>
          <option value="${SENSOR_TYPE_ADC}" ${stype===SENSOR_TYPE_ADC?'selected':''}>ADC Sensor (Ports 4 &amp; 5 only)</option>
          <option value="${SENSOR_TYPE_SHT30}" ${stype===SENSOR_TYPE_SHT30?'selected':''}>SHT30 — Temperature + Humidity</option>
          <option value="${SENSOR_TYPE_BME280}" ${stype===SENSOR_TYPE_BME280?'selected':''}>BME280 — Temp + Pressure + Humidity</option>
          <option value="${SENSOR_TYPE_HIH6130}" ${stype===SENSOR_TYPE_HIH6130?'selected':''}>HIH6130 — Humidity + Temperature</option>
        </select>
      </div>
    </div>
    <div id="m-addr-row" style="margin-bottom:14px;${(stype===SENSOR_TYPE_ADC||stype===SENSOR_TYPE_HIH6130)?'display:none':''}">
      <div class="fg" style="max-width:260px">
        <label class="fl">I2C Address</label>
        <div id="m-addr-input-wrap"></div>
      </div>
    </div>
    <div id="m-regs-section">
      <div class="section-div" id="m-regs-header" style="display:flex;justify-content:space-between;align-items:center">
        <span>Registers</span>
        <div id="m-nregs-ctrl" style="display:${stype===SENSOR_TYPE_ADC?'none':'flex'};align-items:center;gap:8px">
          <span style="font-size:.75rem;color:var(--g500);font-weight:400">Count:</span>
          <select class="fc" id="m-nregs" style="width:55px;padding:3px 6px" onchange="rebuildRegs()">
            ${[0,1,2,3,4].map(n=>`<option value="${n}" ${nregs===n?'selected':''}>${n}</option>`).join('')}
          </select>
        </div>
      </div>
      <div id="m-regs" style="margin-top:10px"></div>
    </div>`;

  if(stype === SENSOR_TYPE_ADC) {
    renderMcp3221Form(cfg);
  } else if(stype === SENSOR_TYPE_SHT30 || stype === SENSOR_TYPE_BME280 || stype === SENSOR_TYPE_HIH6130) {
    const nregsCtrl = document.getElementById('m-nregs-ctrl');
    if(nregsCtrl) nregsCtrl.style.display = 'none';
    renderManagedSensorInfo(stype);
  } else {
    /* Plain I2C — populate address input directly */
    const wrap = document.getElementById('m-addr-input-wrap');
    if(wrap) wrap.innerHTML = `<input class="fc" id="m-addr" value="${addr}" placeholder="0x76">`;
    renderI2CRegs(cfg, nregs);
  }
}

function onSensorTypeChange() {
  const stype = parseInt(document.getElementById('m-stype').value);
  const isAdc    = (stype === SENSOR_TYPE_ADC);
  const isManaged = (stype === SENSOR_TYPE_SHT30 || stype === SENSOR_TYPE_BME280 || stype === SENSOR_TYPE_HIH6130);
  const addrRow   = document.getElementById('m-addr-row');
  const nregsCtrl = document.getElementById('m-nregs-ctrl');
  if(addrRow)   addrRow.style.display   = (isAdc || stype===SENSOR_TYPE_HIH6130) ? 'none' : 'block';
  if(nregsCtrl) nregsCtrl.style.display = (isAdc || isManaged) ? 'none' : 'flex';
  /* For plain I2C sensor populate the addr wrap with a text input */
  const wrap = document.getElementById('m-addr-input-wrap');
  if(wrap && !isAdc && !isManaged) {
    const cur = wrap.querySelector('input')?.value || '0x76';
    wrap.innerHTML = `<input class="fc" id="m-addr" value="${cur}" placeholder="0x76">`;
  }
  if(isAdc) {
    renderMcp3221Form(null);
  } else if(isManaged) {
    renderManagedSensorInfo(stype);
  } else {
    const n = parseInt(document.getElementById('m-nregs').value||'1');
    renderI2CRegs(null, n);
  }
}

function renderMcp3221Form(cfg) {
  const r = cfg?.regs?.[0] || {};
  document.getElementById('m-regs').innerHTML=`
    <div class="reg-block">
      <div class="reg-block-head">ADC Sensor</div>
      <p style="font-size:.72rem;color:var(--g400);margin-bottom:10px">
        Raw ADC value x = 0–4095 (12-bit, 0–3.3V). Scale/offset or expression applied to x.
      </p>
      <div class="g2" style="margin-bottom:10px">
        <div class="fg">
          <label class="fl">Label</label>
          <input class="fc" id="mr0_lbl" value="${esc(r.label||'')}" placeholder="e.g. Temperature">
        </div>
        <div class="fg">
          <label class="fl">Unit</label>
          <input class="fc" id="mr0_unit" value="${esc(r.unit||'')}" placeholder="e.g. C">
        </div>
      </div>
      <div class="g2" style="margin-bottom:10px">
        <div class="fg">
          <label class="fl">Scale x</label>
          <input type="number" step="any" class="fc" id="mr0_scale" value="${r.scale??1}">
        </div>
        <div class="fg">
          <label class="fl">Offset +</label>
          <input type="number" step="any" class="fc" id="mr0_offset" value="${r.offset??0}">
        </div>
      </div>
      <div class="fg" style="margin-bottom:4px">
        <label class="fl" style="display:flex;justify-content:space-between;align-items:center">
          <span>Expression <span style="font-size:.68rem;color:var(--g400);font-weight:400">(overrides scale &amp; offset — use <b>x</b> for raw ADC 0–4095)</span></span>
          <button type="button" class="btn sm secondary" style="padding:1px 7px;font-size:.68rem" onclick="toggleExprRef('adc')">symbols ▾</button>
        </label>
        <input class="fc" id="mr0_expr" value="${esc(r.expr||'')}" placeholder="e.g. 60*x/(4095-x)" style="font-family:monospace;font-size:.8rem">
        <div id="mradc_exprref" style="display:none;margin-top:6px;padding:8px;background:var(--g50);border:1px solid var(--g100);border-radius:var(--r);font-size:.72rem">
          <div style="color:var(--g500);font-weight:600;margin-bottom:6px">Available symbols — click to insert at cursor</div>
          <div style="display:flex;flex-wrap:wrap;gap:4px">
            ${['x','pi','e','+','-','*','/','^','(',')',
               'sqrt()','abs()','sin()','cos()','tan()',
               'asin()','acos()','atan()',
               'log()','log10()','exp()',
               'floor()','ceil()','round()'
            ].map(sym=>`<button type="button" class="expr-sym" onclick="insertAdcExprSym('${sym}')">${sym}</button>`).join('')}
          </div>
          <div style="margin-top:8px;color:var(--g400);line-height:1.6">
            <b>x</b> = raw ADC count (0–4095) &nbsp;·&nbsp; 0V = 0, 3.3V = 4095<br>
            Example FSR with 10k pull-down: <code>60*x/(4095-x)</code> → force in grams<br>
            Example voltage: <code>x*3.3/4095</code> → volts
          </div>
        </div>
      </div>
    </div>`;
}

function toggleExprRef(i) {
  const id = i === 'adc' ? 'mradc_exprref' : `mr${i}_exprref`;
  const el = document.getElementById(id);
  if(el) el.style.display = el.style.display==='none' ? '' : 'none';
}

function insertAdcExprSym(sym) {
  const el = document.getElementById('mr0_expr');
  if(!el) return;
  const start = el.selectionStart ?? el.value.length;
  const end   = el.selectionEnd   ?? el.value.length;
  const ins = sym.endsWith('()') ? sym.slice(0,-1) : sym;
  el.value = el.value.slice(0,start) + ins + el.value.slice(end);
  const pos = start + ins.length;
  el.focus();
  el.setSelectionRange(pos, pos);
}

const MANAGED_SENSOR_INFO = {
  [SENSOR_TYPE_SHT30]: {
    title: 'SHT30 — Temperature &amp; Humidity',
    desc: 'Driver handles measurement commands, 20 ms conversion wait, CRC validation, and calibration automatically.',
    outputs: ['Temperature (°C)', 'Humidity (%RH)'],
    defaultAddr: '0x44',
    addrOptions: [
      { value: '0x44', label: '0x44 — ADDR pin low (default)' },
      { value: '0x45', label: '0x45 — ADDR pin high' },
    ],
  },
  [SENSOR_TYPE_BME280]: {
    title: 'BME280 — Temperature, Pressure &amp; Humidity',
    desc: 'Driver reads factory calibration trims, configures normal mode, and applies the full Bosch compensation algorithm.',
    outputs: ['Temperature (°C)', 'Pressure (hPa)', 'Humidity (%RH)'],
    defaultAddr: '0x76',
    addrOptions: [
      { value: '0x76', label: '0x76 — SDO pin low (default)' },
      { value: '0x77', label: '0x77 — SDO pin high' },
    ],
  },
  [SENSOR_TYPE_HIH6130]: {
    title: 'HIH6130/6131 — Humidity + Temperature',
    desc: 'Driver triggers a measurement, waits 60 ms for conversion, then reads 4 bytes. Status, humidity and temperature are extracted automatically.',
    outputs: ['Humidity (%RH)', 'Temperature (°C)'],
    defaultAddr: '0x27',
    addrOptions: null,  // fixed address — no dropdown
  },
};

function renderManagedSensorInfo(stype) {
  const info = MANAGED_SENSOR_INFO[stype];
  if(!info) return;

  /* Build address control — dropdown for configurable, hidden for fixed */
  const wrap = document.getElementById('m-addr-input-wrap');
  if(wrap) {
    if(info.addrOptions) {
      const curAddr = document.getElementById('m-addr-sel')?.value || info.defaultAddr;
      wrap.innerHTML = `<select class="fc" id="m-addr-sel">` +
        info.addrOptions.map(o =>
          `<option value="${o.value}" ${curAddr===o.value?'selected':''}>${o.label}</option>`
        ).join('') + `</select>`;
    } else {
      wrap.innerHTML = '';
    }
  }

  document.getElementById('m-regs').innerHTML=`
    <div class="reg-block">
      <div class="reg-block-head">${info.title}</div>
      <p style="font-size:.75rem;color:var(--g500);margin-bottom:10px;line-height:1.5">${info.desc}</p>
      <div style="font-size:.75rem;color:var(--g500);margin-bottom:6px;font-weight:600">Outputs:</div>
      <ul style="font-size:.78rem;color:var(--g700);margin:0 0 10px 16px;padding:0">
        ${info.outputs.map(o=>`<li>${o}</li>`).join('')}
      </ul>
    </div>`;
}

function defReg(cfg,i){
  return cfg?.regs?.[i] || {
    reg_addr:0, byte_len:2, is_signed:true, big_endian:true,
    scale:1, offset:0, expr:'', label:'', unit:'',
    write_before_read:false, write_len:0, write_buf:[0,0,0,0],
    delay_us:0, data_shift:0, data_mask:0, valid_bits:0, direct_read:false, merge_with:255, merge_shift:0,
  };
}

function sel(v) { return v ? 'selected' : ''; }
function hx(n)  { return '0x'+(n||0).toString(16).toUpperCase().padStart(2,'0'); }

function renderI2CRegs(cfg, count) {
  const el = document.getElementById('m-regs');
  if(count===0){
    el.innerHTML='<p style="font-size:.8rem;color:var(--g500)">No registers — sensor will not be sampled.</p>';
    return;
  }
  let html='';
  for(let i=0; i<count; i++){
    const r  = defReg(cfg,i);
    const ah = '0x'+(r.reg_addr||0).toString(16).toUpperCase().padStart(2,'0');
    html+=`<div class="reg-block">
      <div class="reg-block-head">Register ${i}</div>
      <div class="section-div" style="font-size:.68rem;margin-bottom:8px">Read</div>
      <div class="g2" style="margin-bottom:10px">
        <div class="fg">
          <label class="fl">Register Address</label>
          <input class="fc" id="mr${i}_addr" value="${ah}">
        </div>
        <div class="fg">
          <label class="fl">Bytes to Read</label>
          <select class="fc" id="mr${i}_len">
            ${[1,2,3,4].map(n=>`<option value="${n}" ${(r.byte_len||2)==n?'selected':''}>${n}B</option>`).join('')}
          </select>
        </div>
      </div>
      <div class="g2" style="margin-bottom:12px">
        <div class="fg">
          <label class="fl">Byte Order</label>
          <select class="fc" id="mr${i}_end">
            <option value="1" ${r.big_endian?'selected':''}>Big Endian (MSB first)</option>
            <option value="0" ${!r.big_endian?'selected':''}>Little Endian (LSB first)</option>
          </select>
        </div>
        <div class="fg">
          <label class="fl">Integer Sign</label>
          <select class="fc" id="mr${i}_sign">
            <option value="1" ${r.is_signed?'selected':''}>Signed</option>
            <option value="0" ${!r.is_signed?'selected':''}>Unsigned</option>
          </select>
        </div>
      </div>
      <div class="section-div" style="font-size:.68rem;margin-bottom:8px">Scaling</div>
      <div class="g2" style="margin-bottom:10px">
        <div class="fg">
          <label class="fl">Label</label>
          <input class="fc" id="mr${i}_lbl" value="${esc(r.label||'')}" placeholder="e.g. Temperature">
        </div>
        <div class="fg">
          <label class="fl">Unit</label>
          <input class="fc" id="mr${i}_unit" value="${esc(r.unit||'')}" placeholder="e.g. C">
        </div>
      </div>
      <div class="g2" style="margin-bottom:10px">
        <div class="fg">
          <label class="fl">Scale x</label>
          <input type="number" step="any" class="fc" id="mr${i}_scale" value="${r.scale??1}">
        </div>
        <div class="fg">
          <label class="fl">Offset +</label>
          <input type="number" step="any" class="fc" id="mr${i}_offset" value="${r.offset??0}">
        </div>
      </div>
      <div class="fg" style="margin-bottom:4px">
        <label class="fl" style="display:flex;justify-content:space-between;align-items:center">
          <span>Expression <span style="font-size:.68rem;color:var(--g400);font-weight:400">(overrides scale &amp; offset when set — use <b>x</b> for raw value)</span></span>
          <button type="button" class="btn sm secondary" style="padding:1px 7px;font-size:.68rem" onclick="toggleExprRef(${i})">symbols ▾</button>
        </label>
        <input class="fc" id="mr${i}_expr" value="${esc(r.expr||'')}" placeholder="e.g. 1/(log(x/10000)/3950 + 1/298.15) - 273.15" style="font-family:monospace;font-size:.8rem">
        <div id="mr${i}_exprref" style="display:none;margin-top:6px;padding:8px;background:var(--g50);border:1px solid var(--g100);border-radius:var(--r);font-size:.72rem">
          <div style="color:var(--g500);font-weight:600;margin-bottom:6px">Available symbols — click to insert at cursor</div>
          <div style="display:flex;flex-wrap:wrap;gap:4px">
            ${['x','pi','e','+','-','*','/','^','(',')',
               'sqrt()','abs()','sin()','cos()','tan()',
               'asin()','acos()','atan()',
               'log()','log10()','exp()',
               'floor()','ceil()','round()'
            ].map(sym=>`<button type="button" class="expr-sym" onclick="insertExprSym(${i},'${sym}')">${sym}</button>`).join('')}
          </div>
          <div style="margin-top:8px;color:var(--g400);line-height:1.6">
            <b>x</b> = raw integer (after shift/mask) &nbsp;·&nbsp;
            <b>pi</b> = 3.14159… &nbsp;·&nbsp; <b>e</b> = 2.71828…<br>
            <b>^</b> = power &nbsp;·&nbsp; <b>log()</b> = natural log &nbsp;·&nbsp; <b>log10()</b> = base-10 log<br>
            Example NTC thermistor: <code>1/(log(x/10000)/3950 + 1/298.15) - 273.15</code>
          </div>
        </div>
      </div>
      <details style="margin-top:4px">
        <summary style="font-size:.68rem;color:var(--g500);cursor:pointer;user-select:none">Advanced — Write before read</summary>
        <div style="margin-top:8px">
          <div class="g2" style="margin-bottom:8px">
            <div class="fg">
              <label class="fl">Write before read</label>
              <select class="fc" id="mr${i}_wbr" onchange="toggleWbr(${i})">
                <option value="0" ${sel(!r.write_before_read)}>No</option>
                <option value="1" ${sel(r.write_before_read)}>Yes</option>
              </select>
            </div>
            <div class="fg">
              <label class="fl">Write length (bytes)</label>
              <select class="fc" id="mr${i}_wlen">
                <option value="0" ${sel((r.write_len??2)===0)}>0 — reg addr only</option>
                <option value="1" ${sel((r.write_len??2)===1)}>1</option>
                <option value="2" ${sel((r.write_len??2)===2)}>2</option>
                <option value="3" ${sel((r.write_len??2)===3)}>3</option>
                <option value="4" ${sel((r.write_len??2)===4)}>4</option>
              </select>
            </div>
          </div>
          <div id="mr${i}_wbuf_row" style="margin-bottom:8px;display:${r.write_before_read?'block':'none'}">
            <label class="fl">Write buffer (hex bytes)</label>
            <div style="display:flex;gap:6px;margin-top:4px">
              <input class="fc" id="mr${i}_wb0" value="${hx(r.write_buf&&r.write_buf[0])}" style="width:60px;text-align:center">
              <input class="fc" id="mr${i}_wb1" value="${hx(r.write_buf&&r.write_buf[1])}" style="width:60px;text-align:center">
              <input class="fc" id="mr${i}_wb2" value="${hx(r.write_buf&&r.write_buf[2])}" style="width:60px;text-align:center">
              <input class="fc" id="mr${i}_wb3" value="${hx(r.write_buf&&r.write_buf[3])}" style="width:60px;text-align:center">
            </div>
          </div>
          <div class="fg" style="margin-bottom:4px">
            <label class="fl">Delay after write (µs)</label>
            <input type="number" class="fc" id="mr${i}_dly" value="${r.delay_us||0}">
          </div>
        </div>
      </details>
      <details style="margin-top:4px">
        <summary style="font-size:.68rem;color:var(--g500);cursor:pointer;user-select:none">Advanced — Bit extraction</summary>
        <div style="margin-top:8px">
          <p style="font-size:.72rem;color:var(--g400);margin-bottom:10px;line-height:1.5">
            Use these fields when the register value needs bit manipulation before scaling.
            Processing order: raw bytes → right-shift → mask → sign-extend → scale/offset.
            x in expressions = value after shift and mask.
          </p>
          <div class="g2" style="margin-bottom:8px">
            <div class="fg">
              <label class="fl">Direct read</label>
              <select class="fc" id="mr${i}_dr">
                <option value="0" ${sel(!r.direct_read)}>No — send register address first</option>
                <option value="1" ${sel(r.direct_read)}>Yes — read bytes immediately (no register write)</option>
              </select>
            </div>
            <div class="fg">
              <label class="fl">Right shift (bits)</label>
              <input type="number" min="0" max="31" class="fc" id="mr${i}_shift" value="${r.data_shift||0}"
                placeholder="0 = no shift">
            </div>
          </div>
          <div class="g2" style="margin-bottom:8px">
            <div class="fg">
              <label class="fl">Bit mask (hex)</label>
              <input class="fc" id="mr${i}_mask" value="${r.data_mask?hx(r.data_mask):'0x00'}"
                placeholder="0x00 = use all bits">
            </div>
            <div class="fg">
              <label class="fl">Valid bits for sign extension</label>
              <input type="number" min="0" max="32" class="fc" id="mr${i}_vbits" value="${r.valid_bits||0}"
                placeholder="0 = byte_len × 8">
            </div>
          </div>
          <p style="font-size:.71rem;color:var(--g400);line-height:1.5">
            Example — INA219 voltage reg (0x02): shift=3, mask=0x1FFF, valid bits=0, scale=0.004<br>
            Example — MCP3221 ADC: direct read=Yes, mask=0x0FFF, valid bits=12
          </p>
        </div>
      </details>
      <details style="margin-top:4px">
        <summary style="font-size:.68rem;color:var(--g500);cursor:pointer;user-select:none">Advanced — Merge with another register</summary>
        <div style="margin-top:8px">
          <p style="font-size:.72rem;color:var(--g400);margin-bottom:10px;line-height:1.5">
            Combines two 1-byte registers into one 16-bit value.<br>
            Set this register as the HIGH byte: pick the LOW byte register below and set shift=8.<br>
            The low byte register will show 0 on the dashboard — only the high byte register displays the merged value.
          </p>
          <div class="g2" style="margin-bottom:8px">
            <div class="fg">
              <label class="fl">Merge with register</label>
              <select class="fc" id="mr${i}_mw">
                <option value="255" ${(r.merge_with===255||r.merge_with===undefined)?'selected':''}>None</option>
                ${[0,1,2,3].filter(n=>n!==i).map(n=>`<option value="${n}" ${r.merge_with===n?'selected':''}>${'Register '+n}</option>`).join('')}
              </select>
            </div>
            <div class="fg">
              <label class="fl">Shift this reg left (bits)</label>
              <input type="number" min="0" max="16" class="fc" id="mr${i}_ms" value="${r.merge_shift||0}"
                placeholder="8 for high byte">
            </div>
          </div>
          <p style="font-size:.71rem;color:var(--g400)">
            Example monochromator: Register 0 (high byte) → merge with Register 1, shift=8. Register 1 (low byte) → no merge.
          </p>
        </div>
      </details>
    </div>`;
  }
  el.innerHTML=html;
}

function insertExprSym(i, sym) {
  const el = document.getElementById(`mr${i}_expr`);
  if(!el) return;
  const start = el.selectionStart ?? el.value.length;
  const end   = el.selectionEnd   ?? el.value.length;
  const ins = sym.endsWith('()') ? sym.slice(0,-1) : sym;
  el.value = el.value.slice(0,start) + ins + el.value.slice(end);
  const pos = start + ins.length;
  el.focus();
  el.setSelectionRange(pos, pos);
}

function toggleWbr(i) {
  const on = document.getElementById(`mr${i}_wbr`)?.value === '1';
  const row = document.getElementById(`mr${i}_wbuf_row`);
  if(row) row.style.display = on ? '' : 'none';
}

function rebuildRegs(){
  const stype = parseInt(document.getElementById('m-stype')?.value ?? SENSOR_TYPE_I2C);
  if(stype === SENSOR_TYPE_ADC) return;
  if(stype === SENSOR_TYPE_SHT30 || stype === SENSOR_TYPE_BME280 || stype === SENSOR_TYPE_HIH6130) return;
  const n   = parseInt(document.getElementById('m-nregs').value);
  const old = captureI2CRegs();
  renderI2CRegs({regs:old}, n);
}

function captureI2CRegs() {
  const n = parseInt(document.getElementById('m-nregs')?.value||'0');
  return Array.from({length:n}, (_,i) => ({
    label:      document.getElementById(`mr${i}_lbl`)?.value?.trim()||'',
    unit:       document.getElementById(`mr${i}_unit`)?.value?.trim()||'',
    reg_addr:   parseHex(document.getElementById(`mr${i}_addr`)?.value||'0'),
    byte_len:   parseInt(document.getElementById(`mr${i}_len`)?.value||'2'),
    scale:      parseFloat(document.getElementById(`mr${i}_scale`)?.value)||1,
    offset:     parseFloat(document.getElementById(`mr${i}_offset`)?.value)||0,
    expr:       document.getElementById(`mr${i}_expr`)?.value?.trim()||'',
    is_signed:  document.getElementById(`mr${i}_sign`)?.value==='1',
    big_endian: document.getElementById(`mr${i}_end`)?.value==='1',
    write_before_read: document.getElementById(`mr${i}_wbr`)?.value==='1',
    write_len:  parseInt(document.getElementById(`mr${i}_wlen`)?.value??'1'),
    write_buf:  [0,1,2,3].map(b=>parseHex(document.getElementById(`mr${i}_wb${b}`)?.value||'0')),
    delay_us:   parseInt(document.getElementById(`mr${i}_dly`)?.value||'0'),
    direct_read: document.getElementById(`mr${i}_dr`)?.value==='1',
    data_shift:  parseInt(document.getElementById(`mr${i}_shift`)?.value||'0')||0,
    data_mask:   parseHex(document.getElementById(`mr${i}_mask`)?.value||'0x00'),
    valid_bits:  parseInt(document.getElementById(`mr${i}_vbits`)?.value||'0')||0,
    merge_with:  parseInt(document.getElementById(`mr${i}_mw`)?.value??'255'),
    merge_shift: parseInt(document.getElementById(`mr${i}_ms`)?.value||'0')||0,
  }));
}

function captureMcp3221Reg() {
  return [{
    label:  document.getElementById('mr0_lbl')?.value?.trim()||'',
    unit:   document.getElementById('mr0_unit')?.value?.trim()||'',
    scale:  parseFloat(document.getElementById('mr0_scale')?.value)||1,
    offset: parseFloat(document.getElementById('mr0_offset')?.value)||0,
    expr:   document.getElementById('mr0_expr')?.value?.trim()||'',
    ...MCP3221_REG,
  }];
}

function captureModalRegs(stype){
  return stype===SENSOR_TYPE_ADC ? captureMcp3221Reg() : captureI2CRegs();
}

const MANAGED_SENSOR_REGS = {
  [SENSOR_TYPE_SHT30]: [
    {label:'Temperature', unit:'°C',  scale:1, offset:0},
    {label:'Humidity',    unit:'%RH', scale:1, offset:0},
  ],
  [SENSOR_TYPE_BME280]: [
    {label:'Temperature', unit:'°C',  scale:1, offset:0},
    {label:'Pressure',    unit:'hPa', scale:1, offset:0},
    {label:'Humidity',    unit:'%RH', scale:1, offset:0},
  ],
  [SENSOR_TYPE_HIH6130]: [
    {label:'Humidity',    unit:'%RH', scale:1, offset:0},
    {label:'Temperature', unit:'°C',  scale:1, offset:0},
  ],
};

function saveCfgModal(){
  const name  = document.getElementById('m-name')?.value?.trim();
  if(!name){ alert('Please enter a name.'); return; }
  const stype = parseInt(document.getElementById('m-stype')?.value ?? SENSOR_TYPE_I2C);
  const isAdc    = (stype === SENSOR_TYPE_ADC);
  const isManaged = (stype === SENSOR_TYPE_SHT30 || stype === SENSOR_TYPE_BME280 || stype === SENSOR_TYPE_HIH6130);
  const regs = isAdc ? captureMcp3221Reg()
             : isManaged ? MANAGED_SENSOR_REGS[stype]
             : captureI2CRegs();
  const entry = {
    id:          editingCfgId||genId(),
    name,
    sensor_type: stype,
    i2c_addr:    isAdc ? 0x4D
               : stype===SENSOR_TYPE_HIH6130 ? 0x27
               : parseHex(document.getElementById('m-addr-sel')?.value || document.getElementById('m-addr')?.value || '0x76'),
    num_regs:    isManaged ? regs.length
               : isAdc ? 1
               : parseInt(document.getElementById('m-nregs')?.value||'0'),
    regs,
  };
  if(editingCfgId){
    const i = savedCfgs.findIndex(c=>c.id===editingCfgId);
    if(i>=0) savedCfgs[i]=entry; else savedCfgs.push(entry);
  } else {
    savedCfgs.push(entry);
  }
  saveLS(LS_CFGS_KEY, savedCfgs);
  closeCfgModal();
  renderSavedCfgsPage();
}

// ── SD card ────────────────────────────────────────────────────────────────
let sdTimer=null;
function startSdPolling(){refreshSdStatus();if(sdTimer)clearInterval(sdTimer);sdTimer=setInterval(refreshSdStatus,3000);}
function refreshSdStatus(){fetch('/api/sdcard').then(r=>r.json()).then(renderSdStatus).catch(_=>renderSdStatus(null));}
async function eraseSD(){
  if(!confirm('Erase ALL files and reset log count?\n\nThis cannot be undone.'))return;
  try{
    const d=await(await fetch('/api/sdcard/erase',{method:'POST'})).json();
    if(d.ok){alert('SD erased. Logging restarted at log_0001.csv');refreshSdStatus();}
    else alert('Erase failed: '+(d.error||'?'));
  }catch(e){alert('Request failed: '+e);}
}
function renderSdStatus(d){
  const dot=document.getElementById('sd-dot'), lbl=document.getElementById('sd-label');
  const mount=document.getElementById('sd-mount-status'), info=document.getElementById('sd-log-info');
  const dl=document.getElementById('sd-download-btn'), er=document.getElementById('sd-erase-btn');
  const pw=document.getElementById('sd-progress-wrap');
  if(!d||!d.mounted){
    dot.className='dot'; lbl.textContent='SD: No Card';
    mount.innerHTML='<span class="badge gray">No Card</span>';
    info.style.display=dl.style.display=pw.style.display='none';
    if(er)er.style.display='none'; return;
  }
  if(!d.logging){
    dot.className='dot amber'; lbl.textContent='SD: Idle';
    mount.innerHTML=`<span class="badge amber">Mounted · Not Logging</span>`;
    info.style.display=dl.style.display=pw.style.display='none';
    if(er)er.style.display='none'; return;
  }
  dot.className='dot green'; lbl.textContent='SD: Logging';
  mount.innerHTML=`<span class="badge green"><span class="sdot sdot-g"></span>Logging Active</span>${d.card_size_mb?` <span style="font-size:.75rem;color:var(--g400)">${d.card_size_mb} MB</span>`:''}`;
  const fname=(d.log_path||'').split('/').pop()||'—';
  document.getElementById('sd-filename').textContent=fname;
  const rows=d.rows_written||0;
  document.getElementById('sd-rowcount').textContent=rows.toLocaleString();
  document.getElementById('sd-cardsize').textContent='~'+(rows*60/1024).toFixed(1)+' KB';
  info.style.display='block'; dl.style.display='inline-flex'; if(er)er.style.display='inline-flex';
  const pct=Math.min(rows/10000*100,100).toFixed(1);
  document.getElementById('sd-progress-label').textContent=rows.toLocaleString()+' rows';
  document.getElementById('sd-progress-bar').style.width=pct+'%';
  pw.style.display='block';
}

// ── Utils ──────────────────────────────────────────────────────────────────
function parseHex(s){return parseInt(String(s||'0').trim().replace(/^0[xX]/,''),16)||0;}
function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');}
function flashAlert(id,msg,ok){const el=document.getElementById(id);if(!el)return;el.textContent=msg;el.className='alert '+(ok?'ok':'err');el.classList.add('show');setTimeout(()=>el.classList.remove('show'),5000);}
['cfg-modal','assign-modal'].forEach(id=>document.getElementById(id)?.addEventListener('click',e=>{if(e.target===e.currentTarget)id==='cfg-modal'?closeCfgModal():closeAssignModal();}));

// ── Boot ───────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded',()=>{renderSavedCfgsPage();connectWS();startSdPolling();});
