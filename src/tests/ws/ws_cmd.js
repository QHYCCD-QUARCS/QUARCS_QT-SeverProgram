// 复现：连接 INDI 驱动后，相机能否连上 / 是否出现候选列表
// 协议：QT client 期望 {"type":"Vue_Command","message":"<cmd>","msgid":"<id>"}
// 用法: node ws_connect_test.js <host> <cmd> [观察秒数]
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');

const host = process.argv[2] || '172.24.217.51';
const cmd  = process.argv[3] || 'ConnectDriver:indi_qhy_ccd:MainCamera';
const secs = parseInt(process.argv[4] || '45', 10);
const url  = `ws://${host}:8600`;

const ws = new WebSocket(url);
const interesting = /ConnectDriver|DeviceToBeAllocated|ShowDeviceAllocation|ConnectSuccess|BindDevice|Failed|Error|CCD|Camera|Allocat/i;
const noise = /CPU|Memory|netStatus|Temperature|MainCameraStatus|FPS|localMessage|Client .* (connected|disconnected)/i;
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(5) + 's';

ws.on('open', () => {
  console.log(`[${stamp()}] connected ${url}`);
  const payload = JSON.stringify({ type: 'Vue_Command', message: cmd, msgid: 'wstest-' + Date.now() });
  console.log(`[${stamp()}] >>> ${cmd}`);
  ws.send(payload);
});

ws.on('message', (data) => {
  let s = data.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  if (noise.test(s) && !interesting.test(s)) return;
  if (interesting.test(s)) console.log(`[${stamp()}] <<< ${s.slice(0, 260)}`);
});

ws.on('error', (e) => console.log(`[${stamp()}] ERROR ${e.message}`));

setTimeout(() => { console.log(`[${stamp()}] --- 观察结束 ---`); ws.close(); process.exit(0); }, secs * 1000);
