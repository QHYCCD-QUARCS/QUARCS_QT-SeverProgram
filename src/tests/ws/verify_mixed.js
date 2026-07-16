// M3 终极验证：Main=SDK + Guider=INDI 混用，各绑不同相机，同时成立。
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const ws = new WebSocket('ws://172.24.217.51:8600');
const send = (c) => { console.log(`\n>>> ${c}`); ws.send(JSON.stringify({ type: 'Vue_Command', message: c, msgid: 'mx-' + Date.now() })); };
const wait = (ms) => new Promise(r => setTimeout(r, ms));

let log = [];
ws.on('message', (d) => {
  let s = d.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  log.push(s);
});
const grab = (re) => log.filter(x => re.test(x));

ws.on('open', async () => {
  send('disconnectAllDevice'); await wait(15000);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1500);   // MainCamera
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:1');  await wait(1500);   // Guider

  // ── 关键：设成混用（以前这一步会被锁挡住 / 被联动拽回同模式）──
  console.log('\n========== 阶段1：设 Main=SDK, Guider=INDI（混用）==========');
  log = [];
  send('SetConnectionMode:MainCamera:SDK'); await wait(2500);
  send('SetConnectionMode:Guider:INDI');    await wait(2500);
  console.log('  Success 回包:', JSON.stringify(grab(/^SetConnectionModeSuccess:/)));
  console.log('  Failed  回包:', JSON.stringify(grab(/^SetConnectionModeFailed:/)));
  console.log('  << 期望：各 1 条 Success（Main:SDK / Guider:INDI），无 Failed，且互不干扰');

  // ── 阶段2：SDK 连 MainCamera 并绑一台真机 ──
  console.log('\n========== 阶段2：SDK 连 MainCamera + 绑 QHY247C ==========');
  log = [];
  send('ConnectDriver:indi_qhy_ccd:MainCamera:SDK'); await wait(16000);
  let cands = grab(/^DeviceToBeAllocated:/);
  console.log(`  SDK 候选 ${cands.length} 个`); cands.forEach(x => console.log('     ', x));
  const sdkPick = cands.find(x => /QHY247C/.test(x));
  if (!sdkPick) { console.log('!! 找不到 QHY247C 候选'); ws.close(); process.exit(0); }
  log = [];
  send(`BindingDevice:MainCamera:${sdkPick.split(':')[2]}`); await wait(12000);
  console.log('  按需打开:', JSON.stringify(grab(/opened on demand/).map(x => x.replace(/^.*\| /, ''))));
  console.log('  ConnectSuccess:', JSON.stringify(grab(/^ConnectSuccess:/)));

  // ── 阶段3：INDI 连 Guider（此时 SDK 已占着 QHY247C）──
  console.log('\n========== 阶段3：INDI 连 Guider（SDK 正占用 QHY247C）==========');
  log = [];
  send('ConnectDriver:indi_qhy_ccd:Guider'); await wait(20000);
  cands = grab(/^DeviceToBeAllocated:/);
  console.log(`  INDI 候选 ${cands.length} 个`); cands.forEach(x => console.log('     ', x));
  console.log('  Failed:', JSON.stringify(grab(/^ConnectDriverFailed:/)));
  console.log('  Pending:', JSON.stringify(grab(/PendingAllocation/)));
  const indiPick = cands.find(x => /5III/.test(x));
  if (indiPick) {
    log = [];
    console.log(`\n  -- 绑 Guider -> ${indiPick.split(':')[3]}`);
    send(`BindingDevice:Guider:${indiPick.split(':')[2]}`); await wait(12000);
    console.log('  ConnectSuccess:', JSON.stringify(grab(/^ConnectSuccess:/)));
    console.log('  BindDeviceTypeList:', JSON.stringify(grab(/^BindDeviceTypeList:/)));
  }

  console.log('\n========== 结论 ==========');
  console.log('若 Main 绑到 SDK 的 QHY247C、Guider 绑到 INDI 的 5III 且都 ConnectSuccess，则混用成立。');
  ws.close(); process.exit(0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
