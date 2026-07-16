// 顺序覆盖测试（SDK 传输侧）——与 order_coverage.js / order_coverage_edge.js 互补。
// 那两个主要跑 INDI；本文件把同样的顺序在 SDK 上再跑一遍，并**对比两条传输的行为**。
//
// 存在的意义：连接逻辑有大量代码是两条传输共用的（如 ConnectDriver 的"单设备兜底"），
// 只在 INDI 上验证过不等于 SDK 也对；而两条传输行为不一致本身就是缺陷
// （同一个动作，结果取决于走哪条传输）。
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const HOST = process.argv[2] || '172.24.217.51';
const ws = new WebSocket(`ws://${HOST}:8600`);

let log = [];
const send = (c) => ws.send(JSON.stringify({ type: 'Vue_Command', message: c, msgid: 'sdk-' + Date.now() + '-' + Math.random() }));
const wait = (ms) => new Promise(r => setTimeout(r, ms));
ws.on('message', (d) => {
  let s = d.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  log.push(s);
});
const grab = (re) => log.filter(x => re.test(x));
const uniq = () => [...new Set(grab(/^DeviceToBeAllocated:/).map(x => x.split(':').slice(0, 4).join(':')))];
const realFailures = () => grab(/Failed/).filter(x => !/bit depth/.test(x));

const results = [];
const check = (n, c, d = '') => { results.push({ n, ok: !!c, d }); console.log(`   ${c ? '✅' : '❌'} ${n}${d ? '  — ' + d : ''}`); };

const reset = async () => { log = []; send('disconnectAllDevice'); await wait(15000); };
const setup = async () => {
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1200);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:1'); await wait(1200);
};
const mode = async (r, m) => { send(`SetConnectionMode:${r}:${m}`); await wait(2000); };
const connSdk = async (r) => { send(`ConnectDriver:indi_qhy_ccd:${r}:SDK`); await wait(16000); };
const bind = async (r, i) => { send(`BindingDevice:${r}:${i}`); await wait(9000); };

let SDK_BASE = 0;

async function baseline() {
  console.log('\n【基线】SDK 一次连接看到几台');
  await reset(); await setup(); await mode('MainCamera', 'SDK');
  log = []; await connSdk('MainCamera');
  SDK_BASE = uniq().length;
  check('SDK 基线可测得', SDK_BASE > 0, `SDK 候选=${SDK_BASE}`);
  // SDK 的 UI 索引是负数（-(poolIndex+1)），与 INDI 的正索引区分开
  const idxs = uniq().map(x => x.split(':')[2]);
  check('SDK 候选用负索引(与INDI正索引区分)', idxs.every(i => Number(i) < 0), `索引: ${idxs.join(',')}`);
}

// 这条是本文件的核心：ConnectDriver 的"单设备兜底"是两条传输共用的代码，
// 8ac3e29 只在 INDI 上验证过"解绑后重连能重新拿到候选"，SDK 侧未验证。
async function K1_sdk_bind_unbind_reconnect() {
  console.log('\n【K1】SDK：绑定 → 解绑 → 再连 → 再绑（8ac3e29 修复在 SDK 侧是否同样成立）');
  await reset(); await setup(); await mode('MainCamera', 'SDK');
  log = []; await connSdk('MainCamera'); const c = uniq();
  if (!c.length) { check('K1 前置：SDK 有候选', false); return; }
  log = []; await bind('MainCamera', c[0].split(':')[2]);
  check('SDK 绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; send('UnBindingDevice:MainCamera'); await wait(9000);
  check('SDK 解绑无Failed', realFailures().length === 0, JSON.stringify(realFailures().slice(0, 2)));
  check('SDK 解绑后相机回到候选', uniq().length >= 1, `解绑回包候选 ${uniq().length}`);
  log = []; await connSdk('MainCamera');
  check('SDK 解绑后重连候选==基线', uniq().length === SDK_BASE, `${uniq().length} vs ${SDK_BASE}`);
  const c2 = uniq();
  if (c2.length) {
    log = []; await bind('MainCamera', c2[0].split(':')[2]);
    check('SDK 解绑后可重新绑定', grab(/^ConnectSuccess:MainCamera/).length === 1);
  } else {
    check('SDK 解绑后可重新绑定', false, '无候选可绑');
  }
}

// 行为对比：同一个动作在两条传输下应当给出同样的结果。
// INDI 侧实测是【改绑】（新角色接管、旧角色清空，见 order_coverage_edge.js E6）。
// SDK 侧读码看到 sdkOccupantRoleOfPoolIndex 像是【拒绝】——先实测，不臆断。
async function K2_sdk_two_roles_same_camera() {
  console.log('\n【K2】SDK：两个角色抢同一台相机 —— 与 INDI 的【改绑】语义是否一致？');
  await reset(); await setup(); await mode('MainCamera', 'SDK'); await mode('Guider', 'SDK');
  log = []; await connSdk('MainCamera'); const c = uniq();
  if (!c.length) { check('K2 前置：SDK 有候选', false); return; }
  const idx = c[0].split(':')[2], name = c[0].split(':')[3];
  log = []; await bind('MainCamera', idx);
  check('SDK Main绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; await connSdk('Guider'); await wait(1000);
  log = []; await bind('Guider', idx);          // 同一台！
  const guiderOk = grab(/^ConnectSuccess:Guider/).length >= 1;
  const rejected = realFailures().length >= 1 || grab(/already|Used|occupied|占用|Occupant/i).length >= 1;
  const btl = grab(/^BindDeviceTypeList:/).slice(-1)[0] || '';
  console.log(`      Guider 结果: ConnectSuccess=${guiderOk ? 1 : 0}, 拒绝迹象=${rejected}`);
  console.log(`      BindDeviceTypeList: ${btl.slice(0, 100)}`);

  // 无论哪种语义，安全不变量都必须成立：一台相机不能同时挂两个角色
  const bothBound = btl.includes(`Guider:${name}`) && btl.includes(`MainCamera:${name}`);
  check('不变量：一台相机不同时挂两个角色(SDK)', !bothBound,
        bothBound ? '⚠ 双开风险：Main 与 Guider 同时持有同一台' : 'OK');

  // 行为一致性：INDI 是改绑；SDK 若是拒绝 → 两条传输行为不一致（缺陷）
  check('行为与INDI一致(应为【改绑】：新角色接管)', guiderOk && !btl.includes(`MainCamera:${name}`),
        guiderOk ? '与INDI一致：改绑' : `与INDI不一致：SDK 拒绝了（INDI 会改绑）`);
}

async function K3_sdk_repeat_connect() {
  console.log('\n【K3】SDK：重复点连接 ×3（幂等/不叠加）');
  await reset(); await setup(); await mode('MainCamera', 'SDK');
  log = [];
  await connSdk('MainCamera'); await connSdk('MainCamera'); await connSdk('MainCamera');
  check('SDK 重复连接候选==基线(幂等)', uniq().length === SDK_BASE, `${uniq().length} vs ${SDK_BASE}`);
  check('SDK 重复连接无Failed', realFailures().length === 0, JSON.stringify(realFailures().slice(0, 1)));
}

async function K4_sdk_disconnect_reconnect() {
  console.log('\n【K4】SDK：连→绑→断开→再连（断开残留）');
  await reset(); await setup(); await mode('MainCamera', 'SDK');
  log = []; await connSdk('MainCamera'); const c = uniq();
  if (c.length) { log = []; await bind('MainCamera', c[0].split(':')[2]); }
  await reset(); await setup(); await mode('MainCamera', 'SDK');
  log = []; await connSdk('MainCamera');
  check('SDK 断开后重连候选==基线(无残留)', uniq().length === SDK_BASE, `${uniq().length} vs ${SDK_BASE}`);
  check('SDK 断开后重连无Failed', realFailures().length === 0);
}

ws.on('open', async () => {
  console.log('connected', HOST);
  const t0 = Date.now();
  try {
    await baseline();
    await K1_sdk_bind_unbind_reconnect();
    await K2_sdk_two_roles_same_camera();
    await K3_sdk_repeat_connect();
    await K4_sdk_disconnect_reconnect();
  } catch (e) { console.log('EXC', e.message); }
  const pass = results.filter(r => r.ok).length, fail = results.filter(r => !r.ok).length;
  console.log(`\n${'='.repeat(62)}\n汇总(SDK): ${pass} 通过 / ${fail} 失败  (${((Date.now() - t0) / 1000).toFixed(0)}s, SDK基线=${SDK_BASE})`);
  results.filter(r => !r.ok).forEach(r => console.log(`  ❌ ${r.n} — ${r.d}`));
  await reset();
  ws.close();
  process.exit(fail ? 1 : 0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
