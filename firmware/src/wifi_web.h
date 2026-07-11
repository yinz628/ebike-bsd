// ============================================================
//  wifi_web.h - WiFi AP + Web控制台 (单配置版, 无预设)
// ============================================================
#ifndef WIFI_WEB_H
#define WIFI_WEB_H

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "config_store.h"

// ==== 全局变量 extern ====
extern MS60Radar radar;
extern TurnState_t turn_state;
extern bool bsd_l_active, bsd_r_active;
extern bool rcw_l_active, rcw_r_active;
extern int buzzer_mode;
extern int ind_left_mode, ind_right_mode;

AsyncWebServer server(80);

// WiFi 发射功率. 默认 20dBm(100mW) 是主控发烫主因之一.
// 11dBm(约 13mW) 在 1.5-2m 距离连接稳定, 发热显著降低.
#define WIFI_TX_POWER  WIFI_POWER_11dBm

void initWiFi() {
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.softAP(config.sys.wifi_ssid, config.sys.wifi_pass);
    Serial.print("[WIFI] AP: "); Serial.print(config.sys.wifi_ssid);
    Serial.print("  IP: "); Serial.println(WiFi.softAPIP());
    Serial.println("[WIFI] TX power: 11dBm (降功率省电)");
}

void handleStatus(AsyncWebServerRequest *req) {
    static StaticJsonDocument<4096> doc;
    doc.clear();
    BSDFrame *f = radar.getFrame();
    JsonObject r = doc["radar"].to<JsonObject>();
    r["targets"] = f->obj_num;
    r["valid"]   = f->valid;
    r["bytes"]   = radar.getTotalBytes();
    r["det_range"] = config.radar.det_range;
    JsonArray tgts = r["list"].to<JsonArray>();
    for (int i = 0; i < f->obj_num && i < 8; i++) {
        JsonObject t = tgts.createNestedObject();
        t["id"]=i; t["range"]=f->objects[i].range;
        t["angle"]=f->objects[i].angle; t["velo"]=f->objects[i].velocity;
    }
    JsonObject s = doc["system"].to<JsonObject>();
    s["turn"]=turn_state; s["bsd_l"]=bsd_l_active; s["bsd_r"]=bsd_r_active;
    s["rcw_l"]=rcw_l_active; s["rcw_r"]=rcw_r_active;
    s["buzzer"]=buzzer_mode; s["ind_left"]=ind_left_mode; s["ind_right"]=ind_right_mode;
    s["uptime"]=millis()/1000;
    String json; serializeJson(doc, json);
    req->send(200, "application/json", json);
}

void handleConfigGet(AsyncWebServerRequest *req) {
    static StaticJsonDocument<4096> doc;
    doc.clear();
    config.toJson(doc);
    String json; serializeJson(doc, json);
    Serial.print("[WEB] GET config: "); Serial.println(json);
    req->send(200, "application/json", json);
}

void handleFactoryReset(AsyncWebServerRequest *req) {
    config.factoryReset();
    radar.setBSDMode();   // 出厂配置立即下发雷达, 无需重启即生效
    req->send(200, "text/plain", "OK");
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="zh">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>eBike BSD</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--accent:#58a6ff;--danger:#f85149;--warn:#d29922;--ok:#3fb950;--text:#c9d1d9;--muted:#8b949e;--border:#30363d}
*{margin:0;padding:0;box-sizing:border-box}
body{font:14px/1.4 -apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);color:var(--text);padding:8px;max-width:520px;margin:0 auto}
h1{font-size:16px;text-align:center;padding:10px 0 4px}
h1 span{color:var(--accent)}
.sub{text-align:center;color:var(--muted);font-size:11px;margin-bottom:8px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;margin:8px 0}
.card h2{font-size:13px;color:var(--accent);margin-bottom:8px}
.row{display:flex;align-items:center;padding:5px 0;gap:8px}
.row label{flex:0 0 80px;font-size:12px;color:var(--muted);text-align:right}
.row input{flex:1;padding:6px 8px;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);font-size:13px;max-width:90px}
.row input:focus{outline:none;border-color:var(--accent)}
.row .unit{font-size:11px;color:var(--muted);min-width:24px}
.btn{display:block;width:100%;padding:10px;margin:4px 0;border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer}
.btn:active{transform:scale(.97)}
.btn-save{background:var(--accent);color:#fff}
.btn-reset{background:var(--danger);color:#fff}
#status-row{display:flex;gap:6px;flex-wrap:wrap;margin:6px 0}
.sys-box{flex:1;min-width:80px;background:var(--bg);border-radius:8px;padding:6px;text-align:center;font-size:10px}
.sys-box .dot{display:inline-block;width:12px;height:12px;border-radius:50%;background:var(--border);border:2px solid var(--muted);margin:2px}
.sys-box .txt{margin-top:2px;font-size:10px;color:var(--muted)}
#radar-viz{height:130px;background:var(--bg);border:1px solid var(--border);border-radius:10px;position:relative;overflow:hidden;margin:8px 0}
#radar-viz .car{position:absolute;left:10px;top:50%;transform:translateY(-50%);font-size:28px;z-index:2}
#radar-viz .dot{position:absolute;width:8px;height:8px;border-radius:50%;background:var(--danger);transform:translate(-50%,-50%);z-index:1;box-shadow:0 0 6px rgba(248,81,73,.5)}
#radar-viz .ruler{position:absolute;left:0;right:0;height:1px;background:var(--border);opacity:.3}
#radar-viz .ruler span{position:absolute;left:4px;bottom:-10px;font-size:8px;color:var(--muted)}
#target-info{font:10px monospace;color:var(--muted);padding:0 4px}
.toast{position:fixed;top:12px;left:50%;transform:translateX(-50%);background:var(--accent);color:#fff;padding:8px 20px;border-radius:8px;font-size:13px;z-index:10;opacity:0;transition:.3s;pointer-events:none}
.toast.show{opacity:1}.toast.err{background:var(--danger)}
footer{text-align:center;color:var(--muted);font-size:10px;padding:16px 0}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.3}}
</style></head>
<body>
<h1>eBike <span>BSD</span> V2.7 <small style="color:var(--ok);font-size:10px">CANVAS</small></h1>
<div class="sub">热点: eBike-BSD | 192.168.4.1 | <span id="load-status" style="color:var(--warn)">加载中...</span></div>

<div class="card">
 <h2>实时状态</h2>
 <canvas id="radar-canvas" style="width:100%;height:auto;background:var(--card);border:1px solid var(--border);border-radius:10px;display:block;margin:8px 0"></canvas>
 <div id="target-info">等待数据...</div>
 <div id="status-row">
  <div class="sys-box"><div>🔊 后方</div>
   <span class="dot" id="rear-l-dot" style="border-color:var(--warn)"></span><span class="dot" id="rear-r-dot" style="border-color:var(--warn)"></span>
   <div class="txt" id="rear-text">安全</div></div>
  <div class="sys-box"><div>🚨 转向</div>
   <span class="dot" id="turn-dot"></span>
   <div class="txt" id="turn-text">未触发</div></div>
  <div class="sys-box"><div>🔔 蜂鸣</div>
   <div id="buzz-ind" style="font-size:16px">🔇</div>
   <div class="txt" id="buzz-text">静音</div></div>
 </div>
</div>

<div class="card"><h2>🔊 后方监测 (BSD+RCW)</h2>
 <div class="row"><label>左角度</label><input id="r_lmin" type="number"> ~ <input id="r_lmax" type="number"><span class="unit">°</span></div>
 <div class="row"><label>右角度</label><input id="r_rmin" type="number"> ~ <input id="r_rmax" type="number"><span class="unit">°</span></div>
 <div class="row"><label>低警告速度</label><input id="r_low" type="number" step="0.5"><span class="unit">m/s</span></div>
 <div class="row"><label>高警告速度</label><input id="r_spd" type="number" step="0.5"><span class="unit">m/s</span></div>
 <div class="row"><label>距离上限</label><input id="r_rng" type="number" step="5"><span class="unit">m</span></div>
 <div class="row"><label>横向距离</label><input id="r_lat" type="number" step="1" min="1" max="10"><span class="unit">m (过滤远处车误报)</span></div>
 <div class="row"><label>保持时间</label><input id="r_hold" type="number" step="100"><span class="unit">ms</span></div>
 <div class="row"><label>低LED闪烁</label><input id="r_lflash" type="number" step="50"><span class="unit">ms</span></div>
 <div class="row"><label>高LED闪烁</label><input id="r_flash" type="number" step="25"><span class="unit">ms</span></div>
 <div class="row"><label>蜂鸣冷却</label><input id="r_cool" type="number" step="500"><span class="unit">ms</span></div>
</div>

<div class="card"><h2>🚨 转向辅助</h2>
 <div class="row"><label>左角度</label><input id="t_lmin" type="number"> ~ <input id="t_lmax" type="number"><span class="unit">°</span></div>
 <div class="row"><label>右角度</label><input id="t_rmin" type="number"> ~ <input id="t_rmax" type="number"><span class="unit">°</span></div>
 <div class="row"><label>速度阈值</label><input id="t_spd" type="number" step="0.5"><span class="unit">m/s</span></div>
 <div class="row"><label>距离上限</label><input id="t_rng" type="number" step="5"><span class="unit">m</span></div>
 <div class="row"><label>横向距离</label><input id="t_lat" type="number" step="1" min="1" max="10"><span class="unit">m (过滤远处车误报)</span></div>
</div>

<div class="card"><h2>⚙️ 系统</h2>
 <div class="row"><label>雷达距离</label><input id="rd_rng" type="number" step="5" min="5" max="50"><span class="unit">m</span></div>
 <div class="row"><label>灵敏度</label><select id="rd_sens" style="flex:1;padding:6px;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);max-width:90px"><option value="0">0 (最灵敏)</option><option value="1">1</option><option value="2">2 (默认)</option><option value="3">3</option><option value="4">4</option><option value="5">5</option><option value="6">6</option><option value="7">7</option><option value="8">8</option><option value="9">9</option><option value="10">10 (最不灵敏)</option></select><span class="unit">档</span></div>
 <div class="row"><label>WiFi</label><button onclick="wifiOff()" style="flex:1;max-width:80px;padding:6px;border:1px solid var(--danger);border-radius:6px;background:var(--bg);color:var(--danger);font-size:12px;cursor:pointer">关闭WiFi</button><span class="unit" style="font-size:10px;color:var(--muted)">立即生效,下次开机重开</span></div>
<div class="row"><label>蜂鸣冷却</label><input id="s_cool" type="number" step="500"><span class="unit">ms</span></div>
 <div class="row"><label>后方蜂鸣</label><select id="s_buzz" style="flex:1;padding:6px;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);max-width:90px"><option value="1">开启</option><option value="0">关闭</option></select><span class="unit">RCW后方监测蜂鸣 (转向辅助不受此控制)</span></div>
</div>

<button class="btn btn-save" onclick="save()">💾 保存当前配置</button>
<button class="btn btn-reset" onclick="resetCfg()">🔄 恢复出厂默认</button>
<div class="toast" id="toast"></div>
<footer>eBike BSD V2.7 · ESP32 MS60-3015</footer>

<script>
function q(s){return document.querySelector(s)}
function toast(m,e){var t=q('#toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(function(){t.className='toast'},1500)}

async function loadCfg(){
 try{
  q('#load-status').textContent='请求API...';q('#load-status').style.color='var(--warn)';
  var r=await Promise.race([fetch('/api/config'),new Promise(function(_,reject){setTimeout(function(){reject(new Error('timeout'))},5000)})]);
  var j=await r.json(),rw=j.rcw,t=j.turn,s=j.sys||{};
  q('#r_lmin').value=rw.left_min||-40;q('#r_lmax').value=rw.left_max||-5;
  q('#r_rmin').value=rw.right_min||5;q('#r_rmax').value=rw.right_max||40;
  q('#r_low').value=rw.low_speed||2;
  q('#r_spd').value=rw.speed;q('#r_rng').value=rw.range;q('#r_lat').value=rw.lateral||3;q('#r_hold').value=rw.hold;q('#r_lflash').value=rw.lflash||500;
  q('#r_flash').value=rw.flash||125;
  q('#r_cool').value=s.bsd_beep_cooldown||5000;
  q('#t_lmin').value=t.left_min||-40;q('#t_lmax').value=t.left_max||-5;
  q('#t_rmin').value=t.right_min||5;q('#t_rmax').value=t.right_max||40;
  q('#t_spd').value=t.speed;q('#t_rng').value=t.range;q('#t_lat').value=t.lateral||3;
  q('#s_cool').value=s.bsd_beep_cooldown||5000;
  q('#s_buzz').value=(s.rcw_buzzer!==undefined?s.rcw_buzzer:1);
  var rd=j.radar||{};
  q('#rd_rng').value=rd.det_range||30;
  q('#rd_sens').value=(rd.sensitivity!==undefined?rd.sensitivity:2);
  q('#load-status').textContent='✅ 加载成功';q('#load-status').style.color='var(--ok)';
 }catch(e){q('#load-status').textContent='❌ '+e.message;q('#load-status').style.color='var(--danger)';toast('加载失败',true)}
}

async function save(){
 var cfg={
  rcw:{left_min:+q('#r_lmin').value,left_max:+q('#r_lmax').value,right_min:+q('#r_rmin').value,right_max:+q('#r_rmax').value,low_speed:+q('#r_low').value,speed:+q('#r_spd').value,range:+q('#r_rng').value,lateral:+q('#r_lat').value,hold:+q('#r_hold').value,lflash:+q('#r_lflash').value,flash:+q('#r_flash').value},
  turn:{left_min:+q('#t_lmin').value,left_max:+q('#t_lmax').value,right_min:+q('#t_rmin').value,right_max:+q('#t_rmax').value,speed:+q('#t_spd').value,range:+q('#t_rng').value,lateral:+q('#t_lat').value},
  sys:{bsd_beep_cooldown:+q('#r_cool').value,rcw_buzzer:+q('#s_buzz').value},
  radar:{det_range:+q('#rd_rng').value,sensitivity:+q('#rd_sens').value}
 };
 try{
  var r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
  toast(r.ok?'配置已保存':('保存失败 HTTP '+r.status),!r.ok);
 }catch(e){toast('网络错误',true)}
}

async function resetCfg(){
 if(!confirm('恢复出厂默认？'))return;
 try{var r=await fetch('/api/reset',{method:'POST'});if(r.ok){toast('已恢复');loadCfg()}else toast('失败',true)}
 catch(e){toast('网络错误',true)}
}

async function poll(){
 try{
  var r=await fetch('/api/status'),j=await r.json(),rd=j.radar||{},st=j.system||{},list=rd.list||[];
  
  // 信息栏
  var info='';for(var i=0;i<list.length;i++){var t=list[i];info+=t.angle+'° '+Math.round(t.range)+'m '+(t.velo?Math.round(t.velo)+'m/s':'')+' | '}
  q('#target-info').textContent=info||(rd.valid?'无目标':'帧无效');

  // 状态指示器
  var ud=function(el,a,c){if(!el)return;if(a){el.style.background=c;el.style.borderColor=c;el.style.boxShadow='0 0 6px '+c}else{el.style.background='var(--border)';el.style.borderColor='var(--muted)';el.style.boxShadow='none'}};
  ud(q('#rear-l-dot'),st.bsd_l||st.rcw_l,(st.rcw_l?'#f85149':'#3fb950'));
  ud(q('#rear-r-dot'),st.bsd_r||st.rcw_r,(st.rcw_r?'#f85149':'#3fb950'));
  var rtxt='安全';
  if(st.rcw_l||st.rcw_r)rtxt='⚠️ 快车逼近!';
  else if(st.bsd_l||st.bsd_r)rtxt='🔹 盲区有车';
  q('#rear-text').textContent=rtxt;
  var ta=(st.ind_left==3||st.ind_right==3);
  q('#turn-dot').style.background=ta?'#d29922':'var(--border)';
  q('#turn-dot').style.boxShadow=ta?'0 0 6px #d29922':'none';
  q('#turn-text').textContent=ta?'触发':'未触发';
  var bi=q('#buzz-ind'),bt=q('#buzz-text'),bz=st.buzzer||0;
  bi.textContent=bz==0?'🔇':(bz==1?'🔔':(bz==2?'🚨':'📢'));
  bt.textContent=bz==0?'静音':(bz==1?'短鸣':(bz==2?'4Hz':'长鸣'));
  
  // Canvas雷达绘制
  var canvas=document.getElementById('radar-canvas');
  if(canvas){
   canvas.style.background='#161b22';
   var w=canvas.parentElement.clientWidth,h=Math.round(w*0.7);
   canvas.width=w;canvas.height=h;
   var ctx=canvas.getContext('2d'),cx=w/2,cy=28,maxR=h-cy-14;
   var toRad=Math.PI/180,FOV=120,half=FOV/2*toRad,base=Math.PI/2;
   var detRange=rd.det_range||30;
   ctx.clearRect(0,0,w,h);
   // 扇形背景
   ctx.beginPath();ctx.moveTo(cx,cy);
   ctx.arc(cx,cy,maxR,base-half,base+half,true);ctx.closePath();
   ctx.fillStyle='rgba(48,54,61,0.12)';ctx.fill();
   // 弧线
   [1,2,3,4].forEach(function(i){var dm=Math.round(detRange*i/4),r=(dm/detRange)*maxR;
    ctx.beginPath();ctx.arc(cx,cy,r,base-half,base+half,true);
    ctx.strokeStyle='rgba(139,148,158,0.22)';ctx.lineWidth=1;ctx.stroke();
    var lx=cx+r*Math.cos(base-half);ctx.fillStyle='#8b949e';ctx.font='9px sans-serif';ctx.fillText(dm+'m',lx-26,cy+r*Math.sin(base-half)+4);
   });
   // 中心虚线
   ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx,cy+maxR);
   ctx.strokeStyle='rgba(139,148,158,0.15)';ctx.setLineDash([4,6]);ctx.stroke();ctx.setLineDash([]);
   // ±40°边界
   var rh=40*toRad;ctx.beginPath();ctx.moveTo(cx,cy);
   ctx.lineTo(cx+maxR*Math.sin(-rh),cy+maxR*Math.cos(-rh));
   ctx.strokeStyle='rgba(88,166,255,0.18)';ctx.setLineDash([2,8]);ctx.stroke();
   ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+maxR*Math.sin(rh),cy+maxR*Math.cos(rh));ctx.stroke();ctx.setLineDash([]);
   // 目标
   list.forEach(function(t){
    var ar=t.angle*toRad,r=(t.range/detRange)*maxR;
    var tx=cx+r*Math.sin(ar),ty=cy+r*Math.cos(ar);
    tx=Math.max(4,Math.min(w-4,tx));ty=Math.max(cy,Math.min(cy+maxR,ty));
    ctx.beginPath();ctx.arc(tx,ty,7,0,2*Math.PI);ctx.fillStyle='rgba(248,81,73,0.2)';ctx.fill();
    ctx.beginPath();ctx.arc(tx,ty,3.5,0,2*Math.PI);ctx.fillStyle='#f85149';ctx.fill();
    var lb=t.angle+'° '+t.range+'m';if(t.velo)lb+=' '+t.velo+'m/s';
    ctx.fillStyle='#c9d1d9';ctx.font='9px monospace';ctx.textAlign=t.angle>=0?'left':'right';
    ctx.fillText(lb,tx+(t.angle>=0?6:-6),ty-8);ctx.textAlign='start';
   });
   // 本车
   ctx.font='20px sans-serif';ctx.textAlign='center';ctx.fillText('🚲',cx,cy-2);
  }
 }catch(e){q('#target-info').textContent='Canvas错误:'+e.message}}
async function wifiOff(){
 if(!confirm('关闭WiFi热点？下次开机自动重开。'))return;
 try{
  var r=await fetch('/api/wifi_off',{method:'POST'});
  toast(r.ok?'WiFi已关闭, 下次开机自动重开':'关闭失败',!r.ok);
 }catch(e){toast('网络错误',true)}
}
loadCfg().then(function(){setInterval(poll,800);poll()});
</script></body></html>
)rawliteral";

void initWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
        Serial.println("[WEB] PAGE served");
        req->send(200, "text/html", INDEX_HTML);
    });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/config", HTTP_OPTIONS, [](AsyncWebServerRequest *req){
        AsyncWebServerResponse *resp = req->beginResponse(200);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
        req->send(resp);
    });
    server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest *req){}, nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
            static String body;
            if (index == 0) body = "";
            body.concat((char*)data, len);
            if (index + len >= total) {
                static StaticJsonDocument<4096> doc;
                doc.clear();
                DeserializationError err = deserializeJson(doc, body);
                if (!err) {
                    Serial.print("[WEB] POST body: "); Serial.println(body);
                    config.fromJson(doc);
                    bool saved = config.saveToNVS();
                    if (!saved) Serial.println("[WEB] WARN: saveToNVS returned false!");
                    radar.setBSDMode();
                } else {
                    Serial.print("[WEB] JSON parse err: "); Serial.println(err.c_str());
                }
                AsyncWebServerResponse *resp = req->beginResponse(err ? 400 : 200, "text/plain", err ? "invalid JSON" : "OK");
                resp->addHeader("Access-Control-Allow-Origin", "*");
                req->send(resp);
            }
        }
    );
        server.on("/api/wifi_off", HTTP_POST, [](AsyncWebServerRequest *req){
        req->send(200, "text/plain", "OK");
        delay(100);
        server.end();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[WIFI] User disabled, WiFi off. Restart to re-enable.");
    });
    
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/reset", HTTP_POST, handleFactoryReset);
    server.begin();
    Serial.println("[WEB] Server started on port 80");
}

#endif
