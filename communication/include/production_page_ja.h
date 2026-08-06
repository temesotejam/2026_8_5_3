#pragma once

constexpr char productionPageJapanese[]=R"HTML(
<!doctype html>
<html lang="ja">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>水上ボート 手動操作</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f3f5f7;color:#17212b;font:16px system-ui,sans-serif}
main{max-width:560px;margin:auto;padding:20px}
h1{font-size:24px;margin:4px 0 16px}
.panel{background:#fff;border:1px solid #d8dee4;border-radius:12px;padding:18px}
.status{margin-bottom:20px;padding:11px 13px;border-radius:8px;background:#fff3cd;color:#664d03}
.status.ok{background:#dff4e5;color:#165c2b}.status.bad{background:#fde2e1;color:#842029}
label{display:block;font-weight:700;margin:14px 0 7px}
select{width:100%;font:inherit;padding:11px;border:1px solid #aab4bd;border-radius:7px;background:#fff}
.range{display:grid;grid-template-columns:1fr 58px;gap:12px;align-items:center}
input[type=range]{width:100%}.value{text-align:right;font-variant-numeric:tabular-nums;font-weight:700}
.buttons{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:22px}
button{min-height:48px;border:0;border-radius:8px;font:700 16px system-ui;cursor:pointer}
button:disabled{opacity:.45;cursor:not-allowed}
.start{background:#2da44e;color:#fff}.stop{background:#d8dee4;color:#17212b}.emergency{grid-column:1/-1;background:#cf222e;color:#fff}
.emergency.clear{background:#f0b429;color:#17212b}
.message{min-height:24px;margin:14px 0 0;color:#57606a}
.output{margin:12px 0 0;color:#57606a;font-size:14px}
</style>
<main>
  <h1>水上ボート 手動操作</h1>
  <div id="connection" class="status">接続を確認しています…</div>
  <section class="panel">
    <label for="channel">動かす出力</label>
    <select id="channel">
      <option value="0">左前翼（CH0）</option>
      <option value="1">右前翼（CH1）</option>
      <option value="2">後部（CH2）</option>
    </select>

    <label for="level">出力値</label>
    <div class="range">
      <input id="level" type="range" min="-1" max="1" step="0.01" value="0">
      <span id="levelValue" class="value">0.00</span>
    </div>

    <div class="buttons">
      <button id="startButton" class="start" onclick="startOutput()">開始</button>
      <button id="stopButton" class="stop" onclick="stopOutput()">停止</button>
      <button id="emergencyButton" class="emergency" onclick="toggleEmergency()">緊急停止</button>
    </div>

    <div id="message" class="message">出力先と値を決めて「開始」を押してください。</div>
    <div id="actualOutput" class="output">全出力OFF</div>
  </section>
</main>

<script>
const byId=id=>document.getElementById(id);
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const stateText=['起動中','停止中','準備中','動作中','緊急停止','異常停止'];
let lastStatus=null;
let sending=false;
let busy=false;
let postingManual=false;

function setMessage(text){byId('message').textContent=text}
function channelMask(){return 1<<Number(byId('channel').value)}
function manualUrl(mask,value){
  let left=0,right=0,rear=0;
  const channel=Number(byId('channel').value);
  if(mask&1&&channel===0)left=value;
  if(mask&2&&channel===1)right=value;
  if(mask&4&&channel===2)rear=value;
  return '/api/competition/manual?left_front_wing='+left+'&right_front_wing='+right+'&rear_yaw='+rear+'&propulsion=0&enabled_mask='+mask;
}
async function post(url){
  try{
    const response=await fetch(url,{method:'POST'});
    return response.ok;
  }catch(error){
    return false;
  }
}
async function sendManual(mask=channelMask(),value=Number(byId('level').value)){
  if(postingManual)return true;
  postingManual=true;
  const ok=await post(manualUrl(mask,value));
  postingManual=false;
  return ok;
}
async function fetchStatus(){
  const response=await fetch('/api/status',{cache:'no-store'});
  if(!response.ok)throw new Error('status');
  const status=await response.json();
  renderStatus(status);
  return status;
}
async function waitFor(predicate,timeoutMs=1800){
  const deadline=Date.now()+timeoutMs;
  while(Date.now()<deadline){
    try{if(predicate(await fetchStatus()))return true}catch(error){}
    await sleep(100);
  }
  return false;
}
function updateButtons(){
  const ready=lastStatus&&lastStatus.connected&&lastStatus.actuators&&lastStatus.actuators.pca_ready;
  const blocked=lastStatus&&(lastStatus.control.safety===4||lastStatus.control.safety===5);
  const stopped=lastStatus&&lastStatus.control.safety===1;
  byId('startButton').disabled=busy||sending||!ready||blocked||!stopped;
  byId('stopButton').disabled=busy||(!sending&&lastStatus&&lastStatus.control.safety===1);
  byId('channel').disabled=busy||sending;
  byId('emergencyButton').disabled=busy||!lastStatus||!lastStatus.connected;
}
function renderStatus(status){
  lastStatus=status;
  const banner=byId('connection');
  if(!status.connected){
    banner.className='status bad';
    banner.textContent=status.ever_received?'制御側XIAOとの通信が切れています':'制御側XIAOを待っています';
  }else if(!status.actuators.pca_ready){
    banner.className='status bad';
    banner.textContent='PCA9685が見つかりません';
  }else{
    banner.className='status ok';
    banner.textContent='接続済み・'+(stateText[status.control.safety]||'状態不明');
  }
  const emergency=status.control.safety===4;
  const emergencyButton=byId('emergencyButton');
  emergencyButton.textContent=emergency?'緊急停止を解除':'緊急停止';
  emergencyButton.classList.toggle('clear',emergency);
  if(status.control.safety===3){
    const masks=[1,2,4];
    const channel=masks.indexOf(status.actuators.enabled_mask);
    const pulses=[status.actuators.left_us,status.actuators.right_us,status.actuators.rear_us];
    byId('actualOutput').textContent=channel>=0?'CH'+channel+' 出力中・'+pulses[channel]+' µs':'出力状態を確認してください';
  }else{
    byId('actualOutput').textContent='全出力OFF';
    if(sending&&!busy)sending=false;
  }
  updateButtons();
}
async function failStart(message){
  sending=false;
  await post('/api/competition/safety?action=stop');
  await sendManual(0,0);
  busy=false;
  setMessage(message);
  updateButtons();
}
async function startOutput(){
  if(busy||sending)return;
  if(!lastStatus||!lastStatus.connected||!lastStatus.actuators.pca_ready){
    setMessage('制御側XIAOとPCA9685の接続を確認してください。');
    return;
  }
  busy=true;
  sending=true;
  updateButtons();
  setMessage('開始準備中…');
  if(!await post('/api/competition/mode?mode=0'))return failStart('手動モードにできませんでした。');
  if(!await waitFor(status=>status.control.mode===0))return failStart('手動モードの確認に失敗しました。');
  if(!await sendManual())return failStart('出力値を送れませんでした。');
  if(!await post('/api/competition/safety?action=arm'))return failStart('開始準備に失敗しました。');
  if(!await waitFor(status=>status.control.safety===2))return failStart('開始できませんでした。接続を確認してください。');
  if(!await post('/api/competition/safety?action=start'))return failStart('開始指令を送れませんでした。');
  if(!await waitFor(status=>status.control.safety===3))return failStart('動作開始を確認できませんでした。');
  busy=false;
  setMessage('選択した1チャンネルだけを出力しています。');
  updateButtons();
}
async function stopOutput(){
  if(busy)return;
  busy=true;
  sending=false;
  updateButtons();
  await post('/api/competition/safety?action=stop');
  await sendManual(0,0);
  byId('level').value=0;
  byId('levelValue').textContent='0.00';
  await waitFor(status=>status.control.safety===1);
  busy=false;
  setMessage('停止しました。すべての出力はOFFです。');
  updateButtons();
}
async function toggleEmergency(){
  if(busy||!lastStatus)return;
  busy=true;
  sending=false;
  updateButtons();
  if(lastStatus.control.safety===4){
    await post('/api/competition/safety?action=clear_estop');
    await waitFor(status=>status.control.safety===1);
    setMessage('緊急停止を解除しました。');
  }else{
    await post('/api/competition/safety?action=estop');
    await waitFor(status=>status.control.safety===4);
    setMessage('緊急停止しました。すべての出力はOFFです。');
  }
  await sendManual(0,0);
  byId('level').value=0;
  byId('levelValue').textContent='0.00';
  busy=false;
  updateButtons();
}
byId('level').addEventListener('input',()=>{
  byId('levelValue').textContent=Number(byId('level').value).toFixed(2);
});
setInterval(()=>{if(sending&&!busy)sendManual()},200);
setInterval(()=>{fetchStatus().catch(()=>{lastStatus=null;byId('connection').className='status bad';byId('connection').textContent='CoreS3の状態を取得できません';updateButtons()})},300);
fetchStatus().catch(()=>{});
</script>
</html>
)HTML";
