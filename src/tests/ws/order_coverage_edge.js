// 顺序覆盖测试（边界/刁钻顺序）——与 order_coverage.js 互补。
// order_coverage.js 覆盖正常顺序；本文件覆盖"用户乱点"的顺序：已连时切模式、
// 快速连打、connectAll 与单连混用、解绑后重连、两个角色抢同一台相机。
//
// 断言全部相对"基线台数"，不硬编码相机数量（qhyccd.ini 的 demo 开关会改变台数）。
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const HOST = process.argv[2] || '172.24.217.51';
const ws = new WebSocket(`ws://${HOST}:8600`);

let log = [];
const send = (c) => ws.send(JSON.stringify({ type: 'Vue_Command', message: c, msgid: 'edge-' + Date.now() + '-' + Math.random() }));
const wait = (ms) => new Promise(r => setTimeout(r, ms));
ws.on('message', (d) => {
  let s = d.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  log.push(s);
});
const grab = (re) => log.filter(x => re.test(x));
// 候选去重：同一台相机可能被多次上报，按 "类型:索引:名字" 去重
const uniq = () => [...new Set(grab(/^DeviceToBeAllocated:/).map(x => x.split(':').slice(0, 4).join(':')))];
// 忽略与连接状态无关的噪声告警
const realFailures = () => grab(/Failed/).filter(x => !/bit depth/.test(x));

const results = [];
const check = (n, c, d = '') => { results.push({ n, ok: !!c, d }); console.log(`   ${c ? '✅' : '❌'} ${n}${d ? '  — ' + d : ''}`); };

const reset = async () => { log = []; send('disconnectAllDevice'); await wait(15000); };
const setup = async () => {
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1200);
  send('ConfirmIndiDriver:indi_qhy_ccd:9600:1'); await wait(1200);
};
const mode = async (r, m) => { send(`SetConnectionMode:${r}:${m}`); await wait(2000); };
const conn = async (r, sdk) => { send(`ConnectDriver:indi_qhy_ccd:${r}${sdk ? ':SDK' : ''}`); await wait(16000); };
const bind = async (r, i) => { send(`BindingDevice:${r}:${i}`); await wait(9000); };

let BASE = 0;

async function baseline() {
  console.log('\n【基线】测出 INDI 一次连接看到几台');
  await reset(); await setup(); await mode('MainCamera', 'INDI');
  log = []; await conn('MainCamera', false);
  BASE = uniq().length;
  check('基线可测得', BASE > 0, `INDI 候选=${BASE}`);
}

async function E1_mode_switch_while_connected() {
  console.log('\n【E1】已连接状态下切模式（应被拒绝，且不破坏已有连接）');
  await reset(); await setup(); await mode('MainCamera', 'INDI');
  log = []; await conn('MainCamera', false); const c = uniq();
  log = []; await bind('MainCamera', c[0].split(':')[2]);
  check('先绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; await mode('MainCamera', 'SDK');
  check('已连接时切模式被拒绝', grab(/^SetConnectionModeFailed/).length >= 1,
        JSON.stringify(grab(/^SetConnectionModeFailed/)));
  log = []; await conn('Guider', false);
  check('被拒后原连接未被破坏(Guider仍可连)', grab(/^ConnectDriverFailed/).length === 0);
}

async function E2_bind_then_switch_other_role() {
  console.log('\n【E2】Main绑定后把 Guider 切到另一模式（混用建立于绑定之后）');
  await reset(); await setup(); await mode('MainCamera', 'INDI'); await mode('Guider', 'INDI');
  log = []; await conn('MainCamera', false); const c = uniq();
  log = []; await bind('MainCamera', c[0].split(':')[2]);
  check('Main(INDI)绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; await mode('Guider', 'SDK');
  check('Main已连时 Guider 仍可切 SDK(混用)', grab(/^SetConnectionModeFailed/).length === 0);
  log = []; await conn('Guider', true);
  check('Guider(SDK)连接未被锁', grab(/ConnectedInOtherMode/).length === 0);
}

async function E3_rapid_fire_connect() {
  console.log('\n【E3】快速连打连接（不等返回，连发3次）');
  await reset(); await setup(); await mode('MainCamera', 'INDI');
  log = [];
  send('ConnectDriver:indi_qhy_ccd:MainCamera'); await wait(300);
  send('ConnectDriver:indi_qhy_ccd:MainCamera'); await wait(300);
  send('ConnectDriver:indi_qhy_ccd:MainCamera'); await wait(22000);
  check('快速连打后候选==基线(无叠加)', uniq().length === BASE, `${uniq().length} vs ${BASE}`);
  check('快速连打无崩溃(仍有回包)', log.length > 0);
}

async function E4_connectAll_then_single() {
  console.log('\n【E4】connectAllDevice → 再单独连一个角色');
  await reset(); await setup(); await mode('MainCamera', 'INDI'); await mode('Guider', 'INDI');
  log = []; send('connectAllDevice'); await wait(25000);
  check('connectAll 有回包', log.length > 0, `候选 ${uniq().length}`);
  log = []; await conn('MainCamera', false);
  check('connectAll后单连不叠加(≤基线)', uniq().length <= BASE, `${uniq().length} vs ${BASE}`);
  check('connectAll后单连无Failed', grab(/^ConnectDriverFailed/).length === 0);
}

// 解绑只退出"角色分配层"，驱动仍在跑（isConnect 保持 true）。
// 因此再点连接必须重新上报候选——否则该角色再也分配不上（历史 bug：兜底逻辑
// 见 isConnect 就 return，把候选上报一并跳过了）。
async function E5_bind_unbind_rebind() {
  console.log('\n【E5】绑定 → 解绑 → 再连 → 再绑（同一角色）');
  await reset(); await setup(); await mode('MainCamera', 'INDI');
  log = []; await conn('MainCamera', false); const c = uniq();
  log = []; await bind('MainCamera', c[0].split(':')[2]);
  check('绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; send('UnBindingDevice:MainCamera'); await wait(9000);
  check('解绑无Failed', realFailures().length === 0, JSON.stringify(realFailures().slice(0, 2)));
  check('解绑后相机回到候选列表', uniq().length >= 1, `解绑回包候选 ${uniq().length}`);
  log = []; await conn('MainCamera', false);
  check('解绑后重连候选==基线', uniq().length === BASE, `${uniq().length} vs ${BASE}`);
  const c2 = uniq();
  if (c2.length) {
    log = []; await bind('MainCamera', c2[0].split(':')[2]);
    check('解绑后可重新绑定', grab(/^ConnectSuccess:MainCamera/).length === 1);
  } else {
    check('解绑后可重新绑定', false, '无候选可绑');
  }
}

// 不变量：一台物理相机至多对应一个角色。
// 实现语义是【改绑】而非【拒绝】：把相机分配给新角色时，旧角色被清空
// （见 BindingDevice 的 "Reassign INDI camera to ..." 分支）。
// M3 放开了"同驱动必须同模式"的锁，其安全性正建立在这条不变量上——
// 同一台相机绝不会被两个角色同时持有，也就不会被双开。
async function E6_two_roles_same_camera() {
  console.log('\n【E6】两个角色抢同一台相机（应改绑：新角色接管，旧角色被清空）');
  await reset(); await setup(); await mode('MainCamera', 'INDI'); await mode('Guider', 'INDI');
  log = []; await conn('MainCamera', false); const c = uniq();
  if (!c.length) { check('E6 前置：有候选', false); return; }
  const idx = c[0].split(':')[2], name = c[0].split(':')[3];
  log = []; await bind('MainCamera', idx);
  check('Main绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; await conn('Guider', false); await wait(1000);
  log = []; await bind('Guider', idx);          // 同一台！
  const btl = grab(/^BindDeviceTypeList:/).slice(-1)[0] || '';
  const guiderOn = btl.includes(`Guider:${name}`);
  const mainStillOn = btl.includes(`MainCamera:${name}`);
  check('新角色接管该相机', guiderOn, btl.slice(0, 90));
  check('不变量：一台相机不会同时挂两个角色(旧角色被清空)', guiderOn && !mainStillOn,
        mainStillOn ? '⚠ Main 仍持有同一台 → 会被双开' : '旧角色已清空');
}

ws.on('open', async () => {
  console.log('connected', HOST);
  const t0 = Date.now();
  try {
    await baseline();
    await E1_mode_switch_while_connected();
    await E2_bind_then_switch_other_role();
    await E3_rapid_fire_connect();
    await E4_connectAll_then_single();
    await E5_bind_unbind_rebind();
    await E6_two_roles_same_camera();
  } catch (e) { console.log('EXC', e.message); }
  const pass = results.filter(r => r.ok).length, fail = results.filter(r => !r.ok).length;
  console.log(`\n${'='.repeat(62)}\n汇总(edge): ${pass} 通过 / ${fail} 失败  (${((Date.now() - t0) / 1000).toFixed(0)}s, 基线=${BASE})`);
  results.filter(r => !r.ok).forEach(r => console.log(`  ❌ ${r.n} — ${r.d}`));
  await reset();
  ws.close();
  process.exit(fail ? 1 : 0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
