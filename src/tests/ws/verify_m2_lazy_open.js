// M2 核心断言验证：SDK 连接不 open 任何相机；只有被绑定的那台才 open。
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const ws = new WebSocket('ws://172.24.217.51:8600');
const send = (c) => { console.log(`\n>>> ${c}`); ws.send(JSON.stringify({ type: 'Vue_Command', message: c, msgid: 'm2-' + Date.now() })); };
const wait = (ms) => new Promise(r => setTimeout(r, ms));

let log = [];
ws.on('message', (d) => {
  let s = d.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  log.push(s);
});
const grab = (re) => log.filter(x => re.test(x));

ws.on('open', async () => {
  send('disconnectAllDevice'); await wait(14000);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1500);
  send('SetConnectionMode:MainCamera:SDK'); await wait(2500);

  // ── 阶段1：SDK 连接（期望：登记但不 open）──
  log = [];
  console.log('\n===== 阶段1：SDK 连接 MainCamera =====');
  send('ConnectDriver:indi_qhy_ccd:MainCamera:SDK');
  await wait(16000);
  const regs  = grab(/registered \(not opened\)/);
  const opens = grab(/opened on demand/);
  const cands = grab(/^DeviceToBeAllocated:/);
  console.log(`  登记(未打开) : ${regs.length} 台`);
  regs.forEach(x => console.log(`      ${x.replace(/^.*\|/, '').trim()}`));
  console.log(`  按需打开     : ${opens.length} 台  <<< 期望 0`);
  console.log(`  上报候选     : ${cands.length} 个  <<< 期望 4`);
  cands.forEach(x => console.log(`      ${x}`));
  console.log(`  PendingAllocation: ${grab(/PendingAllocation/).length}`);

  if (!cands.length) { console.log('!! 无候选，中止'); ws.close(); process.exit(0); }

  // ── 阶段2：绑定一台（期望：只 open 这一台）──
  const pick = cands.find(x => /QHY247C/.test(x)) || cands[0];
  const uiIdx = pick.split(':')[2];
  console.log(`\n===== 阶段2：绑定 MainCamera -> ${uiIdx} (${pick.split(':')[3]}) =====`);
  log = [];
  send(`BindingDevice:MainCamera:${uiIdx}`);
  await wait(12000);
  const opens2 = grab(/opened on demand/);
  console.log(`  按需打开 : ${opens2.length} 台  <<< 期望正好 1`);
  opens2.forEach(x => console.log(`      ${x.replace(/^.*\|/, '').trim()}`));
  console.log(`  ConnectSuccess: ${JSON.stringify(grab(/^ConnectSuccess:/))}`);

  console.log('\n===== 结束 =====');
  send('disconnectAllDevice'); await wait(10000);
  ws.close(); process.exit(0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
