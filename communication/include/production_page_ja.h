#pragma once

constexpr char productionPageJapanese[] = R"HTML(
<!doctype html>
<html lang="ja">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>水上ボート 全体手動運転</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f4f5f7;color:#18202a;font:16px system-ui,sans-serif}
main{max-width:520px;margin:auto;padding:18px}
h1{font-size:23px;margin:2px 0 14px}
.status,.panel{border:1px solid #d7dce2;border-radius:10px;background:#fff;padding:14px}
.status{margin-bottom:12px;background:#fff4ce}.status.ok{background:#dcf3e3}.status.bad{background:#fde0df}
label{display:block;font-weight:700;margin:14px 0 7px}
.range{display:grid;grid-template-columns:1fr 62px;gap:10px;align-items:center}
input[type=range]{width:100%}.number{text-align:right;font-weight:700;font-variant-numeric:tabular-nums}
.buttons{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:20px}
button{min-height:48px;border:0;border-radius:8px;font:700 16px system-ui}
button:disabled{opacity:.42}.start{background:#24863d;color:#fff}.stop{background:#d9dee4}.estop{grid-column:1/-1;background:#c92532;color:#fff}.clear{background:#efb521;color:#18202a}
.message{margin-top:14px;min-height:46px}.detail{margin-top:10px;color:#59636e;font-size:14px;line-height:1.55}
</style>
<main>
  <h1>全体手動運転</h1>
  <div id="connection" class="status">XIAOを確認しています…</div>
  <section class="panel">
    <label for="left">左前翼 CH0</label>
    <div class="range">
      <input id="left" type="range" min="-1" max="1" step="0.01" value="0">
      <span id="left-number" class="number">0.00</span>
    </div>

    <label for="right">右前翼 CH1</label>
    <div class="range">
      <input id="right" type="range" min="-1" max="1" step="0.01" value="0">
      <span id="right-number" class="number">0.00</span>
    </div>

    <label for="rear">後部ヨー CH2</label>
    <div class="range">
      <input id="rear" type="range" min="-1" max="1" step="0.01" value="0">
      <span id="rear-number" class="number">0.00</span>
    </div>

    <label for="propulsion">推進</label>
    <div class="range">
      <input id="propulsion" type="range" min="0" max="1" step="0.01" value="0">
      <span id="propulsion-number" class="number">0.00</span>
    </div>

    <div class="buttons">
      <button id="start" class="start">開始</button>
      <button id="stop" class="stop">停止</button>
      <button id="estop" class="estop">緊急停止</button>
    </div>

    <div id="message" class="message">4つの出力値を決めて開始してください。</div>
    <div id="detail" class="detail">全出力OFF</div>
  </section>
</main>
<script>
const get=id=>document.getElementById(id);
let latest=null;
let posting=false;
let requestsInFlight=0;
const outputIds=['left','right','rear','propulsion'];

function manualQuery(){
  return outputIds.map(id=>id+'='+encodeURIComponent(get(id).value)).join('&');
}

async function post(path){
  requestsInFlight++;
  posting=true;
  try{
    const response=await fetch(path,{method:'POST'});
    const result=await response.json();
    get('message').textContent=result.message||'指令を送りました。';
  }catch(error){
    get('message').textContent='CoreS3へ指令を送れませんでした。';
  }
  requestsInFlight--;
  posting=requestsInFlight>0;
}

function render(state){
  latest=state;
  const connection=get('connection');
  if(!state.connected){
    connection.className='status bad';
    connection.textContent=state.ever_received?'XIAOとの通信が切れています':'XIAOを待っています';
  }else if(!state.actuators.pca_ready){
    connection.className='status bad';
    connection.textContent='XIAO接続済み・PCA9685未接続';
  }else{
    connection.className='status ok';
    connection.textContent='XIAO接続済み・'+state.control.safety_name;
  }
  get('message').textContent=state.message;
  const running=state.operation==='running';
  get('detail').textContent=running
    ? 'PWM '+state.actuators.left_us+'・'+state.actuators.right_us+'・'+state.actuators.rear_us+' µs / 推進Duty '+Number(state.actuators.applied_duty).toFixed(3)+' / D10 '+(state.actuators.relay?'HIGH':'LOW')
    : '全出力OFF / 停止理由 '+state.control.stop_reason_name+' / PCAエラー '+state.actuators.pwm_errors;
  get('start').disabled=posting||!state.connected||!state.actuators.pca_ready||(!['idle','error'].includes(state.operation));
  get('stop').disabled=posting;
  const emergency=state.control.safety===4;
  get('estop').textContent=emergency?'緊急停止を解除':'緊急停止';
  get('estop').classList.toggle('clear',emergency);
}

async function poll(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'});
    if(!response.ok)throw new Error();
    render(await response.json());
  }catch(error){
    latest=null;
    get('connection').className='status bad';
    get('connection').textContent='CoreS3から状態を取得できません';
    get('start').disabled=true;
  }
}

outputIds.forEach(id=>get(id).addEventListener('input',()=>{
  get(id+'-number').textContent=Number(get(id).value).toFixed(2);
}));
outputIds.forEach(id=>get(id).addEventListener('change',()=>{
  if(latest&&latest.operation==='running')post('/api/value?'+manualQuery());
}));
get('start').addEventListener('click',()=>post('/api/start?'+manualQuery()));
get('stop').addEventListener('click',()=>post('/api/stop'));
get('estop').addEventListener('click',()=>post(latest&&latest.control.safety===4?'/api/clear-estop':'/api/estop'));
setInterval(poll,300);
poll();
</script>
</html>
)HTML";
