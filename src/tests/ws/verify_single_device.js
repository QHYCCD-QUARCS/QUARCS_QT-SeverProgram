// 第 2 步（删掉「只有一台就绑」特判）的真实覆盖测试。
//
// 本机 CCD=4、TELESCOPE/FOCUSER/FILTER 都是 0，删掉的 8 处分支一处都执行不到，
// 常规回归证明不了它们。用 INDI 模拟器造出「恰好一台」的场景把这条路跑通。
//
// 用 indi_simulator_telescope 绑到 Mount 槽位(0)，验两个方向：
//   S1 无持久化记录（首次使用）→ 不自动绑，但【必须】上报候选 → 用户可手动指派
//                                （若既不绑又不报 = 死路，本次改动最大的风险）
//   S2 手动绑一次后再 connectAll → 靠持久化回放自动绑上（机制换了，结果不变）
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const ws = new WebSocket('ws://172.24.217.51:8600');
let log = [];
const send = (c) => { console.log(`>>> ${c}`); ws.send(JSON.stringify({type:'Vue_Command',message:c,msgid:'sd-'+Date.now()})); };
const wait = (ms) => new Promise(r => setTimeout(r, ms));
ws.on('message', d => { let s=d.toString(); try{const o=JSON.parse(s); if(o&&o.message!=null)s=String(o.message);}catch(e){} log.push(s); });
const grab = (re) => log.filter(x => re.test(x));
const results = [];
const check = (n, c, d='') => { results.push({n,ok:!!c,d}); console.log(`   ${c?'✅':'❌'} ${n}${d?'  — '+d:''}`); };

ws.on('open', async () => {
  send('disconnectAllDevice'); await wait(16000);
  // Mount 槽位 = DeviceSlot::Mount = 0
  send('ConfirmIndiDriver:indi_simulator_telescope:9600:0'); await wait(2000);

  // ══ S1：首次使用（无持久化记录）→ 应上报候选、不自动绑 ══
  console.log('\n【S1】模拟器赤道仪 + 无持久化记录 → 应报候选让用户手动指派（不能是死路）');
  log=[]; send('connectAllDevice'); await wait(35000);
  const tCount = grab(/Number of Connected TELESCOPE/).slice(-1)[0] || '';
  console.log(`      ${tCount.replace(/^.*\|/,'').trim()}`);
  const onlyOneLog = grab(/Mount device is only one/);
  const cand = grab(/^DeviceToBeAllocated:Mount/);
  const bySaved = grab(/Mount auto-bound by saved name/);
  console.log(`      旧「只有一台就绑」日志=${onlyOneLog.length}, 候选上报=${cand.length}, 持久化回放=${bySaved.length}`);
  cand.slice(0,3).forEach(x=>console.log('        ', x.slice(0,95)));

  const got1 = /TELESCOPE:1/.test(tCount);
  check('S1 前置：模拟器赤道仪已连上（恰好 1 台，能走到目标分支）', got1, tCount.replace(/^.*\|/,'').trim());
  if (!got1) { console.log('      !! 没连上，后续无意义'); ws.close(); process.exit(1); }
  check('S1 旧「只有一台就绑」特判已不再执行', onlyOneLog.length === 0);
  check('S1 【关键】未自动绑时必须上报候选（否则死路）', cand.length >= 1 || bySaved.length >= 1,
        cand.length ? `候选 ${cand.length} 条，用户可手动指派` : bySaved.length ? '持久化回放绑上' : '⚠ 既不绑也不报 = 死路');

  // ══ S2：手动绑一次 → 再 connectAll → 应靠持久化回放绑上 ══
  if (cand.length >= 1) {
    const idx = cand[0].split(':')[2];
    console.log(`\n【S2】手动绑定 Mount:${idx} → 再 connectAll → 应靠持久化回放自动绑上`);
    log=[]; send(`BindingDevice:Mount:${idx}`); await wait(9000);
    check('S2 手动绑定成功', grab(/^ConnectSuccess:Mount|BindDeviceTypeList:Mount/).length >= 1,
          (grab(/^ConnectSuccess:Mount/)[0]||'').slice(0,80));

    send('disconnectAllDevice'); await wait(16000);
    log=[]; send('connectAllDevice'); await wait(35000);
    const bySaved2 = grab(/Mount auto-bound by saved name/);
    const bound2 = grab(/^ConnectSuccess:Mount|BindDeviceTypeList:Mount/);
    console.log(`      持久化回放日志=${bySaved2.length}, Mount 绑定回包=${bound2.length}`);
    check('S2 再次 connectAll 靠持久化回放自动绑上（机制已换、结果不变）',
          bySaved2.length >= 1 || bound2.length >= 1,
          bySaved2.length ? '走的是 auto-bound by saved name' : bound2.length ? '已绑上' : '⚠ 没绑上——回归到需手动');
  }

  send('disconnectAllDevice'); await wait(12000);
  const pass = results.filter(r=>r.ok).length, fail = results.filter(r=>!r.ok).length;
  console.log(`\n${'='.repeat(58)}\n汇总(单设备路径): ${pass} 通过 / ${fail} 失败`);
  results.filter(r=>!r.ok).forEach(r=>console.log(`  ❌ ${r.n} — ${r.d}`));
  ws.close(); process.exit(fail?1:0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
