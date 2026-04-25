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
  if($('devName'))   $('devName').value   =cachedDevName;
  if($('sbDevName')) $('sbDevName').textContent=cachedDevName||'—';
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

async function saveGateway(){const b={device:{name:$('devName').value.trim()}};try{await API.saveConfig(b);log.ok('Saved.')}catch(e){log.error('Save failed: '+e.message)}}
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
['armBadge','armBadgeFlash'].forEach(id=>{const b=$(id);if(!b)return;b.classList.toggle('armed',armed);b.innerHTML=`<span class="ab-dot"></span>${armed?'Armed':(id==='armBadgeFlash'?'Press arm button on device':'Disarmed')}`});
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
else if(a==='clear-live'){FRAMES.length=0;renderFrames()}
else if(a==='toggle-live'){livePaused=!livePaused;btn.textContent=livePaused?'▶ Resume':'⏸ Pause'}
else if(a==='clear-dtc'){if(!confirm('Clear all stored DTCs on the ECU?'))return;try{await clearDTCs();log.ok('DTCs cleared');refreshVehicle()}catch(e){log.error('Clear failed: '+e.message)}}
else if(a==='read-vehicle')refreshVehicle();
else if(a==='cancel-flash')log.info('Cancel not implemented yet')}catch(err){log.error(a+' failed: '+err.message)}});

loadSetupConfig();

/* WebSocket sempre same-origin sull'ESP32. Override solo via ?ws=... per debug. */
const WS_URL=(new URL(window.location.href).searchParams.get('ws'))||
             `${location.protocol==='https:'?'wss':'ws'}://${location.host||'localhost'}/ws`;

class GatewayClient{
constructor(u){this.url=u;this.ws=null;this.pending=new Map();this.listeners=new Set();this.connected=false;this.reconnectDelay=1000;this._reconnectTimer=null}
connect(){try{this.ws=new WebSocket(this.url)}catch{this._scheduleReconnect();return}
this.ws.onopen=()=>{this.connected=true;this.reconnectDelay=1000;updateConnBadge('ok')};
this.ws.onmessage=ev=>{let m;try{m=JSON.parse(ev.data)}catch{return}this._dispatch(m)};
this.ws.onclose=()=>{this.connected=false;updateConnBadge('err');this._failAll('closed');this._scheduleReconnect()};
this.ws.onerror=()=>{}}
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
async function udsReq(sid,data='',t=2000){const{tx,rx}=curIds();return client.request({type:'uds_request',tx_id:tx,rx_id:rx,sid:`0x${hb(sid)}`,data,timeout_ms:t},t+2000)}
async function readDID(d){const r=await udsReq(0x22,hw(d));if(r.status!=='positive')throw new Error('NRC '+(r.nrc||r.status));return h2b(r.data).slice(2)}
async function readDTCs(){const r=await udsReq(0x19,'02FF',3000);if(r.status!=='positive')throw new Error('NRC '+(r.nrc||r.status));const b=h2b(r.data),out=[];for(let i=2;i+3<b.length;i+=4){const[b0,b1,b2,st]=[b[i],b[i+1],b[i+2],b[i+3]];const p=['P','C','B','U'][(b0>>6)&3];out.push({code:p+hb(b0&0x3F)+hb(b1)+hb(b2).slice(0,2),raw:`${hb(b0)}${hb(b1)}${hb(b2)}`,status:st})}return out}
async function clearDTCs(){const r=await udsReq(0x14,'FFFFFF',3000);if(r.status!=='positive')throw new Error('NRC '+(r.nrc||r.status))}
async function startSession(t){const r=await udsReq(0x10,hb(t));if(r.status!=='positive')throw new Error('NRC '+(r.nrc||r.status))}

async function refreshVehicle(){if(!client.connected){log.error('Not connected.');return}
try{await startSession(0x03);
try{$('ovVin').textContent=new TextDecoder().decode(await readDID(0xF190))}catch{}
try{$('ovPart').textContent=new TextDecoder().decode(await readDID(0xF187))}catch{}
try{$('ovSw').textContent=new TextDecoder().decode(await readDID(0xF189))}catch{}
try{renderOvDtcs(await readDTCs())}catch(e){console.warn(e)}
log.ok('Vehicle refreshed')}catch(e){log.error('Refresh failed: '+e.message)}}

function renderOvDtcs(dtcs){const hosts=[$('ovDtcList'),$('dtcEcm')].filter(Boolean);if(!hosts.length)return;
const html=!dtcs.length?'<div class="empty-state">No trouble codes stored.</div>':dtcs.map(d=>{const a=(d.status&1)?'active':'',l=(d.status&1)?'ACTIVE':'STORED';return `<div class="dtc-row"><div class="dtc-code">${d.code}</div><div class="dtc-desc">Raw ${d.raw} · status 0x${hb(d.status)}</div><span class="dtc-status ${a}">${l}</span><button class="btn btn-ghost" style="font-size:11px;padding:3px 8px">Inspect</button></div>`}).join('');
hosts.forEach(h=>h.innerHTML=html)}

const FRAMES=[],MAX_FRAMES=200;let livePaused=false;
function addFrame(f){if(livePaused)return;FRAMES.unshift(f);if(FRAMES.length>MAX_FRAMES)FRAMES.pop();renderFrames()}
function renderFrames(){const host=$('frameLog');if(!host)return;
const flt=($('liveFilter')?.value||'').toUpperCase(),dir=$('liveDir')?.value||'all';
const rows=FRAMES.filter(f=>(dir==='all'||f.dir.toLowerCase()===dir)&&(!flt||f.id.includes(flt)||f.data.includes(flt)));
host.innerHTML=rows.map(f=>`<div class="log-row"><div class="log-time">${f.t}</div><div><span class="log-dir ${f.dir.toLowerCase()}">${f.dir}</span></div><div class="log-id">${f.id}</div><div class="log-data">${f.data}</div><div class="log-meta">${f.info}</div></div>`).join('')}
$('liveFilter')?.addEventListener('input',renderFrames);
$('liveDir')?.addEventListener('change',renderFrames);

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
})();