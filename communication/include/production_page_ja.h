#pragma once

constexpr char productionPageJapanese[]=R"HTML(
<!doctype html>
<html lang="ja">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>水上ボート制御</title>
<style>
body{font:15px system-ui,sans-serif;background:#0d1720;color:#eef;padding:12px;max-width:920px;margin:auto}
h2{margin:8px 0}.card{background:#182735;border-radius:10px;padding:13px;margin:10px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:8px}.row{display:flex;align-items:center;gap:7px;flex-wrap:wrap;margin:7px 0}.status{padding:8px;border-radius:7px;background:#26394a}.ok{color:#7ee787}.warn{color:#ffd166}.bad{color:#ff7b72}.muted{color:#9fb1c1}.active{outline:3px solid #7ee787}button,input,textarea{font:inherit;padding:9px;margin:2px;border-radius:6px;border:1px solid #5c7182}button{background:#f0bd3d;color:#101820;font-weight:650}.danger{background:#ef5350;color:white}.secondary{background:#b8c4cc}input[type=range]{width:min(330px,55vw)}input[type=checkbox]{width:20px;height:20px}textarea{width:95%;height:90px}label.output{display:inline-flex;align-items:center;gap:5px;min-width:105px}pre{white-space:pre-wrap;overflow-wrap:anywhere}small{display:block;color:#b8c7d4;margin-top:5px}
</style>
<h2>水上ボート制御</h2>
<div id="linkBanner" class="status warn">制御側XIAOの状態を確認中です…</div>

<section class="card">
  <b>1. 動作モード</b>
  <div class="row">
    <button id="mode0" onclick="setMode(0)">手動</button>
    <button id="mode1" onclick="setMode(1)">姿勢補助</button>
    <button id="mode2" onclick="setMode(2)">方位保持</button>
    <button id="mode3" onclick="setMode(3)">ウェイポイント自動航行</button>
  </div>
  <div class="row">目標方位 [rad] <input id="heading" value="0" size="8"><button onclick="setHeading()">方位を設定</button></div>
  <small>今のような部分接続試験では「手動」を使用してください。姿勢補助・方位保持にはBNO08X、自動航行にはBNO08X・GNSS・VESC系が必要です。</small>
</section>

<section class="card">
  <b>2. 手動出力（使う出力だけチェック）</b>
  <div class="row"><label class="output"><input id="enL" type="checkbox">左前翼 CH0</label><input id="l" type="range" min="-1" max="1" step=".01" value="0"><span id="lv">0.00</span></div>
  <div class="row"><label class="output"><input id="enR" type="checkbox">右前翼 CH1</label><input id="r" type="range" min="-1" max="1" step=".01" value="0"><span id="rv">0.00</span></div>
  <div class="row"><label class="output"><input id="enY" type="checkbox">後部ヨー CH2</label><input id="y" type="range" min="-1" max="1" step=".01" value="0"><span id="yv">0.00</span></div>
  <div class="row"><label class="output"><input id="enP" type="checkbox" disabled>推進モータ</label><input id="p" type="range" min="0" max="1" step=".01" value="0" disabled><span id="pv">0.00</span></div>
  <div class="row"><button id="manualStart" onclick="startManual()">選択した出力の連続送信を開始</button><button class="secondary" onclick="manualAllOff()">手動出力をすべてOFF</button></div>
  <div id="manualState" class="status muted">手動出力は未選択です。</div>
  <small>未選択のサーボはPCA9685 Full OFFです。VESC応答がない間は推進を選択できず、D10リレーはLOWのままです。</small>
</section>

<section class="card">
  <b>3. 安全状態</b>
  <div class="row">
    <button onclick="sendSafety('arm')">ARM（出力準備）</button>
    <button onclick="sendSafety('start')">START（動作開始）</button>
    <button onclick="sendSafety('stop')">STOP（通常停止）</button>
    <button onclick="sendSafety('disarm')">DISARM</button>
    <button class="danger" onclick="sendSafety('estop')">緊急停止</button>
    <button class="secondary" onclick="sendSafety('clear_estop')">緊急停止を解除</button>
  </div>
  <small>手動試験の順序：手動モード → 出力を選択 → 連続送信開始 → ARM → START。停止時は全サーボFull OFF、VESC Duty 0、D10 LOWです。</small>
</section>

<section class="card">
  <b>現在の状態</b>
  <div id="summary" class="grid"></div>
  <div id="notice" class="status muted">操作結果がここに表示されます。</div>
  <details><summary>詳細データ（JSON）</summary><pre id="rawJson">受信待ち</pre></details>
</section>

<section class="card">
  <b>ウェイポイント</b>
  <textarea id="points" placeholder="36.000000,136.000000&#10;36.000100,136.000100"></textarea>
  <div class="row">到達半径 [m] <input id="radius" value="1.5" size="6"><button onclick="setRoute()">DISARM中に経路を設定</button></div>
</section>

<script>
const el=id=>document.getElementById(id);
const stateName=['起動中','DISARMED（停止）','ARMED（開始待ち）','RUNNING（動作中）','緊急停止','FAULT（異常停止）'];
const modeName=['手動','姿勢補助','方位保持','ウェイポイント自動航行'];
const reasonName=['なし','通常停止','緊急停止','CoreS3通信途絶','手動指令タイムアウト','IMU無効','IMU期限切れ','ToF無効','ToF期限切れ','GNSS無効','GNSS期限切れ','VESC fault','数値異常','最終点到達','電源監視無効','電源監視期限切れ','低電圧','過電流','モータ拘束','危険姿勢','VESC期限切れ','キャビテーション疑い'];
let manualContinuous=false,lastStatus=null,postingManual=false;
function selectedMask(){return (el('enL').checked?1:0)|(el('enR').checked?2:0)|(el('enY').checked?4:0)|(el('enP').checked?8:0)}
function values(){return {l:el('l').value,r:el('r').value,y:el('y').value,p:el('p').value}}
function updateValueLabels(){const v=values();el('lv').textContent=(+v.l).toFixed(2);el('rv').textContent=(+v.r).toFixed(2);el('yv').textContent=(+v.y).toFixed(2);el('pv').textContent=(+v.p).toFixed(2)}
async function post(url,silent=false){try{const response=await fetch(url,{method:'POST'});const text=await response.text();if(!silent)el('notice').textContent=response.ok?'指令を受け付けました。制御側の状態を確認してください。':'指令を受け付けられませんでした: '+text;return response.ok}catch(error){if(!silent)el('notice').textContent='通信に失敗しました: '+error;return false}}
function setMode(mode){return post('/api/competition/mode?mode='+mode)}
function setHeading(){return post('/api/competition/heading?target_yaw_rad='+encodeURIComponent(el('heading').value))}
function sendSafety(action){return post('/api/competition/safety?action='+action)}
async function sendManual(silent=true){if(postingManual)return;postingManual=true;const v=values(),mask=selectedMask();const url='/api/competition/manual?left_front_wing='+v.l+'&right_front_wing='+v.r+'&rear_yaw='+v.y+'&propulsion='+v.p+'&enabled_mask='+mask;await post(url,silent);postingManual=false}
function startManual(){if(!selectedMask()){el('notice').textContent='動かす出力を1つ以上チェックしてください。';return}manualContinuous=true;el('manualStart').classList.add('active');sendManual(false);renderManualState()}
function manualAllOff(){manualContinuous=false;for(const id of ['enL','enR','enY','enP'])el(id).checked=false;for(const id of ['l','r','y','p'])el(id).value=0;updateValueLabels();el('manualStart').classList.remove('active');sendManual(false);renderManualState()}
function setRoute(){const route=encodeURIComponent(el('points').value.trim().split(/\n+/).join(';'));return post('/api/waypoints?reach_radius_m='+encodeURIComponent(el('radius').value)+'&points='+route)}
function renderManualState(){const mask=selectedMask(),names=[];if(mask&1)names.push('左前翼');if(mask&2)names.push('右前翼');if(mask&4)names.push('後部ヨー');if(mask&8)names.push('推進');el('manualState').textContent=manualContinuous?'連続送信中: '+names.join('・'):(names.length?'選択中（まだ連続送信していません）: '+names.join('・'):'手動出力は未選択です。')}
function card(label,value,klass=''){return '<div class="status '+klass+'"><b>'+label+'</b><br>'+value+'</div>'}
function renderStatus(j){
  lastStatus=j;el('rawJson').textContent=JSON.stringify(j,null,2);
  if(j.connected){el('linkBanner').className='status ok';el('linkBanner').textContent='制御側XIAOとの通信は正常です（最終受信 '+j.age_ms+' ms前）'}
  else if(j.ever_received){el('linkBanner').className='status bad';el('linkBanner').textContent='制御側XIAOとの通信が途絶しました（最終受信 '+j.age_ms+' ms前）'}
  else{el('linkBanner').className='status bad';el('linkBanner').textContent='制御側XIAOからデータを一度も受信していません。UART配線（CoreS3 GPIO8←XIAO D7、GPIO9→D6、GND共通）を確認してください。'}
  const motorReady=!!j.motor.valid&&j.motor.fault===0;el('enP').disabled=!motorReady;el('p').disabled=!motorReady;if(!motorReady){el('enP').checked=false;el('p').value=0;updateValueLabels()}
  const pcaReady=!!j.actuators.pca_ready,manualReady=j.connected&&pcaReady;
  el('summary').innerHTML=card('安全状態',stateName[j.control.safety]||('不明 '+j.control.safety),j.control.safety===3?'ok':(j.control.safety>=4?'bad':'warn'))+
    card('モード',modeName[j.control.mode]||('不明 '+j.control.mode))+
    card('手動サーボ試験',manualReady?'可能（PCA9685・通信 正常）':'不可（PCA9685または通信を確認）',manualReady?'ok':'bad')+
    card('BNO08X',j.attitude.valid?'有効':'未接続または無効',j.attitude.valid?'ok':'warn')+
    card('ToF',j.height.valid?'有効 '+Number(j.height.distance_m).toFixed(3)+' m':'未接続または無効',j.height.valid?'ok':'warn')+
    card('GNSS',j.gnss.valid?'測位有効':'未接続または未測位',j.gnss.valid?'ok':'warn')+
    card('INA226',j.power.valid?'有効 '+Number(j.power.voltage_v).toFixed(2)+' V':'未接続または無効',j.power.valid?'ok':'warn')+
    card('VESC',motorReady?'有効 '+Number(j.motor.erpm).toFixed(0)+' ERPM':'未接続・無効・fault',motorReady?'ok':'warn')+
    card('サーボ実出力','左 '+j.actuators.left_us+' / 右 '+j.actuators.right_us+' / 後 '+j.actuators.rear_us+' µs')+
    card('推進安全リレー',j.actuators.motor_relay_enabled?'D10 HIGH（接続）':'D10 LOW（切断）',j.actuators.motor_relay_enabled?'warn':'ok')+
    card('停止・警告理由',reasonName[j.control.reason]||('コード '+j.control.reason),j.control.reason?'warn':'ok')+
    card('UART受信','フレーム '+j.link.frames+' / CRCエラー '+j.link.crc_errors,j.link.crc_errors?'warn':'');
  for(let i=0;i<4;i++)el('mode'+i).classList.toggle('active',j.control.mode===i);renderManualState();
}
async function updateStatus(){try{const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw new Error('HTTP '+response.status);renderStatus(await response.json())}catch(error){el('linkBanner').className='status bad';el('linkBanner').textContent='CoreS3の状態取得に失敗しました: '+error}}
for(const id of ['l','r','y','p'])el(id).addEventListener('input',updateValueLabels);
for(const id of ['enL','enR','enY','enP'])el(id).addEventListener('change',()=>{renderManualState();if(manualContinuous)sendManual()});
setInterval(updateStatus,250);setInterval(()=>{if(manualContinuous)sendManual()},250);updateValueLabels();renderManualState();updateStatus();
</script>
</html>
)HTML";
