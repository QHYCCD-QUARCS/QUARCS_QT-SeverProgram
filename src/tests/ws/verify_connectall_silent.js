// 验证 connectAll 的弹窗判据：从「相机分配完没有」改成「角色决策出结果没有」。
// 两个方向都要验，否则容易把弹窗修死：
//   A) 角色都能由持久化信息决策出来 → 静默，不发 ShowDeviceAllocationWindow
//   B) 有角色决策不出来（持久化里记的设备不在场）→ 仍然要弹，否则用户无从指派
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const ws = new WebSocket('ws://172.24.217.51:8600');
let log = [];
const send = (c) => { console.log(`>>> ${c}`); ws.send(JSON.stringify({type:'Vue_Command',message:c,msgid:'vs-'+Date.now()})); };
const wait = (ms) => new Promise(r => setTimeout(r, ms));
ws.on('message', d => { let s=d.toString(); try{const o=JSON.parse(s); if(o&&o.message!=null)s=String(o.message);}catch(e){} log.push(s); });
const grab = (re) => log.filter(x => re.test(x));
const uniq = () => [...new Set(grab(/^DeviceToBeAllocated:/).map(x=>x.split(':').slice(0,4).join(':')))];
const results = [];
const check = (n, c, d='') => { results.push({n,ok:!!c,d}); console.log(`   ${c?'✅':'❌'} ${n}${d?'  — '+d:''}`); };

ws.on('open', async () => {
  // ══ 场景 A：两个角色都绑好并持久化 → 断开 → connectAll 应静默 ══
  console.log('\n【A】选好相机(Main+Guider) → 断开全部 → connect all（应静默）');
  send('disconnectAllDevice'); await wait(15000);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1200);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:1');  await wait(1200);
  send('SetConnectionMode:MainCamera:INDI'); await wait(1800);
  send('SetConnectionMode:Guider:INDI'); await wait(1800);
  log=[]; send('ConnectDriver:indi_qhy_ccd:MainCamera'); await wait(16000);
  const c = uniq();
  if (c.length < 2) { check('A 前置：至少2台候选', false, `只有 ${c.length}`); ws.close(); process.exit(1); }
  log=[]; send(`BindingDevice:MainCamera:${c[0].split(':')[2]}`); await wait(9000);
  log=[]; send('ConnectDriver:indi_qhy_ccd:Guider'); await wait(16000);
  log=[]; send(`BindingDevice:Guider:${c[1].split(':')[2]}`); await wait(9000);
  check('A 前置：两个角色都绑好', grab(/^ConnectSuccess:Guider/).length >= 1);

  send('disconnectAllDevice'); await wait(16000);
  log=[]; send('connectAllDevice'); await wait(32000);

  const showWin = grab(/^ShowDeviceAllocationWindow/);
  const bound   = grab(/^ConnectSuccess:(MainCamera|Guider)/);
  console.log(`      connectAll 回包: ShowDeviceAllocationWindow=${showWin.length}, 自动绑上=${bound.length} 个角色, 候选=${uniq().length}`);
  check('A: connectAll 不再弹分配窗（静默）', showWin.length === 0,
        showWin.length ? '⚠ 仍在弹' : '未发送 ShowDeviceAllocationWindow');
  check('A: 两个角色仍按持久化信息自动连上', bound.length >= 2, `${bound.length} 个`);
  check('A: ConnectAllDeviceComplete 正常下发', grab(/ConnectAllDeviceComplete/).length >= 1);

  // ══ 场景 B：把 Guider 解绑并清掉它的持久化选择 → connectAll 时该角色决策不出来 → 应弹 ══
  // 用 UnBindingDevice 清掉绑定名（DeviceIndiName 被清空 → 下次 connectAll 找不到 saved name）
  console.log('\n【B】让一个角色决策不出来 → connect all（应仍然弹，否则用户无从指派）');
  log=[]; send('UnBindingDevice:Guider'); await wait(10000);
  console.log('      已解绑 Guider（其持久化设备名被清空）');
  send('disconnectAllDevice'); await wait(16000);
  log=[]; send('connectAllDevice'); await wait(32000);
  const showWinB = grab(/^ShowDeviceAllocationWindow/);
  const boundB   = grab(/^ConnectSuccess:(MainCamera|Guider)/);
  console.log(`      connectAll 回包: ShowDeviceAllocationWindow=${showWinB.length}, 自动绑上=${boundB.length} 个角色, 候选=${uniq().length}`);
  check('B: 角色决策不出来时仍会弹（弹窗没被修死）', showWinB.length >= 1,
        showWinB.length ? '正常弹出' : '⚠ 不弹了 —— 用户无从指派该角色');

  send('disconnectAllDevice'); await wait(12000);
  const pass = results.filter(r=>r.ok).length, fail = results.filter(r=>!r.ok).length;
  console.log(`\n${'='.repeat(58)}\n汇总: ${pass} 通过 / ${fail} 失败`);
  results.filter(r=>!r.ok).forEach(r=>console.log(`  ❌ ${r.n} — ${r.d}`));
  ws.close(); process.exit(fail?1:0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
