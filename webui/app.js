(function(){'use strict';
const $=id=>document.getElementById(id),$$=(s,r=document)=>Array.from(r.querySelectorAll(s));
const toastHost=$('toastHost');
function toast(m,k='info'){if(!toastHost)return console.log(`[${k}]`,m);const e=document.createElement('div');e.className='toast '+k;e.textContent=m;toastHost.appendChild(e);setTimeout(()=>{e.style.opacity='0'},3500);setTimeout(()=>e.remove(),3900)}
const log={info:m=>toast(m,'info'),ok:m=>toast(m,'ok'),warn:m=>toast(m,'warn'),error:m=>toast(m,'error')};
const uid=()=>Math.random().toString(16).slice(2,12);
const hb=n=>n.toString(16).toUpperCase().padStart(2,'0');
const hw=n=>n.toString(16).toUpperCase().padStart(4,'0');
const fmtHex=s=>!s?'':s.replace(/\s/g,'').match(/.{1,2}/g)?.join(' ').toUpperCase()||'';
const h2b=s=>{const c=(s||'').replace(/\s/g,''),o=new Uint8Array(c.length/2);for(let i=0;i<o.length;i++)o[i]=parseInt(c.substr(i*2,2),16);return o};
const esc=s=>(s||'').replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]));

const appSetup=$('app-setup'),appConsole=$('app-console');
function showApp(n){if(n==='setup'){appSetup.hidden=false;appConsole.hidden=true}else{appSetup.hidden=true;appConsole.hidden=false}
$$('.app-switch-btn').forEach(b=>b.classList.toggle('active',b.dataset.app===n));
history.replaceState(null,'','#'+n)}
$$('.app-switch-btn').forEach(b=>b.addEventListener('click',()=>showApp(b.dataset.app)));
showApp(location.hash==='#setup'?'setup':'console');

const API={scan:()=>fj('/api/scan'),status:()=>fj('/api/status'),config:()=>fj('/api/config'),saveConfig:b=>pj('/api/config',b),saveWifi:b=>pj('/api/wifi',b),reboot:()=>pj('/api/reboot'),factory:()=>pj('/api/factory-reset')};
async function fj(u){const r=await fetch(u,{cache:'no-store'});if(!r.ok)throw new Error(`${u} → ${r.status}`);return r.json()}
async function pj(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):'{}'});if(!r.ok)throw new Error(`${u} → ${r.status}`);return r.json()}

function showSetupPane(id){$$('.app-setup .pane').forEach(p=>p.classList.remove('active'));$$('.app-setup .nav-item').forEach(n=>n.classList.remove('active'));$('pane-'+id)?.classList.add('active');$$(`.app-setup .nav-item[data-pane="${id}"]`).forEach(n=>n.classList.add('active'));if(id==='diagnostics'||id==='about')refreshStatus()}
$$('.app-setup .nav-item').forEach(n=>n.addEventListener('click',()=>showSetupPane(n.dataset.pane)));

const sigBars=r=>r>=-55?4:r>=-66?3:r>=-76?2:1;
function renderWifi(nets){const list=$('wifiList');if(!nets||!nets.length){list.innerHTML='<div class="empty-state">No networks found</div>';return}
nets.sort((a,b)=>b.rssi-a.rssi);
list.innerHTML=nets.map(n=>{const bars=sigBars(n.rssi),ssid=esc(n.ssid||'');return `<div class="wifi-item" data-ssid="${ssid}" data-secured="${n.secured}"><div class="wifi-signal">${[1,2,3,4].map(i=>`<span class="${i<=bars?'on':''}"></span>`).join('')}</div><div class="wifi-ssid">${ssid}</div><div class="wifi-lock">${n.secured?'🔒':'open'}</div></div>`}).join('');
$$('.wifi-item',list).forEach(it=>it.addEventListener('click',()=>{$$('.wifi-item',list).forEach(i=>i.classList.remove('selected'));it.classList.add('selected');$('ssid').value=it.dataset.ssid;if(it.dataset.secured==='true')$('pwd').focus()}))}
async function doScan(){const list=$('wifiList');list.innerHTML='<div class="empty-state">Scanning…</div>';try{const r=await API.scan();renderWifi(r.networks)}catch(e){list.innerHTML=`<div class="empty-state" style="color:var(--red)">Scan failed: ${e.message}</div>`}}
$('rescanLink')?.addEventListener('click',e=>{e.preventDefault();doScan()});
$('btnConnectWifi')?.addEventListener('click',async()=>{const ssid=$('ssid').value.trim(),pwd=$('pwd').value,hidden=$('hidden').checked;if(!ssid){log.error('Enter an SSID.');return}try{const r=await API.saveWifi({ssid,pwd,hidden});log.ok(r.message||'Connecting…')}catch(e){log.error('Save failed: '+e.message)}});

let cachedDevName='';
async function loadSetupConfig(){try{const c=await API.config();
if(c.device){
  cachedDevName=c.device.name||'';
  if($('devName'))     $('devName').value      =cachedDevName;
  if($('sbDevName'))   $('sbDevName').textContent=cachedDevName||'—';
  if($('devHostname')) $('devHostname').value   =c.device.hostname||'';
}
if(c.can){
  if(c.can.bitrate)$('canBitrate').value=c.can.bitrate.toLowerCase();
  if(c.can.tx_id)$('canTxId').value=c.can.tx_id;
  if(c.can.rx_id)$('canRxId').value=c.can.rx_id;
  if(typeof c.can.tx_gpio==='number')$('canTxGpio').textContent='GPIO '+c.can.tx_gpio;
  if(typeof c.can.rx_gpio==='number')$('canRxGpio').textContent='GPIO '+c.can.rx_gpio;
}
if($('devUrl'))$('devUrl').textContent=location.origin||'—';
}catch(e){console.warn(e)}}

async function saveGateway(){const b={device:{name:$('devName').value.trim(),hostname:($('devHostname')?.value||'').trim()}};try{await API.saveConfig(b);log.ok('Saved. Reboot to apply new hostname.')}catch(e){log.error('Save failed: '+e.message)}}
async function saveCan(){const b={can:{bitrate:$('canBitrate').value,tx_id:$('canTxId').value.trim(),rx_id:$('canRxId').value.trim()}};try{await API.saveConfig(b);log.ok('CAN saved. Reboot to apply.')}catch(e){log.error('Save failed: '+e.message)}}

function setPill(el,kind,text){if(!el)return;el.className='status-pill '+kind;el.innerHTML=`<span class="status-dot"></span>${text}`}
function fmtUptime(sec){const u=sec|0,h=Math.floor(u/3600).toString().padStart(2,'0'),m=Math.floor((u%3600)/60).toString().padStart(2,'0'),ss=(u%60).toString().padStart(2,'0');return `${h}:${m}:${ss}`}
async function refreshStatus(){try{const s=await API.status();
setPill($('diagWifi'),s.wifi?.connected?'ok':'err',s.wifi?.connected?'Connected':'Disconnected');
setPill($('diagWs'),  s.ws?.connected?'ok':'warn', s.ws?.connected?'Connected':'Disconnected');
setPill($('diagCan'), s.flags?.bus_error?'err':'ok', s.flags?.bus_error?'Bus error':'Active');
setPill($('diagFlash'),s.flags?.flash_in_progress?'warn':'ok',s.flags?.flash_in_progress?'In progress':'Idle');
const armed=!!s.flags?.armed;
setPill($('diagArm'),armed?'ok':'warn',armed?'Armed':'Disarmed');
['armBadge','armBadgeFlash','armBadgeCfg'].forEach(id=>{const b=$(id);if(!b)return;b.classList.toggle('armed',armed);b.innerHTML=`<span class="ab-dot"></span>${armed?'Armed':(id==='armBadge'?'Disarmed':'Press arm button on device')}`});
if(s.system){const sys=s.system;
  if($('infoMac'))     $('infoMac').textContent    =sys.mac||'—';
  if($('devMac'))      $('devMac').textContent     =sys.mac||'—';
  if($('infoVersion')) $('infoVersion').textContent=sys.version||'—';
  if($('infoModel'))   $('infoModel').textContent  =sys.model||'—';
  if($('infoFlash'))   $('infoFlash').textContent  =sys.flash_size?(sys.flash_size+' MB'):'—';
  if($('infoUptime'))  $('infoUptime').textContent =fmtUptime(sys.uptime_s||0);
  if($('infoHeap'))    $('infoHeap').textContent   =(sys.free_heap/1024).toFixed(1)+' KB';
  if($('infoHeapMin')) $('infoHeapMin').textContent=sys.min_free_heap?((sys.min_free_heap/1024).toFixed(1)+' KB'):'—';
  if($('sbDevFw'))     $('sbDevFw').textContent    ='FW '+(sys.version||'—');
  if($('sbDevUp'))     $('sbDevUp').textContent    ='Uptime '+fmtUptime(sys.uptime_s||0);
}
if(s.hardware){
  const vinV=typeof s.hardware.vin_mv==='number'?(s.hardware.vin_mv/1000).toFixed(2)+' V':'—';
  if($('infoVin'))  $('infoVin').textContent =vinV;
  if($('diagVin'))  $('diagVin').textContent =vinV;
  if($('mVin'))     $('mVin').textContent    =vinV;
  const tfMounted=!!s.hardware.tf_mounted;
  setPill($('infoTfStatus'),tfMounted?'ok':'err',tfMounted?'Mounted':'Not mounted');
  setPill($('diagTf'),       tfMounted?'ok':'err',tfMounted?'Mounted':'Not mounted');
}
const name=(await API.config().catch(()=>({}))).device?.name;
if(name&&$('sbDevName'))$('sbDevName').textContent=name;
}catch(e){console.warn(e)}}
setInterval(()=>{if(appSetup.hidden){refreshStatus();return}const p=document.querySelector('.app-setup .pane.active')?.id;if(p==='pane-diagnostics'||p==='pane-about')refreshStatus()},3000);

document.addEventListener('click',async e=>{const btn=e.target.closest('[data-action]');if(!btn)return;const a=btn.dataset.action;
try{if(a==='save-gateway')await saveGateway();
else if(a==='save-can')await saveCan();
else if(a==='reboot'){if(!confirm('Reboot the gateway now?'))return;await API.reboot();log.ok('Rebooting…')}
else if(a==='factory-reset'){if(!confirm('This wipes all settings. Continue?'))return;await API.factory();log.ok('Wiped, rebooting…')}
else if(a==='skip-wifi')showSetupPane('gateway');
else if(a==='test-connection'){log.ok(client.connected?'WebSocket connected to gateway':'WebSocket not connected — open Console to connect')}
else if(a==='export-asc')exportASC()
else if(a==='toggle-stats'){const card=$('canStatsCard');if(card){card.hidden=!card.hidden;const tb=$('statsToggle');if(tb)tb.textContent=card.hidden?'📊 Stats':'📊 Hide Stats'}}
else if(a==='reset-stats'){totalRx=0;totalTx=0;_bytesWindow=[];bytesPeak=0;ID_STATS.clear();renderCanStats(0,0);log.info('CAN stats reset')}
else if(a==='clear-live'){FRAMES.length=0;_fpsTimes=[];fpsPeak=0;totalRx=0;totalTx=0;_bytesWindow=[];bytesPeak=0;ID_STATS.clear();renderSpark();renderFrames();renderCanStats(0,0)}
else if(a==='toggle-live'){livePaused=!livePaused;btn.textContent=livePaused?'▶ Resume':'⏸ Pause'}
else if(a==='clear-dtc'){if(!confirm('Clear all stored DTCs on the ECU?'))return;try{await clearDTCs();log.ok('DTCs cleared');renderOvDtcs([])}catch(e){log.error('Clear failed: '+e.message)}}
else if(a==='read-dtc'){if(!client.connected){log.error('Not connected.');return}try{await startSession(0x03);renderOvDtcs(await readDTCs());log.ok('DTCs refreshed')}catch(e){log.error('DTC read failed: '+e.message)}}
else if(a==='read-vehicle')refreshVehicle();
else if(a==='inspect-dtc')await inspectDtc(btn.dataset.dtc||'')
else if(a==='run-cmd'){
    const c=_cmds[parseInt(btn.dataset.idx)];
    if(!c)return;
    if(!client.connected){log.error('Not connected.');return}
    const sid=parseInt(c.sid.replace(/^0x/i,''),16);
    if(isNaN(sid)){log.error('Invalid SID in command.');return}
    const data=(c.data||'').replace(/\s/g,'');
    log.info('→ '+c.name+': SID '+c.sid+(data?' data '+data:''));
    const r=await udsReq(sid,data,2000);
    if(r.status==='positive')log.ok(c.name+': '+(fmtHex(r.data||'')||'(ok)')+'  ['+r.elapsed_ms+' ms]');
    else if(r.status==='negative')log.error(c.name+': NRC 0x'+hb(r.nrc||0)+' — '+(NRC_NAMES[r.nrc]||'unknown'));
    else log.warn(c.name+': '+r.status)}
else if(a==='del-cmd'){_cmds.splice(parseInt(btn.dataset.idx),1);saveCmds();renderCmds()}
else if(a==='select-preset'){const i=parseInt(btn.dataset.idx);selectPreset(i)}
else if(a==='edit-preset'){
    const i=parseInt(btn.dataset.idx);
    const form=document.getElementById('sbPresetForm'+i);
    if(form)form.style.display=form.style.display==='none'?'block':'none'}
else if(a==='cancel-preset'){
    const i=parseInt(btn.dataset.idx);
    const form=document.getElementById('sbPresetForm'+i);
    if(form)form.style.display='none'}
else if(a==='save-preset'){
    const i=parseInt(btn.dataset.idx);
    const icon=($('sbPIcon'+i)?.value||'📟').trim();
    const name=($('sbPName'+i)?.value||'').trim();
    const tx=($('sbPTx'+i)?.value||'').trim();
    const rx=($('sbPRx'+i)?.value||'').trim();
    if(!name||!tx||!rx){log.error('Name, TX and RX are required.');return}
    _presets[i]={icon,name,tx,rx};savePresets();renderPresets();log.ok('Preset "'+name+'" saved.')}
else if(a==='del-preset'){
    const i=parseInt(btn.dataset.idx);
    if(!confirm('Delete preset "'+(_presets[i]?.name||'?')+'"?'))return;
    _presets.splice(i,1);
    if(_activePIdx===i)_activePIdx=_presets.length?0:-1;
    else if(_activePIdx>i)_activePIdx--;
    savePresets();renderPresets();
    if(_activePIdx>=0&&_presets[_activePIdx])applyIds(_presets[_activePIdx].tx,_presets[_activePIdx].rx)}
else if(a==='save-new-preset'){
    const icon=($('sbPNewIcon')?.value||'📟').trim();
    const name=($('sbPNewName')?.value||'').trim();
    const tx=($('sbPNewTx')?.value||'').trim();
    const rx=($('sbPNewRx')?.value||'').trim();
    if(!name||!tx||!rx){log.error('Name, TX and RX are required.');return}
    _presets.push({icon,name,tx,rx});savePresets();renderPresets();
    if($('sbAddPresetForm'))$('sbAddPresetForm').style.display='none';
    ['sbPNewIcon','sbPNewName','sbPNewTx','sbPNewRx'].forEach(id=>{const el=$(id);if(el){if(id==='sbPNewIcon')el.value='📟';else el.value=''}});
    log.ok('Preset "'+name+'" added.')}
else if(a==='cancel-new-preset'){if($('sbAddPresetForm'))$('sbAddPresetForm').style.display='none'}
else if(a==='scan-ecus')scanEcus()
else if(a==='use-scanned-ecu'){applyIds(btn.dataset.tx,btn.dataset.rx);log.info('ECU: '+btn.dataset.tx+' → '+btn.dataset.rx)}
else if(a==='read-ecu-dtc'){
    const tx=btn.dataset.tx,rx=btn.dataset.rx;
    const cid='ecu_'+tx.replace(/[^a-z0-9]/gi,'');
    const out=$(cid);if(!out)return;
    btn.disabled=true;btn.textContent='⏳ Reading…';
    out.innerHTML='';
    try{
        await client.request({type:'uds_request',tx_id:tx,rx_id:rx,sid:'0x10',data:'03',timeout_ms:1500},3000);
        const r=await client.request({type:'uds_request',tx_id:tx,rx_id:rx,sid:'0x19',data:'02FF',timeout_ms:3000},5000);
        if(r.status!=='positive')throw new Error(r.message||'NRC 0x'+hb(r.nrc||0));
        const b=(r.data||'').trim().split(/\s+/).filter(Boolean);
        /* strip positive SID byte (0x59) and subfunction, leave DTC bytes from index 2 */
        const dtcs=[];
        for(let i=2;i+3<b.length;i+=4){
            const code=(parseInt(b[i],16)<<16)|(parseInt(b[i+1],16)<<8)|parseInt(b[i+2],16);
            const st=parseInt(b[i+3],16);
            const prefix=['P','C','B','U'][(code>>22)&3];
            const num=code&0x3FFFFF;
            dtcs.push({code:prefix+num.toString(16).toUpperCase().padStart(4,'0'),status:st});
        }
        if(!dtcs.length){out.innerHTML='<div style="color:var(--green);font-size:12px;padding:4px 8px">✓ No DTCs</div>';}
        else{out.innerHTML=dtcs.map(d=>`<div style="font-size:12px;padding:2px 8px;color:var(--red)">⚠ ${d.code} <span style="color:var(--fg-muted)">[0x${hb(d.status)}]</span></div>`).join('');}
        btn.textContent='📋 DTCs ('+dtcs.length+')';
    }catch(e){out.innerHTML='<div style="color:var(--red);font-size:12px;padding:4px 8px">✗ '+esc(e.message)+'</div>';btn.textContent='📋 DTCs';}
    btn.disabled=false;
}
else if(a==='cancel-flash')log.info('Cancel not implemented yet')
else if(a==='cfg-clear-log'){const h=$('cfgLog');if(h)h.innerHTML=''}
else if(a==='start-did-scan')await startDidScan()
else if(a==='stop-did-scan'){_memScanStop=true;log.info('Stopping scan after current DID…')}
else if(a==='read-mem-block')await readMemBlock()
else if(a==='export-memory')exportMemory()
else if(a==='export-hex')exportHexBin()
else if(a==='start-mem-upload')await startMemUpload()
else if(a==='save-upl-bin')saveUplBin()
else if(a==='clear-upl-log'){const h=$('memUplLog');if(h)h.innerHTML=''}
}catch(err){log.error(a+' failed: '+err.message)}});

loadSetupConfig();

/* WebSocket sempre same-origin sull'ESP32. Override solo via ?ws=... per debug. */
const WS_URL=(new URL(window.location.href).searchParams.get('ws'))||
             `${location.protocol==='https:'?'wss':'ws'}://${location.host||'localhost'}/ws`;

class GatewayClient{
constructor(u){this.url=u;this.ws=null;this.pending=new Map();this.listeners=new Set();this.connected=false;this.reconnectDelay=1000;this._reconnectTimer=null;this._pingTimer=null;this._pongTimer=null}
connect(){
  /* Guard: don't create a second WS while one is already connecting/open */
  if(this.ws&&this.ws.readyState<2)return;
  try{this.ws=new WebSocket(this.url)}catch{this._scheduleReconnect();return}
  this.ws.onopen=()=>{this.connected=true;this.reconnectDelay=1000;updateConnBadge('ok');this._startHB()};
  this.ws.onmessage=ev=>{let m;try{m=JSON.parse(ev.data)}catch{return}
    /* Heartbeat pong: clear watchdog, don't dispatch to handlers */
    if(m.type==='pong'&&!m.id){if(this._pongTimer){clearTimeout(this._pongTimer);this._pongTimer=null}return}
    this._dispatch(m)};
  this.ws.onclose=()=>{this.connected=false;updateConnBadge('err');this._failAll('closed');this._stopHB();this._scheduleReconnect()};
  /* onerror always precedes onclose in browsers, but force-close to guarantee reconnect */
  this.ws.onerror=()=>{try{this.ws.close()}catch{}}}
_startHB(){
  this._stopHB();
  this._pingTimer=setInterval(()=>{
    if(!this.connected)return;
    try{this.ws.send('{"type":"ping"}')}catch{return}
    this._pongTimer=setTimeout(()=>{this._pongTimer=null;try{this.ws.close()}catch{}},4000)
  },5000)}
_stopHB(){
  if(this._pingTimer){clearInterval(this._pingTimer);this._pingTimer=null}
  if(this._pongTimer){clearTimeout(this._pongTimer);this._pongTimer=null}}
_scheduleReconnect(){if(this._reconnectTimer)clearTimeout(this._reconnectTimer);this._reconnectTimer=setTimeout(()=>{this._reconnectTimer=null;this.connect()},this.reconnectDelay);this.reconnectDelay=Math.min(this.reconnectDelay*2,10000)}
_dispatch(m){if(m.id&&this.pending.has(m.id)){const p=this.pending.get(m.id);if(m.type==='uds_response'||(m.type&&m.type.endsWith('_ack'))){clearTimeout(p.timeout);this.pending.delete(m.id);p.resolve(m);return}}
for(const l of this.listeners)l(m)}
_failAll(r){for(const[,p]of this.pending){clearTimeout(p.timeout);p.reject(new Error(r))}this.pending.clear()}
send(o){if(!this.connected)throw new Error('not connected');this.ws.send(JSON.stringify(o))}
request(o,t=10000){if(!o.id)o.id=uid();return new Promise((res,rej)=>{const timeout=setTimeout(()=>{this.pending.delete(o.id);rej(new Error('timeout'))},t);this.pending.set(o.id,{resolve:res,reject:rej,timeout});try{this.send(o)}catch(e){clearTimeout(timeout);this.pending.delete(o.id);rej(e)}})}
on(f){this.listeners.add(f)}
off(f){this.listeners.delete(f)}}

const client=new GatewayClient(WS_URL);

function updateConnBadge(s){const el=$('connBadge');if(!el)return;el.classList.remove('err','warn');const h=WS_URL.replace(/^wss?:\/\//,'');if(s==='ok')el.querySelector('.conn-text').textContent=`Connected · ${h}`;else if(s==='err'){el.classList.add('err');el.querySelector('.conn-text').textContent='Disconnected — retrying…'}else{el.classList.add('warn');el.querySelector('.conn-text').textContent='Connecting…'}}

$$('.app-console .tb-tab').forEach(t=>t.addEventListener('click',()=>{$$('.app-console .tb-tab').forEach(x=>x.classList.remove('active'));$$('.app-console .view').forEach(x=>x.classList.remove('active'));t.classList.add('active');$('view-'+t.dataset.view)?.classList.add('active')}));

const curIds=()=>({tx:($('canTxId')?.value.trim()||'0x7E0'),rx:($('canRxId')?.value.trim()||'0x7E8')});
async function udsReq(sid,data='',t=2000){const{tx,rx}=curIds();const r=await client.request({type:'uds_request',tx_id:tx,rx_id:rx,sid:`0x${hb(sid)}`,data,timeout_ms:t},t+2000);if(r.status!=='positive')throw new Error(r.message||(r.nrc?'NRC 0x'+hb(r.nrc):'Error: '+r.status));return r}
async function readDID(d){const r=await udsReq(0x22,hw(d));return h2b(r.data).slice(2)}
async function readDTCs(){const r=await udsReq(0x19,'02FF',3000);const b=h2b(r.data),out=[];for(let i=2;i+3<b.length;i+=4){const[b0,b1,b2,st]=[b[i],b[i+1],b[i+2],b[i+3]];const grp=['P','C','B','U'][(b0>>6)&3];const code=grp+((b0>>4)&3).toString(10)+(b0&0x0F).toString(16).toUpperCase()+((b1>>4)&0x0F).toString(16).toUpperCase()+(b1&0x0F).toString(16).toUpperCase();out.push({code,raw:`${hb(b0)}${hb(b1)}${hb(b2)}`,failType:b2,status:st})}return out}
async function clearDTCs(){await udsReq(0x14,'FFFFFF',3000)}
async function startSession(t){await udsReq(0x10,hb(t))}

const DID_KNOWN={
0x0C:{len:2,name:'Engine RPM',decode:b=>(((b[0]<<8)|b[1])/4).toFixed(0)+' RPM'},
0x05:{len:1,name:'Coolant Temp',decode:b=>(b[0]-40)+' °C'},
0x0D:{len:1,name:'Vehicle Speed',decode:b=>b[0]+' km/h'},
0x42:{len:2,name:'Control Voltage',decode:b=>(((b[0]<<8)|b[1])/1000).toFixed(2)+' V'},
0x10:{len:2,name:'MAF',decode:b=>(((b[0]<<8)|b[1])/100).toFixed(2)+' g/s'},
0x11:{len:1,name:'Throttle',decode:b=>((b[0]/255)*100).toFixed(1)+' %'},
0x04:{len:1,name:'Engine Load',decode:b=>((b[0]/255)*100).toFixed(1)+' %'},
0x0F:{len:1,name:'Intake Temp',decode:b=>(b[0]-40)+' °C'},
0x0B:{len:1,name:'MAP',decode:b=>b[0]+' kPa'},
0xF190:{len:17,name:'VIN',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
0xF187:{len:10,name:'Part No.',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
0xF189:{len:4,name:'SW Version',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
0xF191:{len:4,name:'HW Version',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
0xF18A:{len:8,name:'Supplier ID',decode:b=>b.map(x=>hb(x)).join(' ')},
0xF18C:{len:10,name:'ECU Serial',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
0xF199:{len:4,name:'Prog. Date',decode:b=>b.map(x=>hb(x)).join(' ')},
0xF180:{len:4,name:'Boot SW',decode:b=>new TextDecoder().decode(new Uint8Array(b))},
};

async function readObdMetrics(){
const readPid=async pid=>{const r=await udsReq(0x01,hb(pid),1500);if(r.status!=='positive')throw new Error('NRC');return h2b(r.data).slice(2)};
const set=(id,v)=>{if($(id))$(id).textContent=v};
try{const b=await readPid(0x0C);set('mRpm',(((b[0]<<8)|b[1])/4).toFixed(0)+' RPM')}catch{}
try{const b=await readPid(0x05);set('mTemp',(b[0]-40)+' °C')}catch{}
try{const b=await readPid(0x42);set('mBatt',(((b[0]<<8)|b[1])/1000).toFixed(2)+' V')}catch{}
try{const b=await readPid(0x10);set('mMaf',(((b[0]<<8)|b[1])/100).toFixed(2)+' g/s')}catch{}}

async function refreshVehicle(){if(!client.connected){log.error('Not connected.');return}
try{
try{await readObdMetrics()}catch{}
await startSession(0x03);
const readText=async did=>{try{return new TextDecoder().decode(await readDID(did))}catch{return null}};
const readHex=async did=>{try{return Array.from(await readDID(did)).map(x=>hb(x)).join(' ')}catch{return null}};
const set=(id,v)=>{if(v&&$(id))$(id).textContent=v};
set('ovVin',await readText(0xF190));
set('ovPart',await readText(0xF187));
set('ovSw',await readText(0xF189));
set('ovHw',await readText(0xF191));
set('ovSupplier',await readHex(0xF18A));
set('ovSerial',await readText(0xF18C));
set('ovBootSw',await readText(0xF180));
set('ovProgDate',await readHex(0xF199));
try{renderOvDtcs(await readDTCs())}catch(e){console.warn(e)}
log.ok('Vehicle refreshed')}catch(e){log.error('Refresh failed: '+e.message)}}

const DTC_STATUS_BITS=['testFailed','failedThisOpCycle','pendingDTC','confirmedDTC','notTestedSinceClear','failedSinceClear','notTestedThisCycle','MIL'];
function dtcStatusHtml(st){return DTC_STATUS_BITS.map((n,i)=>{const v=(st>>i)&1;return`<span style="opacity:${v?1:0.3};color:${v&&i<=3?'var(--red)':v&&i===7?'var(--orange)':'var(--fg-1)'}">${n}</span>`}).join(' · ')}

function renderOvDtcs(dtcs){
const host=$('ovDtcList');if(!host)return;
if(!dtcs.length){
  const empty='<div class="empty-state">No trouble codes stored.</div>';
  host.innerHTML=empty;
  const tab=$('dtcEcm');if(tab)tab.innerHTML=empty;
  return}
host.innerHTML=dtcs.map(d=>{
const confirmed=(d.status&8)?1:0,active=(d.status&1)?1:0,mil=(d.status&0x80)?1:0;
const pill=active?'<span class="dtc-status active">ACTIVE</span>':confirmed?'<span class="dtc-status">CONFIRMED</span>':'<span class="dtc-status">PENDING</span>';
const milBadge=mil?'<span style="color:var(--orange);font-size:10px;font-weight:700;margin-left:6px">MIL</span>':'';
const failTxt=d.failType?`· FailType 0x${hb(d.failType)}`:'';
return`<div class="dtc-row" style="flex-wrap:wrap;row-gap:4px">
<div class="dtc-code">${d.code}${milBadge}</div>
<div class="dtc-desc" style="flex:1;min-width:160px">Raw ${d.raw} ${failTxt}</div>
${pill}
<button class="btn btn-ghost" style="font-size:11px;padding:3px 8px" data-action="inspect-dtc" data-dtc="${d.raw}">Inspect</button>
<div style="width:100%;font-size:10px;color:var(--fg-2);padding:2px 0 0 4px">${dtcStatusHtml(d.status)}</div>
<div data-dtc-freeze="${d.raw}" style="display:none;width:100%;margin-top:6px;padding:6px 8px;background:var(--surface-2);border-radius:6px;font-size:12px"></div>
</div>`}).join('');
/* Mirror to DTC tab */
const tab=$('dtcEcm');if(tab)tab.innerHTML=host.innerHTML;}

async function parseFreezeFrame(bytes){
const rows=[];let i=0;
while(i+3<bytes.length){
const did=(bytes[i]<<8)|bytes[i+1];const recNum=bytes[i+2];i+=3;
const info=DID_KNOWN[did];
if(info&&i+info.len<=bytes.length){const data=Array.from(bytes.slice(i,i+info.len));rows.push({did,name:info.name,raw:data.map(x=>hb(x)).join(' '),decoded:info.decode(data)});i+=info.len}
else{const rem=Array.from(bytes.slice(i));rows.push({did,name:`DID 0x${hw(did)}`,raw:rem.map(x=>hb(x)).join(' '),decoded:null});break}}
return rows}

async function inspectDtc(rawHex){
const panel=document.querySelector(`[data-dtc-freeze="${rawHex}"]`);if(!panel)return;
if(panel.style.display!=='none'){panel.style.display='none';return}
panel.style.display='block';panel.textContent='Loading freeze frame…';
try{
const r=await udsReq(0x19,'04'+rawHex+'01',3000);
if(r.status!=='positive'){panel.textContent='No freeze frame (NRC 0x'+hb(r.nrc||0)+')';return}
const b=h2b(r.data);
/* skip: subFunc(1) + dtcHighMid(3) + statusMask(1) + recNum(1) = 6 bytes before DID data */
const payload=Array.from(b.slice(6));
const rows=await parseFreezeFrame(payload);
if(!rows.length){panel.textContent='No DID data in freeze frame.';return}
panel.innerHTML='<table style="width:100%;border-collapse:collapse">'+
rows.map(row=>`<tr><td style="padding:2px 8px 2px 0;color:var(--fg-2);white-space:nowrap">0x${hw(row.did)} ${row.name}</td><td class="mono" style="padding:2px 0">${row.decoded??row.raw}</td><td class="mono" style="padding:2px 0 2px 8px;color:var(--fg-2);font-size:10px">${row.decoded?row.raw:''}</td></tr>`).join('')+
'</table>'}catch(e){panel.textContent='Error: '+e.message}}

const FRAMES=[],MAX_FRAMES=500;let livePaused=false,_renderDirty=false;

/* ---- Bus activity / sparkline ---- */
const SPARK_N=60;
const SPARK_BUF=new Array(SPARK_N).fill(0);
let _fpsTimes=[],fpsPeak=0;

/* ---- CAN statistics ---- */
const ID_STATS=new Map(); // id -> {rx,tx,bytes}
let totalRx=0,totalTx=0,_bytesWindow=[],bytesPeak=0;

function addFrame(f){
    if(livePaused)return;
    const now=Date.now();
    _fpsTimes.push(now);
    /* byte count from space-separated hex data e.g. "06 62 F1 90" */
    const nb=(f.data||'').trim().split(/\s+/).filter(Boolean).length;
    _bytesWindow.push({t:now,n:nb});
    /* direction counters */
    const isTx=(f.dir||'').toUpperCase()==='TX';
    if(isTx)totalTx++;else totalRx++;
    /* per-ID stats */
    const id=(f.id||'???').toUpperCase();
    const s=ID_STATS.get(id)||{rx:0,tx:0,bytes:0};
    if(isTx)s.tx++;else s.rx++;
    s.bytes+=nb;
    ID_STATS.set(id,s);
    FRAMES.unshift(f);
    if(FRAMES.length>MAX_FRAMES)FRAMES.pop();
    _renderDirty=true;
}

function renderSpark(){
    const W=300,H=44,N=SPARK_BUF.length;
    const peak=Math.max(1,...SPARK_BUF);
    const xs=i=>(i/(N-1))*W,ys=v=>H-(v/peak)*(H-4);
    const pts=SPARK_BUF.map((v,i)=>xs(i).toFixed(1)+','+ys(v).toFixed(1));
    const lineD='M'+pts.join(' L');
    const fillD=lineD+` L${W},${H} L0,${H} Z`;
    $('sparkLine')?.setAttribute('d',lineD);
    $('sparkFill')?.setAttribute('d',fillD);
}

function renderCanStats(fps,bps){
    const card=$('canStatsCard');if(!card||card.hidden)return;
    if($('csTotRx'))$('csTotRx').textContent=totalRx.toLocaleString();
    if($('csTotTx'))$('csTotTx').textContent=totalTx.toLocaleString();
    if($('csFps'))$('csFps').textContent=fps;
    if($('csFpsPk'))$('csFpsPk').textContent=fpsPeak;
    if($('csUniq'))$('csUniq').textContent=ID_STATS.size;
    if($('csBps'))$('csBps').textContent=bps>=1024?(bps/1024).toFixed(1)+' KB/s':bps+' B/s';
    const host=$('idFreqTable');if(!host)return;
    if(!ID_STATS.size){host.innerHTML='<div class="empty-state" style="padding:12px">No frames captured yet.</div>';return}
    const rows=Array.from(ID_STATS.entries())
        .map(([id,s])=>({id,total:s.rx+s.tx,rx:s.rx,tx:s.tx,bytes:s.bytes}))
        .sort((a,b)=>b.total-a.total).slice(0,16);
    const peak=rows[0]?.total||1;
    host.innerHTML=rows.map(r=>{
        const pct=Math.floor((r.total/peak)*100);
        const bStr=r.bytes>=1024?(r.bytes/1024).toFixed(1)+' KB':r.bytes+' B';
        return`<div class="id-freq-row">
<div class="id-freq-id">${r.id}</div>
<div class="id-freq-bar-wrap"><div class="id-freq-bar" style="width:${pct}%"></div></div>
<div class="id-freq-counts"><span class="log-dir rx" style="font-size:9.5px">RX ${r.rx}</span><span class="log-dir tx" style="font-size:9.5px">TX ${r.tx}</span></div>
<div class="id-freq-bytes">${bStr}</div>
</div>`}).join('')}

setInterval(()=>{
    const now=Date.now(),cut=now-1000;
    while(_fpsTimes.length&&_fpsTimes[0]<cut)_fpsTimes.shift();
    while(_bytesWindow.length&&_bytesWindow[0].t<cut)_bytesWindow.shift();
    const fps=_fpsTimes.length;
    const bps=_bytesWindow.reduce((a,b)=>a+b.n,0);
    if(fps>fpsPeak)fpsPeak=fps;
    if(bps>bytesPeak)bytesPeak=bps;
    SPARK_BUF.shift();SPARK_BUF.push(fps);
    if($('busFps'))$('busFps').textContent=fps;
    if($('busPeak'))$('busPeak').textContent=fpsPeak;
    renderSpark();
    renderCanStats(fps,bps);
},1000);
function renderFrames(){const host=$('frameLog');if(!host)return;
const flt=($('liveFilter')?.value||'').toUpperCase(),dir=$('liveDir')?.value||'all';
const rows=FRAMES.filter(f=>(dir==='all'||f.dir.toLowerCase()===dir)&&(!flt||f.id.includes(flt)||f.data.includes(flt)));
host.innerHTML=rows.map(f=>`<div class="log-row"><div class="log-time">${f.t}</div><div><span class="log-dir ${f.dir.toLowerCase()}">${f.dir}</span></div><div class="log-id">${f.id}</div><div class="log-data">${f.data}</div><div class="log-meta">${f.info}</div></div>`).join('')}
$('liveFilter')?.addEventListener('input',renderFrames);
$('liveDir')?.addEventListener('change',renderFrames);
/* RAF render loop: decouples frame ingestion from DOM updates, caps at ~60fps */
(function _liveTick(){if(_renderDirty){renderFrames();_renderDirty=false}requestAnimationFrame(_liveTick)})();

client.on(m=>{const t=(performance.now()/1000).toFixed(3);
if(m.type==='uds_response')addFrame({t,dir:'RX',id:m.id||'',data:fmtHex(m.data||''),info:m.status==='positive'?`SID ${m.sid}`:m.status==='negative'?`NRC ${m.nrc}`:m.status});
else if(m.type==='can_frame')addFrame({t:m.ts||t,dir:m.dir||'RX',id:m.id||'',data:fmtHex(m.data||''),info:m.info||''})});

let flashFile=null;
const dropZone=$('dropZone'),fileInput=$('fwFile');
if(dropZone&&fileInput){['dragenter','dragover'].forEach(ev=>dropZone.addEventListener(ev,e=>{e.preventDefault();dropZone.classList.add('over')}));
['dragleave','drop'].forEach(ev=>dropZone.addEventListener(ev,e=>{e.preventDefault();dropZone.classList.remove('over')}));
dropZone.addEventListener('drop',e=>{if(e.dataTransfer.files.length)handleFile(e.dataTransfer.files[0])});
fileInput.addEventListener('change',e=>{if(e.target.files.length)handleFile(e.target.files[0])})}

async function handleFile(f){flashFile=f;$('fileName').textContent=f.name;$('fileMeta').textContent=`${f.size.toLocaleString()} bytes · reading…`;$('fileInfo').hidden=false;
const buf=new Uint8Array(await f.arrayBuffer()),crc=crc32(buf);flashFile._crc=crc;flashFile._buf=buf;
$('fileMeta').textContent=`${f.size.toLocaleString()} bytes · CRC32 0x${crc.toString(16).toUpperCase().padStart(8,'0')}`;
$('flashBytes').textContent='0';$('flashBytesTot').textContent=`/ ${f.size.toLocaleString()} bytes`}

let _crcT=null;
function crc32(b){if(!_crcT){_crcT=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=(c&1)?(0xEDB88320^(c>>>1)):(c>>>1);_crcT[n]=c}}
let c=0xFFFFFFFF;for(let i=0;i<b.length;i++)c=_crcT[(c^b[i])&0xFF]^(c>>>8);return(c^0xFFFFFFFF)>>>0}
function b2b64(u){let s='';for(let i=0;i<u.length;i++)s+=String.fromCharCode(u[i]);return btoa(s)}

async function uploadFw(buf,crc,cs=2048){await client.request({type:'flash_upload_begin',size:buf.length,crc32:crc});
for(let off=0;off<buf.length;off+=cs){const end=Math.min(off+cs,buf.length);
await client.request({type:'flash_upload_chunk',offset:off,data_b64:b2b64(buf.subarray(off,end))});
const p=Math.floor((end*100)/buf.length);
$('flashStage').textContent='Uploading firmware';$('flashTitle').textContent='Uploading to gateway…';
$('flashBarFill').style.width=p+'%';$('flashBytes').textContent=end.toLocaleString();$('flashPct').textContent=p+'%'}
await client.request({type:'flash_upload_end'})}

const flashBtn=$('flashBtn');
flashBtn?.addEventListener('click',startFlash);

async function startFlash(){if(!flashFile||!flashFile._buf){log.error('Select a firmware file first.');return}
if(!client.connected){log.error('Not connected to gateway.');return}
$$('.step').forEach(s=>{s.classList.remove('done','active','error');const i=s.querySelector('.step-icon'),idx=Array.from(s.parentNode.children).indexOf(s)+1;i.textContent=idx;s.querySelector('.step-time').textContent=''});
flashBtn.disabled=true;flashBtn.textContent='Flashing…';const ts=Date.now();
try{await uploadFw(flashFile._buf,flashFile._crc);
let settle;const done=new Promise(r=>settle=r);
const onMsg=m=>{if(m.type==='flash_progress')updateFlashProgress(m,ts);if(m.type==='flash_result'){settle(m);client.off(onMsg)}};
client.on(onMsg);
const ack=await client.request({type:'flash_start',tx_id:$('flashTxId').value,rx_id:$('flashRxId').value,address:$('flashAddr').value,security_level:$('flashSec').value,erase_before:true,erase_routine_id:$('flashErase').value,check_routine_id:$('flashCheck').value},5000);
if(ack.status!=='ok'){client.off(onMsg);throw new Error(ack.message||'start refused')}
const to=Math.max(60000,flashFile._buf.length/100);
const r=await Promise.race([done,new Promise((_,rj)=>setTimeout(()=>rj(new Error('flash timeout')),to))]);
if(r.status==='ok'){$('flashStage').textContent='Complete';$('flashTitle').textContent='✓ Flash completed successfully';flashBtn.textContent='Done';flashBtn.style.background='var(--green)';log.ok('Flash completed')}else throw new Error('flash failed, code '+r.code)}
catch(e){log.error(e.message);$('flashStage').textContent='Error';$('flashTitle').textContent='✗ '+e.message;
const a=document.querySelector('.step.active');if(a){a.classList.remove('active');a.classList.add('error')}
flashBtn.disabled=false;flashBtn.textContent='Retry'}}

function updateFlashProgress(m,ts){const phase=m.phase,order=['session','security','erase','request_download','transfer','transfer_exit','verify','reset','done'],idx=order.indexOf(phase);
$$('.step').forEach(s=>{const k=order.indexOf(s.dataset.phase);
if(k<idx){if(!s.classList.contains('done')){s.classList.remove('active');s.classList.add('done');s.querySelector('.step-icon').innerHTML='<svg viewBox="0 0 24 24"><path d="M9 16.2L4.8 12l-1.4 1.4L9 19 21 7l-1.4-1.4L9 16.2z"/></svg>';s.querySelector('.step-time').textContent=((Date.now()-ts)/1000).toFixed(1)+'s'}}
else if(k===idx&&!s.classList.contains('done')){$$('.step').forEach(o=>o.classList.remove('active'));s.classList.add('active');s.querySelector('.step-icon').innerHTML='<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/></svg>'}});
const titles={session:'Opening programming session…',security:'Unlocking security access…',erase:'Erasing memory…',request_download:'Requesting download…',transfer:'Transferring firmware…',transfer_exit:'Finalizing transfer…',verify:'Verifying dependencies…',reset:'Resetting ECU…',done:'Flash completed'};
$('flashStage').textContent='Step '+Math.max(1,idx+1);$('flashTitle').textContent=titles[phase]||phase;
const done=m.done||0,total=m.total||0;
if(total>0){const p=Math.floor((done*100)/total);$('flashBarFill').style.width=p+'%';$('flashBytes').textContent=done.toLocaleString();$('flashPct').textContent=p+'%';
const el=(Date.now()-ts)/1000,eta=p>0?Math.max(0,Math.floor(el*(100-p)/p)):0;
$('flashEta').textContent=eta>0?eta+'s':'—';$('flashBar').classList.remove('indet')}
else $('flashBar').classList.add('indet')}

updateConnBadge('warn');
client.connect();
doScan();

/* =====================================================================
   ODIS Config Writer
   Parses VW ODIS XML datasets (kalibrierung / parametrierung / service42)
   and replays the calibration + parametrization writes over UDS.
   ===================================================================== */

let odisDataset = null;

async function parseOdisFiles(files) {
    const ds = { calibrations: [], param: null, meta: {} };
    for (const f of files) {
        const text = await f.text();
        const doc = new DOMParser().parseFromString(text, 'application/xml');
        const resp = doc.querySelector('RESPONSE');
        if (!resp) continue;
        const name = resp.getAttribute('NAME');
        if (name === 'GetAdjustCalibrationData') {
            ds.calibrations = Array.from(doc.querySelectorAll('ADJUST_CALIBRATION_DATA')).map(el => ({
                diagAddr: el.getAttribute('DIAGNOSTIC_ADDRESS') || '0009',
                did: (el.getAttribute('RDI') || '').replace(/^0x/i, '').toUpperCase().padStart(4, '0'),
                data: el.textContent.trim().toUpperCase(),
                login: el.getAttribute('LOGIN') || '',
                zdcName: el.getAttribute('ZDC_NAME') || '',
                zdcVersion: el.getAttribute('ZDC_VERSION') || '',
            })).filter(e => e.did && e.data);
        } else if (name === 'GetParametrizeData') {
            const pd = doc.querySelector('PARAMETER_DATA');
            if (pd) {
                const startAddr = parseInt(pd.getAttribute('START_ADDRESS') || '0', 16);
                const rawText = pd.textContent.trim();
                /* format: "0xFE,0x00,..." or plain "FE00..." */
                let bytes;
                if (rawText.includes(',')) {
                    bytes = rawText.split(',').map(s => parseInt(s.trim(), 16));
                } else {
                    const hex = rawText.replace(/\s/g, '');
                    bytes = [];
                    for (let i = 0; i < hex.length; i += 2) bytes.push(parseInt(hex.substr(i, 2), 16));
                }
                ds.param = {
                    startAddr,
                    data: new Uint8Array(bytes.filter(b => !isNaN(b))),
                    prIdx: pd.getAttribute('PR_IDX') || '',
                    zdcName: pd.getAttribute('ZDC_NAME') || '',
                    zdcVersion: pd.getAttribute('ZDC_VERSION') || '',
                };
            }
        } else if (name === 'GetRepairAdvice') {
            ds.meta = {
                vin: doc.querySelector('VIN')?.textContent?.trim() || '',
                changeCode: doc.querySelector('CHANGE_CODE')?.textContent?.trim() || '',
            };
        }
    }
    return ds;
}

function renderCfgDataset(ds) {
    $('cfgInfo').hidden = false;
    const cal0 = ds.calibrations[0] || {};
    const diagAddrRaw = cal0.diagAddr || ds.param?.zdcName || '0009';
    const diagAddrNum = parseInt(diagAddrRaw, 16) || parseInt(diagAddrRaw, 10) || 0;
    $('cfgDiagAddr').textContent = '0x' + diagAddrNum.toString(16).toUpperCase().padStart(4, '0') + ' (' + diagAddrNum + ')';
    $('cfgZdcName').textContent = cal0.zdcName || ds.param?.zdcName || '—';
    $('cfgZdcVer').textContent = cal0.zdcVersion || ds.param?.zdcVersion || '—';
    $('cfgVin').textContent = ds.meta?.vin || '—';
    $('cfgLogin').textContent = cal0.login || '—';
    $('cfgCalCount').textContent = ds.calibrations.length
        ? ds.calibrations.length + ' × SID 0x2E (WriteDataByIdentifier)'
        : 'None';
    $('cfgParamSize').textContent = ds.param && ds.param.data.length
        ? ds.param.data.length + ' bytes at 0x' + ds.param.startAddr.toString(16).toUpperCase() + ' (PR_IDX: ' + (ds.param.prIdx || '—') + ')'
        : 'None';
}

function cfgAppendLog(msg, kind) {
    $('cfgLogCard').hidden = false;
    const host = $('cfgLog');
    if (!host) return;
    const row = document.createElement('div');
    row.className = 'log-row';
    if (kind === 'ok')   row.style.color = 'var(--green)';
    if (kind === 'err')  row.style.color = 'var(--red)';
    if (kind === 'warn') row.style.color = 'var(--orange)';
    row.textContent = '[' + new Date().toISOString().slice(11, 23) + '] ' + msg;
    host.insertBefore(row, host.firstChild);
}

async function cfgGetSeed() {
    if (!client.connected) { log.error('Not connected.'); return; }
    const txId = $('cfgTxId').value.trim() || '0x7E0';
    const rxId = $('cfgRxId').value.trim() || '0x7E8';
    const secLevelStr = $('cfgSecLevel').value;
    if (secLevelStr === 'skip') { cfgAppendLog('Security skipped — no seed to fetch.', 'warn'); return; }
    const secLevel = parseInt(secLevelStr, 16);
    cfgAppendLog('Opening extended session…');
    try {
        const sr = await client.request({type:'uds_request',tx_id:txId,rx_id:rxId,sid:'0x10',data:'03',timeout_ms:3000}, 5000);
        if (sr.status !== 'positive') { cfgAppendLog('Session failed: ' + sr.status, 'err'); return; }
        cfgAppendLog('Session OK — requesting seed…');
        const seedR = await client.request({type:'uds_request',tx_id:txId,rx_id:rxId,sid:'0x27',data:hb(secLevel),timeout_ms:2000}, 4000);
        if (seedR.status !== 'positive') { cfgAppendLog('Seed request failed: nrc=0x' + hb(seedR.nrc||0), 'err'); return; }
        const seedHex = (seedR.data || '').replace(/\s/g,'').slice(2); /* skip echo byte */
        cfgAppendLog('SEED: ' + (seedHex || '(empty — already unlocked)'), 'warn');
        cfgAppendLog('Compute the key and paste it into "Manual Key", then click Write Config.', 'warn');
    } catch (e) { cfgAppendLog('Error: ' + e.message, 'err'); }
}

async function runOdisConfig() {
    if (!odisDataset) { log.error('Load an ODIS dataset first.'); return; }
    if (!odisDataset.calibrations.length && !odisDataset.param) { log.error('Dataset is empty.'); return; }
    if (!client.connected) { log.error('Not connected to gateway.'); return; }

    const txId = $('cfgTxId').value.trim() || '0x7E0';
    const rxId = $('cfgRxId').value.trim() || '0x7E8';
    const secLevelStr = $('cfgSecLevel').value;
    const skipSec = secLevelStr === 'skip';
    const secLevel = skipSec ? 0 : parseInt(secLevelStr, 16);
    const manualKey = ($('cfgManualKey').value || '').replace(/[\s\-]/g, '').toUpperCase();
    const paramMethod = $('cfgParamMethod').value;

    const btn = $('cfgRunBtn');
    btn.disabled = true; btn.textContent = 'Running…';
    $('cfgStage').textContent = 'Running';
    $('cfgTitle').textContent = 'Writing ODIS configuration…';
    $('cfgBar').style.display = '';
    $('cfgBarFill').style.width = '0%';

    let passCount = 0, failCount = 0, allOk = true;

    const req = (sid, data, t) => client.request(
        {type:'uds_request', tx_id:txId, rx_id:rxId, sid, data: data||'', timeout_ms: t||3000},
        (t||3000) + 2000
    );

    try {
        /* 1. Extended diagnostic session */
        cfgAppendLog('Opening extended diagnostic session (0x10 03)…');
        const sR = await req('0x10', '03', 3000);
        if (sR.status !== 'positive') { cfgAppendLog('Session failed: ' + sR.status + ' nrc=0x' + hb(sR.nrc||0), 'err'); throw new Error('DiagnosticSessionControl failed'); }
        cfgAppendLog('Session OK', 'ok');

        /* 2. Security access */
        if (!skipSec) {
            cfgAppendLog('Requesting seed (SID 0x27 sub 0x' + hb(secLevel) + ')…');
            const seedR = await req('0x27', hb(secLevel), 2000);
            if (seedR.status !== 'positive') {
                cfgAppendLog('Seed request failed: nrc=0x' + hb(seedR.nrc||0), 'err');
                throw new Error('SecurityAccess seed request failed');
            }
            const seedHex = (seedR.data || '').replace(/\s/g,'').slice(2);
            cfgAppendLog('Seed: ' + (seedHex || '(empty — ECU already unlocked or no security)'));

            if (seedHex && seedHex !== '0000' && seedHex !== '00000000') {
                const keyHex = manualKey || '00'.repeat(seedHex.length / 2);
                if (!manualKey) cfgAppendLog('No manual key — trying zero-key (may fail with NRC 0x35)', 'warn');
                const keyR = await req('0x27', hb(secLevel + 1) + keyHex, 2000);
                if (keyR.status !== 'positive') {
                    cfgAppendLog('Key rejected: nrc=0x' + hb(keyR.nrc||0) + (keyR.nrc===0x35?' (wrong key)':keyR.nrc===0x36?' (attempts exceeded)':''), 'err');
                    cfgAppendLog('Enter the correct computed key in "Manual Key" and retry.', 'warn');
                    throw new Error('SecurityAccess key rejected');
                }
                cfgAppendLog('Security access granted', 'ok');
            } else {
                cfgAppendLog('Seed is zero — ECU already unlocked', 'ok');
            }
        } else {
            cfgAppendLog('Security skipped by user.', 'warn');
        }

        /* 3. Calibration writes — SID 0x2E (WriteDataByIdentifier) */
        const cals = odisDataset.calibrations;
        if (cals.length) {
            cfgAppendLog('Writing ' + cals.length + ' calibration entries (SID 0x2E)…');
            for (let i = 0; i < cals.length; i++) {
                const cal = cals[i];
                const wdbiData = cal.did + cal.data;   /* DID(4 hex chars) + value bytes */
                const r = await req('0x2E', wdbiData, 3000);
                if (r.status === 'positive') {
                    passCount++;
                } else {
                    failCount++;
                    cfgAppendLog('[' + (i+1) + '/' + cals.length + '] DID 0x' + cal.did + ' FAIL nrc=0x' + hb(r.nrc||0), 'err');
                }
                /* Progress bar */
                $('cfgBarFill').style.width = Math.floor(((i+1) / (cals.length + (odisDataset.param?1:0))) * 100) + '%';
                /* Log summary every 10 entries */
                if ((i+1) % 10 === 0 || i === cals.length - 1) {
                    cfgAppendLog('[' + (i+1) + '/' + cals.length + '] ' + passCount + ' OK, ' + failCount + ' failed', failCount ? 'warn' : 'ok');
                }
            }
        }

        /* 4. Parametrization block */
        if (odisDataset.param && odisDataset.param.data.length) {
            const { startAddr, data } = odisDataset.param;
            cfgAppendLog('Writing param block: ' + data.length + ' bytes at 0x' + startAddr.toString(16).toUpperCase() + ' via ' + (paramMethod === 'dl' ? '0x34/0x36/0x37' : '0x3D') + '…');

            let paramOk = false;
            if (paramMethod === 'wma') {
                /* SID 0x3D WriteMemoryByAddress
                   addressAndLengthFormatIdentifier: high nibble = bytes for size, low nibble = bytes for addr
                   Use 0x24: 2-byte size, 4-byte address */
                const addrB = [(startAddr>>24)&0xFF,(startAddr>>16)&0xFF,(startAddr>>8)&0xFF,startAddr&0xFF];
                const sizeB = [(data.length>>8)&0xFF, data.length&0xFF];
                const payload = [0x24, ...addrB, ...sizeB, ...data];
                const payHex = payload.map(b => hb(b)).join('');
                const r = await req('0x3D', payHex, 10000);
                paramOk = r.status === 'positive';
                if (!paramOk) {
                    cfgAppendLog('0x3D failed nrc=0x' + hb(r.nrc||0) + '. Try the "0x34/0x36/0x37 Download" method.', 'err');
                    allOk = false;
                }
            } else {
                /* SID 0x34 RequestDownload → 0x36 TransferData × N → 0x37 RequestTransferExit
                   Use same address/size encoding as 0x3D: ALFI=0x24 */
                const addrB = [(startAddr>>24)&0xFF,(startAddr>>16)&0xFF,(startAddr>>8)&0xFF,startAddr&0xFF];
                const sizeB = [(data.length>>8)&0xFF, data.length&0xFF];
                const rdPay = [0x00, 0x24, ...addrB, ...sizeB];
                const rdR = await req('0x34', rdPay.map(b=>hb(b)).join(''), 5000);
                if (rdR.status !== 'positive') {
                    cfgAppendLog('0x34 RequestDownload failed nrc=0x' + hb(rdR.nrc||0), 'err');
                    allOk = false;
                } else {
                    /* Extract max block size from response */
                    const rdBytes = h2b(rdR.data || '');
                    const lfi = rdBytes[0] || 0;
                    const lfiLen = (lfi >> 4) & 0x0F;
                    let mbs = 0;
                    for (let i = 0; i < lfiLen && i + 1 < rdBytes.length; i++) mbs = (mbs << 8) | rdBytes[1 + i];
                    const chunkSize = Math.max(0, (mbs || 256) - 2); /* subtract SID + BSC bytes */
                    let bsc = 1, off = 0;
                    while (off < data.length) {
                        const end = Math.min(off + chunkSize, data.length);
                        const chunk = data.subarray(off, end);
                        const tdPay = [bsc, ...chunk].map(b => hb(b)).join('');
                        const tdR = await req('0x36', tdPay, 10000);
                        if (tdR.status !== 'positive') {
                            cfgAppendLog('0x36 TransferData bsc=' + bsc + ' failed nrc=0x' + hb(tdR.nrc||0), 'err');
                            allOk = false; break;
                        }
                        bsc = (bsc % 0xFF) + 1;
                        off = end;
                    }
                    if (allOk) {
                        const teR = await req('0x37', '', 5000);
                        paramOk = teR.status === 'positive';
                        if (!paramOk) { cfgAppendLog('0x37 RequestTransferExit failed nrc=0x' + hb(teR.nrc||0), 'err'); allOk = false; }
                    }
                }
            }
            if (paramOk) cfgAppendLog('Param block written OK', 'ok');
            $('cfgBarFill').style.width = '100%';
        } else {
            $('cfgBarFill').style.width = '100%';
        }

        const ok = allOk && failCount === 0;
        $('cfgStage').textContent = ok ? 'Done' : 'Partial';
        $('cfgTitle').textContent = ok
            ? '✓ Config write complete (' + passCount + ' calibrations)'
            : '⚠ Completed — ' + failCount + ' calibration failure(s)' + (!allOk ? ' + param error' : '');
        log[ok ? 'ok' : 'warn']('Config write: ' + passCount + ' OK, ' + failCount + ' failed');

    } catch (e) {
        $('cfgStage').textContent = 'Error';
        $('cfgTitle').textContent = '✗ ' + e.message;
        cfgAppendLog('Aborted: ' + e.message, 'err');
        log.error('Config: ' + e.message);
        allOk = false;
    }

    btn.disabled = false;
    btn.textContent = 'Write Config';
}

async function handleCfgFiles(files) {
    cfgAppendLog('Parsing ' + files.length + ' file(s)…');
    try {
        odisDataset = await parseOdisFiles(Array.from(files));
        if (!odisDataset.calibrations.length && !odisDataset.param) {
            log.error('No ODIS calibration or parametrization data found in selected files.');
            cfgAppendLog('No ODIS data found. Check that files contain GetAdjustCalibrationData or GetParametrizeData responses.', 'err');
            odisDataset = null;
            return;
        }
        renderCfgDataset(odisDataset);
        cfgAppendLog(
            'Loaded: ' + odisDataset.calibrations.length + ' calibration entries' +
            (odisDataset.param ? ', param block ' + odisDataset.param.data.length + ' B' : '') +
            (odisDataset.meta?.vin ? ' — VIN ' + odisDataset.meta.vin : ''),
            'ok'
        );
        $('cfgStage').textContent = 'Ready';
        $('cfgTitle').textContent = 'Dataset loaded — configure session parameters and click Write Config';
    } catch (e) {
        log.error('Parse error: ' + e.message);
        cfgAppendLog('Parse error: ' + e.message, 'err');
        odisDataset = null;
    }
}

const cfgDrop = $('cfgDropZone'), cfgFilesInput = $('cfgFiles');
if (cfgDrop && cfgFilesInput) {
    ['dragenter','dragover'].forEach(ev => cfgDrop.addEventListener(ev, e => { e.preventDefault(); cfgDrop.classList.add('over'); }));
    ['dragleave','drop'].forEach(ev => cfgDrop.addEventListener(ev, e => { e.preventDefault(); cfgDrop.classList.remove('over'); }));
    cfgDrop.addEventListener('drop', async e => { if (e.dataTransfer.files.length) await handleCfgFiles(e.dataTransfer.files); });
    cfgFilesInput.addEventListener('change', async e => { if (e.target.files.length) await handleCfgFiles(e.target.files); });
}
$('cfgRunBtn')?.addEventListener('click', runOdisConfig);
$('cfgSeedBtn')?.addEventListener('click', cfgGetSeed);

/* =====================================================================
   ECU Sidebar — dynamic preset system
   ===================================================================== */
const DEFAULT_PRESETS=[
    {icon:'🔧',name:'Engine Control',tx:'0x7E0',rx:'0x7E8'},
    {icon:'⚙️', name:'Transmission',  tx:'0x7E1',rx:'0x7E9'},
    {icon:'🛡️', name:'ABS / ESP',     tx:'0x7E2',rx:'0x7EA'},
    {icon:'💡', name:'Body Control',  tx:'0x760',rx:'0x768'},
];
let _presets,_activePIdx=0;
try{_presets=JSON.parse(localStorage.getItem('uds_presets')||'null')}catch{}
if(!Array.isArray(_presets)||!_presets.length)_presets=DEFAULT_PRESETS.map(p=>({...p}));
function savePresets(){try{localStorage.setItem('uds_presets',JSON.stringify(_presets))}catch{}}

function renderPresets(){
    const host=$('sbPresetList');if(!host)return;
    if(!_presets.length){host.innerHTML='<div class="sb-empty-cmd">No presets — click + Add.</div>';return}
    host.innerHTML=_presets.map((p,i)=>{
        const active=i===_activePIdx;
        return`<div class="sb-item${active?' active':''}" data-action="select-preset" data-idx="${i}" role="button" tabindex="0">
  <span class="sb-preset-icon">${esc(p.icon||'📟')}</span>
  <div class="sb-preset-content">
    <div class="sb-preset-name">${esc(p.name)}</div>
    <div class="sb-meta">${esc(p.tx)} → ${esc(p.rx)}</div>
  </div>
  <div class="sb-preset-acts">
    <span class="sb-preset-edit-btn" data-action="edit-preset" data-idx="${i}" title="Edit" role="button" tabindex="0">✏️</span>
    <span class="sb-preset-del-btn" data-action="del-preset" data-idx="${i}" title="Delete" role="button" tabindex="0">✕</span>
  </div>
</div>
<div class="sb-preset-form" id="sbPresetForm${i}" style="display:none">
  <div class="sb-form-row">
    <input class="sb-form-input sb-form-icon-inp" id="sbPIcon${i}" value="${esc(p.icon||'📟')}" maxlength="4" autocomplete="off" title="Emoji">
    <input class="sb-form-input" id="sbPName${i}" value="${esc(p.name)}" placeholder="Name" autocomplete="off">
  </div>
  <div class="sb-form-row">
    <input class="sb-form-input mono" id="sbPTx${i}" value="${esc(p.tx)}" placeholder="TX 0x7E0" autocomplete="off">
    <input class="sb-form-input mono" id="sbPRx${i}" value="${esc(p.rx)}" placeholder="RX 0x7E8" autocomplete="off">
  </div>
  <div class="sb-form-row">
    <button class="btn btn-primary" data-action="save-preset" data-idx="${i}" type="button" style="flex:1;font-size:11.5px;padding:5px 0">Save</button>
    <button class="btn btn-ghost" data-action="cancel-preset" data-idx="${i}" type="button" style="font-size:11.5px;padding:5px 10px">Cancel</button>
  </div>
</div>`}).join('')}

function selectPreset(i){
    _activePIdx=i;renderPresets();
    const p=_presets[i];if(!p)return;
    applyIds(p.tx,p.rx);
    log.info('ECU: '+p.icon+' '+p.name+' · '+p.tx+' → '+p.rx)}

function deactivatePresets(){_activePIdx=-1;renderPresets()}

function applyIds(tx, rx) {
    if ($('canTxId')) $('canTxId').value = tx;
    if ($('canRxId')) $('canRxId').value = rx;
    if ($('sbCustomTx')) $('sbCustomTx').value = tx;
    if ($('sbCustomRx')) $('sbCustomRx').value = rx;
}

$('sbAddPresetBtn')?.addEventListener('click',()=>{
    const f=$('sbAddPresetForm');if(f)f.style.display=f.style.display==='none'?'block':'none'});

/* =====================================================================
   Custom address block
   ===================================================================== */
function syncCustomInputs() {
    const tx = ($('sbCustomTx')?.value || '').trim();
    const rx = ($('sbCustomRx')?.value || '').trim();
    if ($('canTxId')) $('canTxId').value = tx || '0x7E0';
    if ($('canRxId')) $('canRxId').value = rx || '0x7E8';
}

$('sbCustomApply')?.addEventListener('click', () => {
    _activePIdx=-1;renderPresets();
    syncCustomInputs();
    log.info('Custom: ' + ($('canTxId')?.value || '') + ' → ' + ($('canRxId')?.value || ''));
});
['sbCustomTx', 'sbCustomRx'].forEach(id => {
    $(id)?.addEventListener('keydown', e => { if (e.key === 'Enter') $('sbCustomApply')?.click(); });
});

/* =====================================================================
   Command shortcuts — saved to localStorage
   ===================================================================== */
let _cmds = [];
try { _cmds = JSON.parse(localStorage.getItem('uds_cmds') || '[]'); } catch {}

function saveCmds() {
    try { localStorage.setItem('uds_cmds', JSON.stringify(_cmds)); } catch {}
}

function renderCmds() {
    const host = $('sbCmdList');
    if (!host) return;
    if (!_cmds.length) {
        host.innerHTML = '<div class="sb-empty-cmd">No commands yet.</div>';
        return;
    }
    host.innerHTML = _cmds.map((c, i) => {
        const tip = 'SID ' + c.sid + (c.data ? '  data ' + c.data : '');
        return `<div class="sb-cmd-item">` +
            `<span class="sb-cmd-name" title="${tip}">${c.name}</span>` +
            `<span class="sb-cmd-run" data-action="run-cmd" data-idx="${i}" title="${tip}" role="button" tabindex="0">▶</span>` +
            `<span class="sb-cmd-del" data-action="del-cmd" data-idx="${i}" title="Remove" role="button" tabindex="0">✕</span>` +
            `</div>`;
    }).join('');
}
renderCmds();

/* Initial preset render + apply first preset IDs */
renderPresets();
if(_presets.length&&_activePIdx>=0){const p=_presets[_activePIdx];if(p)applyIds(p.tx,p.rx)}

/* keyboard accessibility for div/span "buttons" */
['sbCustomApply','sbAddCmdBtn','sbAddPresetBtn'].forEach(id=>{
    $(id)?.addEventListener('keydown',e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();$(id).click()}});
});

$('sbAddCmdBtn')?.addEventListener('click', () => {
    const f = $('sbAddCmdForm');
    if (f) f.style.display = f.style.display === 'none' ? 'block' : 'none';
});
$('sbCancelCmdBtn')?.addEventListener('click', () => {
    if ($('sbAddCmdForm')) $('sbAddCmdForm').style.display = 'none';
});
$('sbSaveCmdBtn')?.addEventListener('click', () => {
    const name = ($('sbCmdName')?.value || '').trim();
    const sid  = ($('sbCmdSid')?.value  || '').trim();
    const data = ($('sbCmdData')?.value || '').trim().replace(/\s/g, '');
    if (!name || !sid) { log.error('Command needs a name and SID.'); return; }
    const sidNum = parseInt(sid.replace(/^0x/i, ''), 16);
    if (isNaN(sidNum)) { log.error('Invalid SID.'); return; }
    _cmds.push({ name, sid: '0x' + hb(sidNum), data });
    saveCmds();
    renderCmds();
    if ($('sbAddCmdForm')) $('sbAddCmdForm').style.display = 'none';
    ['sbCmdName', 'sbCmdSid', 'sbCmdData'].forEach(id => { if ($(id)) $(id).value = ''; });
    log.ok('Command "' + name + '" saved.');
});

/* =====================================================================
   Sidebar — Session / Bootloader / Security Access + Seed-to-Key engine
   ===================================================================== */
let _sbSeedHex = '';

/* --- S2K helpers --- */
function s2kRotl(n, bits) {
    n = n >>> 0; bits &= 31;
    return bits === 0 ? n : ((n << bits) | (n >>> (32 - bits))) >>> 0;
}

function s2kCompute(seedHex) {
    const algo = $('sbS2kAlgo')?.value || 'none';
    if (algo === 'none') return null;
    const seed = parseInt(seedHex, 16);
    if (isNaN(seed)) return null;

    if (algo === 'demo') {
        const key = s2kRotl((seed ^ 0xA5A5A5A5) >>> 0, 7);
        return key.toString(16).toUpperCase().padStart(8, '0');
    }

    if (algo === 'custom') {
        const expr = ($('sbS2kExpr')?.value || '').trim();
        if (!expr) return null;
        try {
            const b0 = (seed >>> 24) & 0xFF, b1 = (seed >>> 16) & 0xFF;
            const b2 = (seed >>>  8) & 0xFF, b3 =  seed         & 0xFF;
            const rotl = s2kRotl;
            const rotr = (n, b) => s2kRotl(n, 32 - b);
            /* new Function keeps the expression out of the local scope */
            const fn = new Function('seed','b0','b1','b2','b3','rotl','rotr',
                                    '"use strict"; return (' + expr + ');');
            const res = fn(seed, b0, b1, b2, b3, rotl, rotr);
            if (typeof res === 'number')
                return (res >>> 0).toString(16).toUpperCase().padStart(8, '0');
            if (Array.isArray(res) || res instanceof Uint8Array)
                return Array.from(res).map(x => hb(x & 0xFF)).join('');
        } catch (e) { return 'ERR: ' + e.message; }
    }
    return null;
}

function s2kUpdate() {
    if (!_sbSeedHex) return;
    const algo      = $('sbS2kAlgo')?.value || 'none';
    const exprInp   = $('sbS2kExpr');
    const keyValEl  = $('sbS2kKeyVal');
    const manualRow = $('sbSaKeyRow');
    const autoBtn   = $('sbAutoUnlockBtn');

    if (exprInp) exprInp.style.display = algo === 'custom' ? '' : 'none';

    if (algo === 'none') {
        if (keyValEl)  keyValEl.style.display  = 'none';
        if (manualRow) manualRow.style.display  = '';
        if (autoBtn)   autoBtn.style.display    = 'none';
        return;
    }

    if (manualRow) manualRow.style.display = 'none';
    const key = s2kCompute(_sbSeedHex);
    if (!key) {
        if (keyValEl) keyValEl.style.display = 'none';
        if (autoBtn)  autoBtn.style.display  = 'none';
        return;
    }
    if (key.startsWith('ERR:')) {
        if (keyValEl) {
            keyValEl.textContent = key;
            keyValEl.style.color = 'var(--red)';
            keyValEl.style.display = '';
        }
        if (autoBtn) autoBtn.style.display = 'none';
    } else {
        if (keyValEl) {
            keyValEl.textContent = 'KEY: ' + key;
            keyValEl.style.color = 'var(--teal)';
            keyValEl.style.display = '';
        }
        if (autoBtn) autoBtn.style.display = '';
    }
}

$('sbS2kAlgo')?.addEventListener('change', s2kUpdate);
$('sbS2kExpr')?.addEventListener('input',  s2kUpdate);

/* Shared: handle a positive SecurityAccess unlock */
function sbSaUnlockSuccess(level) {
    const statusEl = $('sbSaStatus');
    [$('sbSaSeedVal'), $('sbS2kSection'), $('sbSaKeyRow'), $('sbAutoUnlockBtn')]
        .forEach(el => { if (el) el.style.display = 'none'; });
    if (statusEl) {
        statusEl.className = 'sb-sa-status ok';
        statusEl.textContent = '✓ Unlocked — SA 0x' + hb(level);
        statusEl.style.display = '';
    }
    log.ok('Security access 0x' + hb(level) + ' granted');
}

function sbSaUnlockFail(level, nrc) {
    const reason = nrc === 0x35 ? ' — wrong key'
                 : nrc === 0x36 ? ' — attempts exceeded (ECU locked)' : '';
    const statusEl = $('sbSaStatus');
    if (statusEl) {
        statusEl.className = 'sb-sa-status err';
        statusEl.textContent = '✗ NRC 0x' + hb(nrc) + reason;
        statusEl.style.display = '';
    }
    log.error('Key rejected: NRC 0x' + hb(nrc) + reason);
}

/* --- DSC (session open) --- */
$('sbDscBtn')?.addEventListener('click', async () => {
    if (!client.connected) { log.error('Not connected.'); return; }
    const sub    = $('sbDscSel').value;
    const subNum = parseInt(sub, 16);
    const label  = {1:'Default', 2:'Bootloader', 3:'Extended'}[subNum] || '0x'+hb(subNum);
    $('sbDscBtn').disabled = true;
    try {
        const r = await udsReq(0x10, sub, 3000);
        if (r.status === 'positive') log.ok('Session ' + label + ' opened');
        else log.error('DSC NRC 0x' + hb(r.nrc||0) + ' — ' + (NRC_NAMES[r.nrc]||''));
    } catch (e) { log.error('DSC: ' + e.message); }
    $('sbDscBtn').disabled = false;
});

/* --- SA: get seed --- */
$('sbSaSeedBtn')?.addEventListener('click', async () => {
    if (!client.connected) { log.error('Not connected.'); return; }
    const level = parseInt($('sbSaSel').value, 16);
    /* reset UI */
    [$('sbSaSeedVal'), $('sbS2kSection'), $('sbSaKeyRow'),
     $('sbAutoUnlockBtn'), $('sbSaStatus')].forEach(el => {
        if (el) el.style.display = 'none';
    });
    _sbSeedHex = '';
    $('sbSaSeedBtn').disabled = true;
    try {
        const r = await udsReq(0x27, hb(level), 2000);
        if (r.status !== 'positive') {
            log.error('Seed NRC 0x' + hb(r.nrc||0) + ' — ' + (NRC_NAMES[r.nrc]||''));
            $('sbSaSeedBtn').disabled = false;
            return;
        }
        _sbSeedHex = (r.data||'').replace(/\s/g,'').slice(2); /* strip level echo */
        if (_sbSeedHex && _sbSeedHex.replace(/0/g,'') !== '') {
            const seedEl = $('sbSaSeedVal');
            if (seedEl) { seedEl.textContent = 'SEED: ' + _sbSeedHex; seedEl.style.display = ''; }
            const s2kSec = $('sbS2kSection');
            if (s2kSec) s2kSec.style.display = '';
            if ($('sbSaKey')) $('sbSaKey').value = '';
            s2kUpdate(); /* auto-compute key if algo already selected */
        } else {
            /* seed = 0x00…00 means already unlocked */
            sbSaUnlockSuccess(level);
        }
    } catch (e) { log.error('Seed: ' + e.message); }
    $('sbSaSeedBtn').disabled = false;
});

/* --- SA: manual key unlock --- */
$('sbSaKeyBtn')?.addEventListener('click', async () => {
    if (!client.connected) { log.error('Not connected.'); return; }
    const level = parseInt($('sbSaSel').value, 16);
    const key   = ($('sbSaKey')?.value||'').replace(/[\s\-]/g,'').toUpperCase();
    if (!key) { log.error('Enter the computed key first.'); return; }
    $('sbSaKeyBtn').disabled = true;
    try {
        const r = await udsReq(0x27, hb(level + 1) + key, 2000);
        if (r.status === 'positive') sbSaUnlockSuccess(level);
        else sbSaUnlockFail(level, r.nrc||0);
    } catch (e) { log.error('Unlock: ' + e.message); }
    $('sbSaKeyBtn').disabled = false;
});

/* --- SA: auto unlock (S2K computed) --- */
$('sbAutoUnlockBtn')?.addEventListener('click', async () => {
    if (!client.connected) { log.error('Not connected.'); return; }
    const level = parseInt($('sbSaSel').value, 16);
    const key   = s2kCompute(_sbSeedHex);
    if (!key || key.startsWith('ERR:')) { log.error('Cannot compute key: ' + (key||'no algo')); return; }
    $('sbAutoUnlockBtn').disabled = true;
    try {
        const r = await udsReq(0x27, hb(level + 1) + key, 2000);
        if (r.status === 'positive') sbSaUnlockSuccess(level);
        else sbSaUnlockFail(level, r.nrc||0);
    } catch (e) { log.error('Auto-unlock: ' + e.message); }
    $('sbAutoUnlockBtn').disabled = false;
});

/* =====================================================================
   Raw UDS send — ad-hoc requests from Overview
   ===================================================================== */
const NRC_NAMES = {
    0x10:'generalReject', 0x11:'serviceNotSupported', 0x12:'subFunctionNotSupported',
    0x13:'incorrectMessageLength', 0x14:'responseTooLong', 0x21:'busyRepeatRequest',
    0x22:'conditionsNotCorrect', 0x24:'requestSequenceError', 0x25:'noResponseFromSubnet',
    0x26:'failurePreventsExecution', 0x31:'requestOutOfRange', 0x33:'securityAccessDenied',
    0x35:'invalidKey', 0x36:'exceededAttempts', 0x37:'requiredTimeDelayNotExpired',
    0x70:'uploadDownloadNotAccepted', 0x71:'transferDataSuspended', 0x72:'generalProgrammingFailure',
    0x73:'wrongBlockSequenceCounter', 0x78:'responsePending', 0x7E:'notSupportedInSession',
    0x7F:'serviceNotSupportedInSession',
};

async function sendRawUds() {
    if (!client.connected) { log.error('Not connected.'); return; }
    const sidStr = ($('rawSid').value || '').trim().replace(/^0x/i, '');
    const sid = parseInt(sidStr, 16);
    if (isNaN(sid) || sid < 0 || sid > 0xFF) { log.error('Invalid SID.'); return; }
    const data = ($('rawData').value || '').replace(/\s/g, '');
    const timeout = parseInt($('rawTimeout').value) || 2000;

    const resp = $('rawResp');
    resp.style.display = '';
    resp.style.color = 'var(--fg-2)';
    resp.textContent = 'Sending…';
    $('rawSendBtn').disabled = true;

    try {
        const r = await udsReq(sid, data, timeout);
        if (r.status === 'positive') {
            resp.textContent = '✓  SID 0x' + hb(sid + 0x40) + '  →  ' + (fmtHex(r.data || '') || '(no data)') + '  [' + r.elapsed_ms + ' ms]';
            resp.style.color = 'var(--green)';
        } else if (r.status === 'negative') {
            const name = NRC_NAMES[r.nrc] || 'unknown';
            resp.textContent = '✗  NRC 0x' + hb(r.nrc || 0) + '  —  ' + name;
            resp.style.color = 'var(--red)';
        } else {
            resp.textContent = '⚠  ' + r.status;
            resp.style.color = 'var(--orange)';
        }
    } catch (e) {
        resp.textContent = '✗  ' + e.message;
        resp.style.color = 'var(--red)';
    }
    $('rawSendBtn').disabled = false;
}

$('rawSendBtn')?.addEventListener('click', sendRawUds);
$('rawData')?.addEventListener('keydown', e => { if (e.key === 'Enter') sendRawUds(); });

/* =====================================================================
   ECU Discovery — scan common UDS addresses for live ECUs
   ===================================================================== */
const SCAN_TARGETS=[
    {tx:'0x7E0',rx:'0x7E8',label:'Engine (OBD-II)'},
    {tx:'0x7E1',rx:'0x7E9',label:'Transmission'},
    {tx:'0x7E2',rx:'0x7EA',label:'ABS / ESP'},
    {tx:'0x7E3',rx:'0x7EB',label:'ECU #3'},
    {tx:'0x7E4',rx:'0x7EC',label:'ECU #4'},
    {tx:'0x7E5',rx:'0x7ED',label:'ECU #5'},
    {tx:'0x7E6',rx:'0x7EE',label:'ECU #6'},
    {tx:'0x7E7',rx:'0x7EF',label:'ECU #7'},
    {tx:'0x760',rx:'0x768',label:'Body Control'},
    {tx:'0x744',rx:'0x74C',label:'Gateway/BCM'},
    {tx:'0x713',rx:'0x71B',label:'Airbag'},
    {tx:'0x71C',rx:'0x724',label:'Instrument'},
];

async function scanEcus(){
    if(!client.connected){log.error('Not connected.');return}
    const card=$('scanResultCard'),list=$('scanResultList');
    if(!card||!list)return;

    /* Use user presets if available; fall back to built-in address list */
    const targets=_presets.length
        ? _presets.map(p=>({tx:p.tx,rx:p.rx,label:(p.icon?p.icon+' ':'')+p.name}))
        : SCAN_TARGETS;

    card.hidden=false;
    const found=[];
    for(let i=0;i<targets.length;i++){
        const t=targets[i];
        list.innerHTML='<div class="empty-state">Scanning '+esc(t.label)+' ('+t.tx+')… '+(i+1)+'/'+targets.length+'</div>';
        try{
            const r=await client.request({type:'uds_request',tx_id:t.tx,rx_id:t.rx,
                sid:'0x10',data:'01',timeout_ms:500},(500+1500));
            if(r.status==='positive'||r.status==='negative')
                found.push({...t,nrc:r.status==='negative'?r.nrc:null,positive:r.status==='positive'});
        }catch{}
    }
    if(!found.length){list.innerHTML='<div class="empty-state">No ECUs responded.</div>';return}
    list.innerHTML=found.map(f=>{
        const pill=f.positive?
            '<span class="dtc-status active" style="background:rgba(48,209,88,.18);color:var(--green)">✓ Responded</span>':
            '<span class="dtc-status">NRC 0x'+hb(f.nrc||0)+'</span>';
        const cid='ecu_'+f.tx.replace(/[^a-z0-9]/gi,'');
        return`<div class="dtc-row">
            <div class="dtc-code" style="color:var(--teal)">${esc(f.label)}</div>
            <div class="dtc-desc">${f.tx} → ${f.rx}</div>
            ${pill}
            <button class="btn btn-ghost" style="font-size:11px;padding:3px 8px"
                data-action="use-scanned-ecu" data-tx="${f.tx}" data-rx="${f.rx}">Use</button>
            <button class="btn btn-ghost" style="font-size:11px;padding:3px 8px"
                data-action="read-ecu-dtc" data-tx="${f.tx}" data-rx="${f.rx}" id="btn_${cid}">📋 DTCs</button>
        </div>
        <div id="${cid}"></div>`}).join('');
    log.ok('Scan done — '+found.length+'/'+targets.length+' responded')
}

/* =====================================================================
   Export live CAN log as Vector .ASC
   ===================================================================== */
function exportASC() {
    if (!FRAMES.length) { log.warn('No frames to export.'); return; }

    const now = new Date();
    /* Vector ASC header */
    const dayNames = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
    const monNames = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    const pad2 = n => String(n).padStart(2,'0');
    const dateHdr = [
        dayNames[now.getDay()], monNames[now.getMonth()], pad2(now.getDate()),
        pad2(now.getHours())+':'+pad2(now.getMinutes())+':'+pad2(now.getSeconds())+'.000',
        now.getFullYear()
    ].join(' ');

    const lines = [
        'date ' + dateHdr,
        'base hex  timestamps absolute',
        'internal events logged',
        '// ESP32 UDS Gateway — ' + FRAMES.length + ' frame(s)',
        '',
    ];

    /* FRAMES is newest-first → reverse to get chronological order */
    const sorted = FRAMES.slice().reverse();
    for (const f of sorted) {
        const ts = parseFloat(f.t || '0').toFixed(6).padStart(13, ' ');
        /* CAN ID: right-padded to 15 chars for alignment */
        const id = (f.id || '000').toUpperCase().padEnd(15, ' ');
        const dir = (f.dir || 'RX').toUpperCase() === 'TX' ? 'Tx' : 'Rx';
        /* Parse data bytes */
        const bytes = (f.data || '').replace(/\s+/g,' ').trim().split(' ').filter(Boolean);
        const dlc = Math.min(bytes.length, 8);
        /* Pad to 8 bytes as required by most ASC readers */
        while (bytes.length < 8) bytes.push('00');
        lines.push(`${ts} 1  ${id} ${dir}   d ${dlc} ${bytes.slice(0,8).join(' ').toUpperCase()}`);
    }

    const blob = new Blob([lines.join('\r\n')], { type: 'text/plain;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'can_' + now.toISOString().slice(0,19).replace(/[T:]/g,'-') + '.asc';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
    log.ok('Exported ' + FRAMES.length + ' frames → ' + a.download);
}

/* =====================================================================
   Memory / DID Scanner
   ===================================================================== */
let _memResults = [];   // [{did, didStr, name, raw, decoded, nrc, nrcName}]
let _memScanStop = false;
let _memLastBytes = null; // last raw bytes from ReadMemoryByAddress

const MEM_PRESETS = {
    id:    {start: 0xF100, end: 0xF1FF},
    obd:   {start: 0x0001, end: 0x00FF},
    vw:    {start: 0xD900, end: 0xD9FF},
    full1: {start: 0x0000, end: 0x01FF},
};

$('memPreset')?.addEventListener('change', () => {
    $('memCustomRange').style.display =
        $('memPreset').value === 'custom' ? '' : 'none';
});

async function startDidScan() {
    if (!client.connected) { log.error('Not connected.'); return; }
    const preset = $('memPreset')?.value || 'id';
    let startDid, endDid;
    if (MEM_PRESETS[preset]) {
        startDid = MEM_PRESETS[preset].start;
        endDid   = MEM_PRESETS[preset].end;
    } else {
        startDid = parseInt(($('memRangeStart')?.value || 'F100').replace(/^0x/i,''), 16);
        endDid   = parseInt(($('memRangeEnd')?.value   || 'F1FF').replace(/^0x/i,''), 16);
    }
    if (isNaN(startDid)||isNaN(endDid)||startDid>endDid) {
        log.error('Invalid DID range.'); return; }

    const timeout = parseInt($('memTimeout')?.value) || 500;
    const sessVal = $('memSession')?.value || 'none';

    _memResults = [];
    _memScanStop = false;

    /* UI — switch button to Stop */
    const scanBtn = $('memScanBtn');
    if (scanBtn) { scanBtn.textContent = '⏹ Stop'; scanBtn.dataset.action = 'stop-did-scan'; }
    $('memProgressCard').hidden = false;
    $('memResultCard').hidden = true;
    $('exportMemBtn').disabled = true;

    try {
        /* Optional: open session */
        if (sessVal !== 'none') {
            const sr = await udsReq(0x10, sessVal, 3000);
            if (sr.status !== 'positive')
                log.warn('Session 0x10 ' + sessVal + ' failed — scanning without it');
        }

        const total = endDid - startDid + 1;
        let found = 0;

        for (let did = startDid; did <= endDid; did++) {
            if (_memScanStop) break;

            const pct = Math.floor(((did - startDid) / total) * 100);
            if ($('memBarFill'))  $('memBarFill').style.width  = pct + '%';
            if ($('memCurDid'))   $('memCurDid').textContent   = '0x' + hw(did);
            if ($('memFound'))    $('memFound').textContent     = found;
            if ($('memPct'))      $('memPct').textContent       = pct;
            if ($('memScanTitle'))$('memScanTitle').textContent = 'Reading 0x' + hw(did) + '…';

            try {
                const r = await client.request({
                    type: 'uds_request', tx_id: curIds().tx, rx_id: curIds().rx,
                    sid: '0x22', data: hw(did), timeout_ms: timeout
                }, timeout + 1500);

                if (r.status === 'positive') {
                    /* Response: [SID+0x40][DIDhi][DIDlo][data...] — skip 3 bytes */
                    const rawHex = (r.data || '').replace(/\s/g, '').slice(6);
                    const info   = DID_KNOWN[did];
                    const bytes  = Array.from(h2b(rawHex));
                    let decoded  = null;
                    try { if (info?.decode) decoded = info.decode(bytes); } catch {}
                    _memResults.push({
                        did, didStr: '0x' + hw(did),
                        name: info?.name || '—',
                        raw: fmtHex(rawHex),
                        decoded
                    });
                    found++;
                } else if (r.status === 'negative' && r.nrc === 0x22) {
                    /* conditionsNotCorrect — DID exists, not readable in this session */
                    _memResults.push({
                        did, didStr: '0x' + hw(did),
                        name: DID_KNOWN[did]?.name || '—',
                        raw: null, decoded: null,
                        nrc: 0x22, nrcName: 'conditionsNotCorrect'
                    });
                    found++;
                }
                /* NRC 0x31 requestOutOfRange = DID simply not supported, skip */
            } catch { /* timeout = not implemented */ }
        }

        /* Done */
        if ($('memBarFill'))  $('memBarFill').style.width  = '100%';
        if ($('memPct'))      $('memPct').textContent       = '100';
        if ($('memFound'))    $('memFound').textContent     = found;
        if ($('memScanStage'))$('memScanStage').textContent = _memScanStop ? 'Stopped' : 'Done';
        if ($('memScanTitle'))$('memScanTitle').textContent =
            (found ? found + ' DID(s) found' : 'No DIDs responded') +
            (_memScanStop ? ' (scan stopped)' : ' in range 0x' + hw(startDid) + '–0x' + hw(endDid));

        renderMemResults();
        if (_memResults.length) $('exportMemBtn').disabled = false;
        log.ok('DID scan: ' + found + ' readable DID(s) out of ' + (endDid - startDid + 1));

    } catch (e) { log.error('Scan error: ' + e.message); }

    if (scanBtn) { scanBtn.textContent = '🔍 Start DID Scan'; scanBtn.dataset.action = 'start-did-scan'; }
}

function renderMemResults() {
    const host = $('memResultTable');
    if (!host) return;
    $('memResultCard').hidden = false;
    if (!_memResults.length) {
        host.innerHTML = '<div class="empty-state">No readable DIDs found in this range.</div>';
        return;
    }
    host.innerHTML = _memResults.map(r => {
        const rawCell = r.raw
            ? `<span class="mono" style="color:var(--fg)">${esc(r.raw)}</span>`
            : `<span style="color:var(--orange);font-size:11px">NRC 0x${hb(r.nrc||0)} — ${esc(r.nrcName||'')}</span>`;
        const decCell = r.decoded != null
            ? `<span style="color:var(--green)">${esc(String(r.decoded))}</span>` : '—';
        return `<div class="did-row">
<div class="did-addr">${r.didStr}</div>
<div class="did-name">${esc(r.name)}</div>
<div class="did-raw">${rawCell}</div>
<div class="did-decoded">${decCell}</div>
</div>`;
    }).join('');
}

function exportMemory() {
    if (!_memResults.length) { log.warn('Run a DID scan first.'); return; }
    const fmt  = $('memExportFmt')?.value || 'json';
    const ts   = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    let content, mime, ext;

    if (fmt === 'csv') {
        const rows = ['DID,Name,Raw Data,Decoded,NRC'];
        _memResults.forEach(r => {
            rows.push([
                r.didStr,
                '"' + (r.name  || '').replace(/"/g, '""') + '"',
                '"' + (r.raw   || ('NRC 0x' + hb(r.nrc||0))).replace(/"/g,'""') + '"',
                '"' + (r.decoded != null ? String(r.decoded) : '').replace(/"/g,'""') + '"',
                r.nrc ? '0x' + hb(r.nrc) : '',
            ].join(','));
        });
        content = rows.join('\r\n');
        mime = 'text/csv'; ext = 'csv';
    } else {
        content = JSON.stringify({
            exported: new Date().toISOString(),
            ecu: { tx: curIds().tx, rx: curIds().rx },
            count: _memResults.length,
            dids: _memResults,
        }, null, 2);
        mime = 'application/json'; ext = 'json';
    }

    _triggerDownload(content, mime, 'ecu_memory_' + ts + '.' + ext);
    log.ok('Exported ' + _memResults.length + ' DID(s) → ecu_memory_' + ts + '.' + ext);
}

async function readMemBlock() {
    if (!client.connected) { log.error('Not connected.'); return; }
    const addrStr = ($('memBlockAddr')?.value || '0').replace(/^0x/i, '');
    const addr    = parseInt(addrStr, 16);
    const len     = Math.min(Math.max(1, parseInt($('memBlockLen')?.value) || 256), 4095);
    if (isNaN(addr)) { log.error('Invalid address.'); return; }

    /* SID 0x23 ReadMemoryByAddress — ALFI 0x24: 2-byte size, 4-byte address */
    const addrB = [(addr>>24)&0xFF,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF];
    const lenB  = [(len>>8)&0xFF, len&0xFF];
    const data  = [0x24, ...addrB, ...lenB].map(b => hb(b)).join('');

    try {
        const r = await udsReq(0x23, data, 8000);
        if (r.status !== 'positive') {
            log.error('ReadMemoryByAddress NRC 0x' + hb(r.nrc||0) + ' — ' + (NRC_NAMES[r.nrc]||''));
            return;
        }
        /* Skip SID echo byte (2 hex chars) */
        const rawHex = (r.data || '').replace(/\s/g, '').slice(2);
        _memLastBytes = h2b(rawHex);
        renderHexDump(addr, _memLastBytes);
        $('memHexCard').hidden = false;
        log.ok('Read ' + _memLastBytes.length + ' bytes from 0x' + addr.toString(16).toUpperCase().padStart(8, '0'));
    } catch (e) { log.error('ReadMemoryByAddress failed: ' + e.message); }
}

function renderHexDump(baseAddr, bytes) {
    const host = $('memHexDump');
    if (!host) return;
    const lines = [];
    for (let i = 0; i < bytes.length; i += 16) {
        const chunk = Array.from(bytes.slice(i, Math.min(i + 16, bytes.length)));
        const addr  = (baseAddr + i).toString(16).toUpperCase().padStart(8, '0');
        const hex   = chunk.map(b => hb(b)).join(' ').padEnd(47, ' ');
        const ascii = chunk.map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
        lines.push(addr + '  ' + hex + '  ' + ascii);
    }
    host.textContent = lines.join('\n');
}

function exportHexBin() {
    if (!_memLastBytes || !_memLastBytes.length) { log.warn('Read memory first.'); return; }
    const addr = parseInt(($('memBlockAddr')?.value||'0').replace(/^0x/i,''),16)||0;
    const ts   = new Date().toISOString().slice(0,19).replace(/[T:]/g,'-');
    const name = 'mem_0x' + addr.toString(16).toUpperCase().padStart(8,'0') + '_' + ts + '.bin';
    const blob = new Blob([_memLastBytes], {type:'application/octet-stream'});
    _triggerDownload(blob, 'application/octet-stream', name);
    log.ok('Saved ' + _memLastBytes.length + ' bytes → ' + name);
}

function _triggerDownload(content, mime, filename) {
    const blob   = content instanceof Blob ? content : new Blob([content], {type: mime});
    const a      = document.createElement('a');
    a.href       = URL.createObjectURL(blob);
    a.download   = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
}

/* =====================================================================
   ECU Memory Backup — SID 0x35 RequestUpload → 0x36 × N → 0x37
   ===================================================================== */
let _uplBuffer  = null;
let _uplBaseAddr = 0;

$('uplAddrPreset')?.addEventListener('change', () => {
    const v = $('uplAddrPreset').value;
    if (v && $('uplBlockAddr')) $('uplBlockAddr').value = v;
});

function uplLog(msg, kind) {
    $('memUplLogCard').hidden = false;
    const host = $('memUplLog');
    if (!host) return;
    const row = document.createElement('div');
    row.className = 'log-row';
    if (kind === 'ok')   row.style.color = 'var(--green)';
    if (kind === 'err')  row.style.color = 'var(--red)';
    if (kind === 'warn') row.style.color = 'var(--orange)';
    row.textContent = '[' + new Date().toISOString().slice(11, 23) + '] ' + msg;
    host.insertBefore(row, host.firstChild);
}

async function startMemUpload() {
    if (!client.connected) { log.error('Not connected.'); return; }
    const addrStr = ($('uplBlockAddr')?.value || '0').replace(/^0x/i, '');
    const addr    = parseInt(addrStr, 16);
    const size    = Math.min(Math.max(1, parseInt($('uplBlockSize')?.value) || 4096), 65535);
    if (isNaN(addr)) { log.error('Invalid address.'); return; }

    const sessVal = $('uplSession')?.value  || 'none';
    const secVal  = $('uplSecLevel')?.value || 'none';
    const manKey  = ($('uplManualKey')?.value || '').replace(/[\s\-]/g, '').toUpperCase();

    const btn = $('memUplBtn');
    btn.disabled = true; btn.textContent = 'Uploading…';
    $('memUplProgressCard').hidden = false;
    $('memUplLogCard').hidden      = false;
    $('memUplSaveBtn').disabled    = true;
    $('memUplStage').textContent   = 'Starting';
    $('memUplTitle').textContent   = 'Initializing…';
    $('memUplBarFill').style.width = '0%';
    $('memUplBytes').textContent   = '0';
    $('memUplTotal').textContent   = size;
    $('memUplPct').textContent     = '0';
    $('memUplBlock').textContent   = '—';

    _uplBuffer = null;
    _uplBaseAddr = addr;

    const req = (sid, data, t) => client.request(
        {type:'uds_request', tx_id:curIds().tx, rx_id:curIds().rx,
         sid, data: data||'', timeout_ms: t||3000},
        (t||3000) + 2000
    );

    try {
        /* 1. Open session */
        if (sessVal !== 'none') {
            uplLog('Opening session 0x10 ' + sessVal + '…');
            const sr = await req('0x10', sessVal, 3000);
            if (sr.status !== 'positive')
                uplLog('Session 0x10 ' + sessVal + ' rejected — continuing', 'warn');
            else
                uplLog('Session OK', 'ok');
        }

        /* 2. Security access */
        if (secVal !== 'none') {
            const secLevel = parseInt(secVal, 16);
            uplLog('Requesting seed (0x27 sub 0x' + hb(secLevel) + ')…');
            const seedR = await req('0x27', hb(secLevel), 2000);
            if (seedR.status !== 'positive') {
                uplLog('Seed request failed: nrc=0x' + hb(seedR.nrc||0) + ' — skipping security', 'warn');
            } else {
                const seedHex = (seedR.data || '').replace(/\s/g, '').slice(2);
                uplLog('Seed: ' + (seedHex || '(empty — already unlocked)'));
                if (seedHex && seedHex !== '0000' && seedHex !== '00000000') {
                    const keyHex = manKey || '00'.repeat(seedHex.length / 2);
                    if (!manKey) uplLog('No key — trying zero-key (may fail with NRC 0x35)', 'warn');
                    const keyR = await req('0x27', hb(secLevel + 1) + keyHex, 2000);
                    if (keyR.status !== 'positive') {
                        uplLog('Key rejected: nrc=0x' + hb(keyR.nrc||0) +
                               (keyR.nrc===0x35?' (wrong key)':keyR.nrc===0x36?' (attempts exceeded)':''), 'err');
                        throw new Error('SecurityAccess failed — enter the correct key in Manual Key and retry');
                    }
                    uplLog('Security access granted', 'ok');
                } else {
                    uplLog('Seed is zero — ECU already unlocked', 'ok');
                }
            }
        }

        /* 3. RequestUpload (SID 0x35)
               dataFormatIdentifier : 0x00 (no compression, no encryption)
               ALFI 0x24            : 2-byte size, 4-byte address              */
        $('memUplStage').textContent = 'RequestUpload';
        $('memUplTitle').textContent = 'Sending 0x35…';
        uplLog('RequestUpload 0x35 — addr 0x' +
               addr.toString(16).toUpperCase().padStart(8,'0') +
               '  size ' + size + ' B');

        const addrB  = [(addr>>24)&0xFF,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF];
        const sizeB  = [(size>>8)&0xFF, size&0xFF];
        const uplPay = [0x00, 0x24, ...addrB, ...sizeB].map(b=>hb(b)).join('');
        const uplR   = await req('0x35', uplPay, 5000);

        if (uplR.status !== 'positive') {
            uplLog('0x35 RequestUpload failed: nrc=0x' + hb(uplR.nrc||0) +
                   ' — ' + (NRC_NAMES[uplR.nrc]||''), 'err');
            throw new Error('RequestUpload NRC 0x' + hb(uplR.nrc||0));
        }

        /* Parse maxNumberOfBlockLength from response
           After SID strip: [lengthFormatId(1)] [maxBlockLen(n bytes)]
           lengthFormatId high nibble = n (number of following bytes)           */
        const uplResp = h2b((uplR.data||'').replace(/\s/g,''));
        const lfi     = uplResp[0] || 0;
        const lfiLen  = (lfi >> 4) & 0x0F;
        let mbl = 0;
        for (let i = 0; i < lfiLen && i + 1 < uplResp.length; i++)
            mbl = (mbl << 8) | uplResp[1 + i];
        /* mbl includes SID (1) + BSC (1) — subtract to get net data bytes */
        const chunkSize = Math.max(1, (mbl || 130) - 2);
        uplLog('MaxBlockLen=' + mbl + ' → data per block=' + chunkSize + ' B');

        /* 4. TransferData loop (SID 0x36, upload direction)
               Tester sends  : [0x36][BSC]
               ECU responds  : [0x76][BSC][data...]                             */
        $('memUplStage').textContent = 'Downloading';
        const collected = [];
        let bsc = 1, received = 0;

        while (received < size) {
            $('memUplBlock').textContent = bsc;

            const tdR = await req('0x36', hb(bsc), 10000);
            if (tdR.status !== 'positive') {
                uplLog('TransferData block ' + bsc +
                       ' failed: nrc=0x' + hb(tdR.nrc||0), 'err');
                throw new Error('TransferData block ' + bsc + ' NRC 0x' + hb(tdR.nrc||0));
            }

            /* Skip BSC echo (first 2 hex chars = 1 byte) */
            const blockHex   = (tdR.data||'').replace(/\s/g,'').slice(2);
            const blockBytes = h2b(blockHex);
            collected.push(...blockBytes);
            received += blockBytes.length;

            const pct = Math.min(100, Math.floor((received / size) * 100));
            $('memUplBarFill').style.width = pct + '%';
            $('memUplBytes').textContent   = received;
            $('memUplPct').textContent     = pct;
            $('memUplTitle').textContent   = 'Received ' + received + ' / ' + size + ' bytes…';

            bsc = (bsc % 0xFF) + 1;
        }

        /* 5. RequestTransferExit (SID 0x37) */
        $('memUplStage').textContent = 'Exit';
        uplLog('RequestTransferExit 0x37…');
        const teR = await req('0x37', '', 5000);
        if (teR.status !== 'positive')
            uplLog('0x37 failed: nrc=0x' + hb(teR.nrc||0) + ' (data still saved)', 'warn');
        else
            uplLog('Transfer exit OK', 'ok');

        /* Done */
        _uplBuffer = new Uint8Array(collected.slice(0, size));
        $('memUplStage').textContent   = 'Done';
        $('memUplTitle').textContent   = '✓ ' + _uplBuffer.length + ' bytes received';
        $('memUplBarFill').style.width = '100%';
        $('memUplSaveBtn').disabled    = false;
        uplLog('Complete — ' + _uplBuffer.length + ' bytes from 0x' +
               addr.toString(16).toUpperCase().padStart(8,'0'), 'ok');
        log.ok('Memory upload: ' + _uplBuffer.length + ' bytes');

    } catch (e) {
        $('memUplStage').textContent = 'Error';
        $('memUplTitle').textContent = '✗ ' + e.message;
        uplLog('Aborted: ' + e.message, 'err');
        log.error('Memory upload: ' + e.message);
    }

    btn.disabled = false; btn.textContent = '📤 Start Upload';
}

function saveUplBin() {
    if (!_uplBuffer || !_uplBuffer.length) { log.warn('No data to save.'); return; }
    const ts   = new Date().toISOString().slice(0,19).replace(/[T:]/g,'-');
    const name = 'ecu_upload_0x' +
                 _uplBaseAddr.toString(16).toUpperCase().padStart(8,'0') +
                 '_' + ts + '.bin';
    _triggerDownload(new Blob([_uplBuffer], {type:'application/octet-stream'}),
                     'application/octet-stream', name);
    log.ok('Saved ' + _uplBuffer.length + ' B → ' + name);
}

/* ─────────────────────────────────────────────────────────────────────────
   Hex Terminal — invio comandi UDS raw in formato hex con cronologia
   ──────────────────────────────────────────────────────────────────────── */
{
    const termLog      = $('termLog');
    const termInput    = $('termInput');
    const termSendBtn  = $('termSendBtn');
    const termClearBtn = $('termClearBtn');
    const termEcuLbl   = $('termEcuLabel');

    let _tHist    = [];   /* cronologia comandi inviati */
    let _tHistIdx = -1;   /* indice di navigazione (−1 = input corrente) */
    let _tCurrent = '';   /* testo salvato prima della navigazione */

    function _ts() {
        const d = new Date();
        return d.toLocaleTimeString('en-GB', {hour12:false,hour:'2-digit',minute:'2-digit',second:'2-digit'})
               + '.' + d.getMilliseconds().toString().padStart(3,'0');
    }

    function _append(html) {
        if (!termLog) return;
        const el = document.createElement('div');
        el.innerHTML = html;
        termLog.appendChild(el);
        termLog.scrollTop = termLog.scrollHeight;
    }

    function _fmt(hex) {
        return (hex || '').replace(/\s/g,'').toUpperCase().match(/.{1,2}/g)?.join(' ') || '';
    }

    function termSystem(msg) {
        _append(`<div class="term-entry system">${esc(msg)}</div>`);
    }

    function _termTx(hexStr) {
        _append(`<div class="term-entry"><span class="term-ts">${_ts()}</span><span class="term-dir tx">TX</span><span class="term-hex">${_fmt(hexStr)}</span></div>`);
    }

    function _termRx(r, sid) {
        let cls, display, meta;
        if (r.status === 'positive') {
            cls     = 'rx-ok';
            const d = (r.data || '').replace(/\s/g,'');
            display = _fmt(hb((sid + 0x40) & 0xFF) + d);
            meta    = '+' + r.elapsed_ms + ' ms';
        } else if (r.status === 'negative') {
            cls     = 'rx-err';
            display = _fmt('7F' + hb(sid & 0xFF) + hb(r.nrc || 0));
            meta    = 'NRC 0x' + hb(r.nrc || 0) + ' — ' + (NRC_NAMES[r.nrc] || 'unknown');
        } else if (r.status === 'timeout') {
            cls     = 'rx-timeout';
            display = '—';
            meta    = 'timeout';
        } else {
            cls     = 'rx-err';
            display = '—';
            meta    = r.status || 'error';
        }
        _append(`<div class="term-entry"><span class="term-ts">${_ts()}</span><span class="term-dir ${cls}">RX</span><span class="term-hex">${display}</span><span class="term-meta">${esc(meta)}</span></div>`);
    }

    async function termSend() {
        if (!termInput) return;
        const raw = termInput.value.replace(/\s/g,'').toUpperCase();
        if (!raw) return;
        if (!/^[0-9A-F]+$/.test(raw))   { log.error('Terminal: caratteri hex non validi'); return; }
        if (raw.length % 2 !== 0)        { log.error('Terminal: numero di cifre hex dispari'); return; }
        if (raw.length < 2)              { log.error('Terminal: inserire almeno 1 byte (SID)'); return; }

        const prev = termInput.value.trim();
        if (prev && _tHist[0] !== prev) { _tHist.unshift(prev); if (_tHist.length > 100) _tHist.pop(); }
        _tHistIdx = -1;  _tCurrent = '';
        termInput.value = '';

        const sid  = parseInt(raw.slice(0,2), 16);
        const data = raw.slice(2);
        _termTx(raw);

        if (!client.connected) { termSystem('Gateway non connesso'); return; }
        try {
            const {tx, rx} = curIds();
            const r = await client.request({
                type:'uds_request', tx_id:tx, rx_id:rx,
                sid:'0x' + hb(sid), data, timeout_ms:2000
            }, 4000);
            _termRx(r, sid);
        } catch(e) { termSystem('Errore: ' + e.message); }
    }

    /* Auto-format input → "XX XX XX" mentre si digita */
    if (termInput) {
        termInput.addEventListener('input', () => {
            const cur = termInput.selectionStart;
            const hex = termInput.value.replace(/[^0-9a-fA-F]/g,'').slice(0,128).toUpperCase();
            const fmt = hex.match(/.{1,2}/g)?.join(' ') || '';
            if (termInput.value === fmt) return;
            const hexBefore = termInput.value.slice(0, cur).replace(/[^0-9a-fA-F]/g,'').length;
            termInput.value = fmt;
            let cnt = 0, pos = fmt.length;
            for (let i = 0; i < fmt.length; i++) {
                if (fmt[i] !== ' ') cnt++;
                if (cnt === hexBefore) { pos = i + 1; break; }
            }
            termInput.setSelectionRange(pos, pos);
        });

        termInput.addEventListener('keydown', e => {
            if (e.key === 'Enter')     { e.preventDefault(); termSend(); return; }
            if (e.key === 'ArrowUp')   {
                e.preventDefault();
                if (_tHistIdx === -1) _tCurrent = termInput.value;
                if (_tHistIdx < _tHist.length - 1) { _tHistIdx++; termInput.value = _tHist[_tHistIdx]; }
                return;
            }
            if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (_tHistIdx > 0)        { _tHistIdx--; termInput.value = _tHist[_tHistIdx]; }
                else if (_tHistIdx === 0) { _tHistIdx = -1; termInput.value = _tCurrent; }
                return;
            }
            if (e.key === 'Escape') { termInput.value = ''; _tHistIdx = -1; }
        });
    }

    termSendBtn?.addEventListener('click', termSend);
    termClearBtn?.addEventListener('click', () => {
        if (termLog) termLog.innerHTML = '';
        termSystem('Terminale ripulito');
    });

    /* Etichetta ECU nella toolbar del terminale */
    function _updateTermLabel() {
        if (!termEcuLbl) return;
        const {tx, rx} = curIds();
        termEcuLbl.textContent = `TX ${tx} → RX ${rx}`;
    }
    $('sbCustomApply')?.addEventListener('click', _updateTermLabel);
    setInterval(_updateTermLabel, 2000);
    _updateTermLabel();

    /* Focus sull'input quando si attiva il tab Terminal */
    $$('.app-console .tb-tab').forEach(t => {
        if (t.dataset.view === 'terminal')
            t.addEventListener('click', () => setTimeout(() => termInput?.focus(), 60));
    });
}

})();