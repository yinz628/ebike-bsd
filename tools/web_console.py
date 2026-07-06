#!/usr/bin/env python3
"""BSD 调试控制台 V2 — 完整设置 + 数据监控"""
import http.server
import json
import serial
import time
import threading
import re
import os
from collections import deque

# Auto-detect ESP32 serial port
import glob
_ports = sorted(glob.glob('/dev/ttyUSB*'))
SERIAL_PORT = _ports[0] if _ports else '/dev/ttyUSB0'
SERIAL_BAUD = 115200
WEB_PORT = 8080

# Serial writer for commands
serial_send = None

state = {
    "rx_bytes": 0, "bsd_targets": 0, "bsd_valid": False,
    "targets": [], "turn": "OFF", "buzzer": 0,
    "indicator_l": False, "indicator_r": False,
    "rcw_l": False, "rcw_r": False,
    "last_update": 0, "uptime": 0,
    "raw_hex": "", "raw_len": 0,
    "events": deque(maxlen=50),
    "settings": {
        "blind_spot_l_min": -40, "blind_spot_l_max": -15,
        "blind_spot_r_min": 15, "blind_spot_r_max": 40,
        "led_timeout_ms": 3000,
        "rcw_speed_threshold": 4,
        "rcw_range_limit": 20
    }
}

def parse_line(line):
    try:
        m = re.search(r'\[(\d+)\]', line)
        if m: state['uptime'] = int(m.group(1))
        m = re.search(r'RX:(\d+)', line)
        if m: state['rx_bytes'] = int(m.group(1))
        m = re.search(r'BSD:(\d+)tgt\s+(!|V)', line)
        if m:
            state['bsd_targets'] = int(m.group(1))
            state['bsd_valid'] = (m.group(2) == 'V')
        targets = []
        for m in re.finditer(r'\[(\d+)m,(-?\d+)°,(-?\d+)m/s[^\]]*\]', line):
            targets.append({'range':int(m.group(1)),'angle':int(m.group(2)),'velocity':int(m.group(3))})
        if targets: state['targets'] = targets
        m = re.search(r'BZ:(\d+)', line)
        if m: state['buzzer'] = int(m.group(1))
        if 'IND:L' in line: state['indicator_l'], state['indicator_r'] = True, False
        elif 'IND:R' in line: state['indicator_l'], state['indicator_r'] = False, True
        elif 'IND:-' in line: state['indicator_l'], state['indicator_r'] = False, False
        state['rcw_l'], state['rcw_r'] = 'RCW-L' in line, 'RCW-R' in line
        m = re.search(r'TURN:(OFF|L|R|HAZ)', line)
        if m: state['turn'] = m.group(1)
        # Raw hex
        m = re.search(r'RAW=([0-9A-Fa-f ]{20,})', line)
        if m:
            state['raw_hex'] = m.group(1).strip()
            state['raw_len'] = len(state['raw_hex'].split())
        # Events
        for pat, label in [('RCW.*快速接近', '⚠ RCW'), ('AUTO-CFG.*启动', '⚙ 自动配置'),
                           ('BSD.*盲区', '🔍 盲区'), ('RADAR.*OK', '📡 雷达就绪')]:
            if re.search(pat, line):
                state['events'].appendleft({'time': time.strftime('%H:%M:%S'), 'msg': label, 'detail': line[:100]})
                break
        state['last_update'] = time.time()
    except: pass

def serial_reader():
    while True:
        try:
            ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.5)
            global serial_send; serial_send = ser
            buf = b""
            while True:
                if ser.in_waiting: buf += ser.read(ser.in_waiting)
                else: time.sleep(0.02)
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    try: parse_line(line.decode('utf-8', errors='replace').strip())
                    except: pass
        except Exception as e:
            print(f"[串口] 等待 ({e})"); time.sleep(2)

HTML = r"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BSD 调试控制台 V2</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0d1117;color:#c9d1d9;font:13px monospace;padding:15px}
h1{color:#58a6ff;font-size:18px;margin-bottom:15px}
h1 span{color:#8b949e;font-size:12px;font-weight:normal;margin-left:10px}
.tabs{display:flex;gap:2px;margin-bottom:15px}
.tab{padding:8px 16px;background:#161b22;border:1px solid #30363d;border-radius:6px 6px 0 0;cursor:pointer;color:#8b949e}
.tab.active{background:#21262d;color:#c9d1d9;border-bottom-color:#21262d}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px}
.card h2{font-size:12px;color:#8b949e;margin-bottom:8px;text-transform:uppercase;letter-spacing:1px}
.card h2 .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.dot.green{background:#3fb950;box-shadow:0 0 6px #3fb950}
.dot.red{background:#f85149}
.dot.yellow{background:#d2991d}
.val{font-size:26px;font-weight:bold}
.green{color:#3fb950}.red{color:#f85149}.yellow{color:#d2991d}.blue{color:#58a6ff}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0}
.badge{padding:2px 8px;border-radius:4px;font-size:11px;font-weight:bold;min-width:50px;text-align:center}
.badge.on{background:#3fb950;color:#000}
.badge.off{background:#30363d;color:#8b949e}
.badge.warn{background:#d2991d;color:#000}
.target{border-left:3px solid #58a6ff;padding:6px 10px;margin:4px 0;background:#21262d;border-radius:0 4px 4px 0}
.target.left{border-left-color:#f85149}
.target.right{border-left-color:#3fb950}
.bar{height:6px;background:#30363d;border-radius:3px;margin:4px 0;overflow:hidden}
.bar div{height:100%;border-radius:3px;transition:width 0.3s}
.hex{font-size:11px;color:#8b949e;word-break:break-all;line-height:1.6;max-height:120px;overflow-y:auto}
table{width:100%;font-size:12px}
th{text-align:left;color:#8b949e;padding:4px 8px;border-bottom:1px solid #30363d}
td{padding:4px 8px}
input,select{background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:4px 8px;border-radius:4px;width:80px;font:12px monospace}
label{color:#8b949e;font-size:11px}
.event{padding:4px 0;border-bottom:1px solid #21262d;font-size:11px}
.event .time{color:#58a6ff;margin-right:8px}
.panel{display:none}
.panel.active{display:block}
</style></head><body>
<h1>🔵 BSD 调试控制台 <span id="conn">● 已连接</span></h1>
<div class="tabs">
    <div class="tab active" onclick="showTab('main')">📊 主面板</div>
    <div class="tab" onclick="showTab('raw')">📝 原始数据</div>
    <div class="tab" onclick="showTab('events')">📋 事件日志</div>
    <div class="tab" onclick="showTab('settings')">⚙ 设置</div>
    <div class="tab" onclick="showTab('commands')">📡 命令</div>
</div>

<div class="panel active" id="panel-main">
<div class="grid">
    <div class="card"><h2><span class="dot green" id="dot_radar"></span>📡 雷达状态</h2>
        <div class="row"><span>累计字节</span><span class="val blue" id="rx">0</span></div>
        <div class="row"><span>目标数</span><span class="val green" id="tgt">0</span></div>
        <div class="row"><span>帧校验</span><span id="valid" class="badge off">-</span></div>
        <div class="row"><span>运行时间</span><span id="uptime">0s</span></div>
        <div id="targets"></div>
    </div>
    <div class="card"><h2>💡 输出设备</h2>
        <div class="row"><span>🔴 左RCW LED</span><span id="led_l" class="badge off">OFF</span></div>
        <div class="row"><span>🟢 右RCW LED</span><span id="led_r" class="badge off">OFF</span></div>
        <div class="row" style="margin-top:6px"><span>🔔 蜂鸣器</span><span id="buzzer" class="badge off">OFF</span></div>
        <div class="row"><span>🚗 转向</span><span id="turn" class="val" style="font-size:18px">OFF</span></div>
        <div class="row"><span>⚠ 左RCW</span><span id="rcw_l" class="badge off">-</span></div>
        <div class="row"><span>⚠ 右RCW</span><span id="rcw_r" class="badge off">-</span></div>
    </div>
    <div class="card"><h2>📐 目标详情</h2>
        <div id="target_detail"></div>
    </div>
</div></div>

<div class="panel" id="panel-raw">
    <div class="card"><h2>📝 原始 HEX 数据</h2>
        <div class="row"><span>字节数</span><span id="raw_len">0</span></div>
        <div class="hex" id="raw_hex">等待数据...</div>
    </div>
</div>

<div class="panel" id="panel-events">
    <div class="card"><h2>📋 事件日志</h2>
        <div id="events_list"></div>
    </div>
</div>

<div class="panel" id="panel-settings">
    <div class="card"><h2>⚙ BSD 参数</h2>
        <table>
        <tr><td><label>左盲区最小角</label></td><td><input id="s_bl_min" value="-40" onchange="saveSetting('blind_spot_l_min',this.value)"> °</td></tr>
        <tr><td><label>左盲区最大角</label></td><td><input id="s_bl_max" value="-15" onchange="saveSetting('blind_spot_l_max',this.value)"> °</td></tr>
        <tr><td><label>右盲区最小角</label></td><td><input id="s_br_min" value="15" onchange="saveSetting('blind_spot_r_min',this.value)"> °</td></tr>
        <tr><td><label>右盲区最大角</label></td><td><input id="s_br_max" value="40" onchange="saveSetting('blind_spot_r_max',this.value)"> °</td></tr>
        <tr><td><label>LED超时</label></td><td><input id="s_led_to" value="3000" onchange="saveSetting('led_timeout_ms',this.value)"> ms</td></tr>
        <tr><td><label>RCW速度阈值</label></td><td><input id="s_rcw_spd" value="4" onchange="saveSetting('rcw_speed_threshold',this.value)"> m/s</td></tr>
        <tr><td><label>RCW距离上限</label></td><td><input id="s_rcw_rng" value="20" onchange="saveSetting('rcw_range_limit',this.value)"> m</td></tr>
        </table>
        <div style="margin-top:10px;color:#8b949e;font-size:11px">修改后需重新上传固件生效 (设置仅用于参考)</div>
    </div>
</div>

<script>
function showTab(name) {
    document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
    document.getElementById('panel-'+name).classList.add('active');
    event.target.classList.add('active');
}
async function poll() {
    try {
        const r=await fetch('/state'); const s=await r.json();
        // Main
        document.getElementById('rx').textContent=s.rx_bytes;
        document.getElementById('tgt').textContent=s.bsd_targets;
        const v=document.getElementById('valid');
        v.textContent=s.bsd_valid?'✅ PASS':'❌ FAIL';
        v.className='badge '+(s.bsd_valid?'on':'off');
        document.getElementById('dot_radar').className='dot '+(s.bsd_valid?'green':'red');
        document.getElementById('uptime').textContent=Math.floor(s.uptime)+'s';
        // Targets
        let th=''; let td='';
        for(const t of (s.targets||[])) {
            const side=t.angle<0?'⬅ 左':'➡ 右';
            const cls=t.angle<0?'left':'right';
            th+=`<div class="target ${cls}">${side} | ${t.range}m | ${t.angle}° | ${t.velocity}m/s</div>`;
            const danger=t.velocity<0?'靠近':'远离';
            td+=`<div class="target ${cls}">
                <div class="row"><span>距离</span><span class="val" style="font-size:20px">${t.range}<small>m</small></span></div>
                <div class="bar"><div style="width:${Math.min(t.range/50*100,100)}%;background:${t.range<15?'#f85149':'#3fb950'}"></div></div>
                <div class="row"><span>角度</span><span style="color:${t.angle<0?'#f85149':'#3fb950'};font-size:18px">${t.angle}° ${side}</span></div>
                <div class="row"><span>速度</span><span style="color:${t.velocity<0?'#f85149':'#8b949e'}">${t.velocity} m/s (${danger})</span></div>
            </div>`;
        }
        document.getElementById('targets').innerHTML=th||'<div style="color:#8b949e;padding:8px">无目标</div>';
        document.getElementById('target_detail').innerHTML=td||'<div style="color:#8b949e;padding:8px">等待检测...</div>';
        // LEDs
        ['l','r'].forEach(x=>{
            const el=document.getElementById('led_'+x);
            const on=s['indicator_'+x];
            el.textContent=on?'🟢 ON':'⚫ OFF';
            el.className='badge '+(on?'on':'off');
        });
        // Buzzer
        const bz=document.getElementById('buzzer');
        bz.textContent=s.buzzer>0?['','🔊 短鸣','🚨 连续报警'][s.buzzer]||'ON':'OFF';
        bz.className='badge '+(s.buzzer>0?(s.buzzer>1?'warn':'on'):'off');
        // Turn
        document.getElementById('turn').textContent={'OFF':'OFF','L':'⬅ 左转','R':'➡ 右转','HAZ':'⚠ 双闪'}[s.turn]||s.turn;
        // RCW
        ['l','r'].forEach(x=>{
            const el=document.getElementById('rcw_'+x);
            el.textContent=s['rcw_'+x]?'⚠ ACTIVE':'-';
            el.className='badge '+(s['rcw_'+x]?'warn':'off');
        });
        // Raw
        document.getElementById('raw_len').textContent=s.raw_len+' bytes';
        document.getElementById('raw_hex').textContent=s.raw_hex||'等待数据...';
        // Events
        let ev='';
        for(const e of (s.events||[]).slice(0,30)){
            ev+=`<div class="event"><span class="time">${e.time}</span>${e.msg}: ${e.detail}</div>`;
        }
        document.getElementById('events_list').innerHTML=ev||'<div style="color:#8b949e">暂无事件</div>';
        // Connection
        const age=(Date.now()/1000)-s.last_update;
        const conn=document.getElementById('conn');
        conn.textContent=age<3?'● 已连接':age<10?'● 延迟 '+Math.floor(age)+'s':'● 断开';
        conn.style.color=age<3?'#3fb950':age<10?'#d2991d':'#f85149';
    }catch(e){}
}
function saveSetting(key,val) { fetch('/settings?key='+key+'&val='+val); }
setInterval(poll,400); poll();

function sendHex(hex) {
    document.getElementById('cmd_hex').value = hex;
    sendCmd();
}
function sendBeep() {
    fetch('/send?beep=1').then(r=>r.json()).then(d=>{
        document.getElementById('cmd_result').textContent = d.ok ? '🔔 蜂鸣已触发' : '❌ 失败';
    });
}
function sendCmd() {
    const hex = document.getElementById('cmd_hex').value.replace(/\s/g,'');
    if (!hex) return;
    fetch('/send?hex=' + hex).then(r=>r.json()).then(d=>{
        document.getElementById('cmd_result').textContent = d.ok ? '✅ 已发送 ' + hex : '❌ 失败';
    });
}

</script>
<div class="panel" id="panel-commands">
    <div class="card"><h2>📡 AT6010 命令发送</h2>
        <div style="margin-bottom:8px">
            <input id="cmd_hex" placeholder="58D101012B01" style="width:250px;font-size:14px" onkeypress="if(event.key=='Enter')sendCmd()">
            <button onclick="sendBeep()" style="background:#d2991d;color:#000;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;font:13px monospace;margin-right:8px">🔔 蜂鸣测试</button>
            <button onclick="sendCmd()" style="background:#238636;color:white;border:none;padding:6px 16px;border-radius:4px;cursor:pointer;font:13px monospace">发送</button>
        </div>
        <div id="cmd_result" style="color:#58a6ff;margin:8px 0;font-size:12px"></div>
    </div>
    <div class="card"><h2>📋 常用命令</h2>
        <table style="font-size:12px">
        <tr><th>命令</th><th>Hex</th><th>说明</th><th></th></tr>
        <tr><td>查询版本</td><td><code>58FE005601</code></td><td>返回软硬件版本</td>
            <td><button onclick="sendHex('58FE005601')" style="background:#21262d;color:#58a6ff;border:1px solid #30363d;padding:2px 8px;border-radius:3px;cursor:pointer">▶</button></td></tr>
        <tr><td>打开感应</td><td><code>58D101012B01</code></td><td>打开雷达检测</td>
            <td><button onclick="sendHex('58D101012B01')">▶</button></td></tr>
        <tr><td>关闭感应</td><td><code>58D101002A01</code></td><td>关闭雷达检测</td>
            <td><button onclick="sendHex('58D101002A01')">▶</button></td></tr>
        <tr><td>等级=0(最灵敏)</td><td><code>580201005B00</code></td><td>Level 0 最高灵敏度</td>
            <td><button onclick="sendHex('580201005B00')">▶</button></td></tr>
        <tr><td>等级=15</td><td><code>5802010F6A00</code></td><td>Level 15 最低灵敏度</td>
            <td><button onclick="sendHex('5802010F6A00')">▶</button></td></tr>
        <tr><td>系统复位</td><td><code>581301016D00</code></td><td>雷达重启</td>
            <td><button onclick="sendHex('581301016D00')">▶</button></td></tr>
        <tr><td>保存设置</td><td><code>580801016200</code></td><td>写入Flash</td>
            <td><button onclick="sendHex('580801016200')">▶</button></td></tr>
        <tr><td>获取检测信息</td><td><code>5830008800</code></td><td>CMD 0x30 轮询</td>
            <td><button onclick="sendHex('5830008800')">▶</button></td></tr>
        <tr><td>运动灵敏度=1</td><td><code>583501018F00</code></td><td>0~10, 越低越灵敏</td>
            <td><button onclick="sendHex('583501018F00')">▶</button></td></tr>
        <tr><td>最远距离=50m</td><td><code>58D20288132F01</code></td><td>5000cm</td>
            <td><button onclick="sendHex('58D20288132F01')">▶</button></td></tr>
        </table>
    </div>
    <div class="card"><h2>📖 完整命令列表</h2>
        <table style="font-size:11px">
        <tr><th>指令码</th><th>名称</th><th>格式</th></tr>
        <tr><td>0x02</td><td>设置感应等级</td><td>58 02 01 [level 0-15] [csum2B]</td></tr>
        <tr><td>0x03</td><td>获取感应等级</td><td>58 03 00 [csum2B]</td></tr>
        <tr><td>0x04</td><td>设置OUT持续时间</td><td>58 04 02 [time 2B LE] [csum]</td></tr>
        <tr><td>0x08</td><td>保存设置</td><td>58 08 01 [0=不保存,1=保存] [csum]</td></tr>
        <tr><td>0x13</td><td>系统复位</td><td>58 13 01 [01] [csum]</td></tr>
        <tr><td>0x19</td><td>切换波特率</td><td>58 19 04 [baud 4B LE] [csum]</td></tr>
        <tr><td>0x30</td><td>获取检测信息</td><td>58 30 00 [csum]</td></tr>
        <tr><td>0x31</td><td>获取算法类型</td><td>58 31 00 [csum]</td></tr>
        <tr><td>0x32</td><td>获取边界值</td><td>58 32 00 [csum]</td></tr>
        <tr><td>0x34</td><td>设置运动最近距离</td><td>58 34 02 [cm 2B LE] [csum]</td></tr>
        <tr><td>0x35</td><td>设置运动灵敏度</td><td>58 35 01 [0-10] [csum]</td></tr>
        <tr><td>0xD0</td><td>获取感应状态</td><td>58 D0 00 [csum]</td></tr>
        <tr><td>0xD1</td><td>设置感应开关</td><td>58 D1 01 [0=关,1=开] [csum]</td></tr>
        <tr><td>0xD2</td><td>设置运动最远距离</td><td>58 D2 02 [cm 2B LE] [csum]</td></tr>
        <tr><td>0xFE</td><td>获取软硬件版本</td><td>58 FE 00 [csum]</td></tr>
        </table>
    </div>
</div>

</body></html>"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/state'):
            self._json(state)
        elif self.path.startswith('/send'):
            qs = self.path.split('?',1)[-1] if '?' in self.path else ''
            # BEEP test
            if 'beep=1' in qs:
                if serial_send:
                    serial_send.write(b'BEEP\n')
                    self._json({"ok": True, "msg": "BEEP sent"})
                else:
                    self._json({"ok": False, "error": "no serial"})
                return
            # Send hex command
            hex_str = ''
            for part in qs.split('&'):
                if part.startswith('hex='):
                    hex_str = part[4:]
            if hex_str and serial_send:
                try:
                    cmd = bytes.fromhex(hex_str)
                    serial_send.write(('CMD:'+hex_str+'\n').encode())
                    self._json({"ok": True, "len": len(cmd)})
                except Exception as e:
                    self._json({"ok": False, "error": str(e)})
            else:
                self._json({"ok": False, "error": "no serial"})
        elif self.path.startswith('/settings'):
            # Store setting
            qs = self.path.split('?',1)[-1] if '?' in self.path else ''
            for part in qs.split('&'):
                if '=' in part:
                    k,v = part.split('=',1)
                    try: v=int(v)
                    except: pass
                    if k in state['settings']: state['settings'][k] = v
            self._json({"ok": True})
        else:
            self.send_response(200)
            self.send_header('Content-Type','text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(HTML.encode())
    
    def _json(self, data):
        self.send_response(200)
        self.send_header('Content-Type','application/json')
        self.send_header('Access-Control-Allow-Origin','*')
        self.end_headers()
        # Convert non-serializable objects
        clean = {}
        for k, v in data.items():
            if hasattr(v, '__iter__') and not isinstance(v, (str, dict, list)):
                v = list(v)
            clean[k] = v
        self.wfile.write(json.dumps(clean, ensure_ascii=False).encode())

if __name__ == '__main__':
    t = threading.Thread(target=serial_reader, daemon=True)
    t.start()
    print(f"\n🔵 BSD 调试控制台 V2: http://localhost:{WEB_PORT}\n")
    http.server.HTTPServer(('0.0.0.0', WEB_PORT), Handler).serve_forever()
