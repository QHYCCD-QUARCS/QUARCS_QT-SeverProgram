// 顺序覆盖测试 v2：断言改为【配置无关】——不假设相机台数，只断言一致性/不叠加/可复原。
// 针对用户报的"按键顺序会导致列表紊乱/状态紊乱"。
const WebSocket = require('/home/q/workspace_origin/QUARCS_stellarium-web-engine/apps/web-frontend/node_modules/ws');
const HOST = '172.24.217.51';
const ws = new WebSocket(`ws://${HOST}:8600`);

let log = [];
const send = (c) => ws.send(JSON.stringify({ type: 'Vue_Command', message: c, msgid: 'oc2-' + Date.now() + '-' + Math.random() }));
const wait = (ms) => new Promise(r => setTimeout(r, ms));
ws.on('message', (d) => {
  let s = d.toString();
  try { const o = JSON.parse(s); if (o && o.message != null) s = String(o.message); } catch (e) {}
  log.push(s);
});
const grab = (re) => log.filter(x => re.test(x));
// 候选去重（同一次连接里后端可能重复发同一条）
const uniq = () => [...new Set(grab(/^DeviceToBeAllocated:/).map(x => x.split(':').slice(0,4).join(':')))];

const results = [];
const check = (name, cond, detail='') => {
  results.push({ name, ok: !!cond, detail });
  console.log(`   ${cond ? '✅' : '❌'} ${name}${detail ? '  — ' + detail : ''}`);
};

const reset  = async () => { log = []; send('disconnectAllDevice'); await wait(15000); };
const setup  = async () => { send('ConfirmIndiDriver:indi_qhy_ccd:9600:20'); await wait(1200);
                             send('ConfirmIndiDriver:indi_qhy_ccd:9600:1');  await wait(1200); };
const mode   = async (r,m) => { send(`SetConnectionMode:${r}:${m}`); await wait(2000); };
const conn   = async (r,sdk) => { send(`ConnectDriver:indi_qhy_ccd:${r}${sdk?':SDK':''}`); await wait(16000); };
const bind   = async (r,i) => { send(`BindingDevice:${r}:${i}`); await wait(9000); };

let BASE = 0;   // INDI 基线台数（由第一次连接测得，不硬编码）

async function S0_baseline() {
  console.log('\n【S0】测基线：INDI 一次连接看到几台（不硬编码）');
  await reset(); await setup(); await mode('MainCamera','INDI');
  log = []; await conn('MainCamera', false);
  BASE = uniq().length;
  check('基线可测得(>0)', BASE > 0, `INDI 候选基线 = ${BASE}`);
  check('无 Failed', grab(/^ConnectDriverFailed/).length === 0, JSON.stringify(grab(/^ConnectDriverFailed/)));
}

async function S1() {
  console.log('\n【S1】连Main→绑→连Guider→绑（正常顺序）');
  await reset(); await setup(); await mode('MainCamera','INDI'); await mode('Guider','INDI');
  log = []; await conn('MainCamera', false);
  const c1 = uniq();
  check('连Main候选==基线', c1.length === BASE, `${c1.length} vs ${BASE}`);
  log = []; await bind('MainCamera', c1[0].split(':')[2]);
  check('Main绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
  log = []; await conn('Guider', false);
  const c2 = uniq();
  check('连Guider候选==基线-1(已绑的不再是候选)', c2.length === BASE - 1, `${c2.length} vs ${BASE-1}`);
  check('连Guider无Failed', grab(/^ConnectDriverFailed/).length === 0, JSON.stringify(grab(/^ConnectDriverFailed/)));
  if (c2.length) { log = []; await bind('Guider', c2[c2.length-1].split(':')[2]);
    check('Guider绑定成功', grab(/^ConnectSuccess:Guider/).length === 1); }
}

async function S2() {
  console.log('\n【S2】连Main→连Guider→再绑两次（先连后绑）');
  await reset(); await setup(); await mode('MainCamera','INDI'); await mode('Guider','INDI');
  await conn('MainCamera', false);
  log = []; await conn('Guider', false);
  const c = uniq();
  check('两次连接候选==基线(不叠加)', c.length === BASE, `${c.length} vs ${BASE}`);
  check('无Failed', grab(/^ConnectDriverFailed/).length === 0);
  if (c.length >= 2) {
    log = []; await bind('MainCamera', c[0].split(':')[2]);
    check('Main绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
    log = []; await bind('Guider', c[1].split(':')[2]);
    check('Guider绑定成功', grab(/^ConnectSuccess:Guider/).length === 1);
  }
}

async function S3() {
  console.log('\n【S3】反复切模式 SDK→INDI→SDK→INDI 后再连');
  await reset(); await setup();
  log = [];
  await mode('MainCamera','SDK'); await mode('MainCamera','INDI');
  await mode('MainCamera','SDK'); await mode('MainCamera','INDI');
  check('切模式无Failed', grab(/^SetConnectionModeFailed/).length === 0, JSON.stringify(grab(/^SetConnectionModeFailed/)));
  check('选模式不触发连接(候选0)', grab(/^DeviceToBeAllocated:/).length === 0, `候选 ${grab(/^DeviceToBeAllocated:/).length}`);
  check('每次切模式只回1条Success', grab(/^SetConnectionModeSuccess:/).length === 4, `${grab(/^SetConnectionModeSuccess:/).length} 条(4次操作)`);
  log = []; await conn('MainCamera', false);
  check('最终按INDI连接候选==基线', uniq().length === BASE, `${uniq().length} vs ${BASE}`);
}

async function S4() {
  console.log('\n【S4】连→绑→断开→再连（断开残留）');
  await reset(); await setup(); await mode('MainCamera','INDI');
  await conn('MainCamera', false);
  let c = uniq(); if (c.length) await bind('MainCamera', c[0].split(':')[2]);
  await reset();
  log = []; await conn('MainCamera', false);
  check('断开后重连候选==基线(无残留)', uniq().length === BASE, `${uniq().length} vs ${BASE}`);
  check('重连无Failed', grab(/^ConnectDriverFailed/).length === 0);
}

async function S5() {
  console.log('\n【S5】混用: Main=SDK先连绑 → Guider=INDI后连绑');
  await reset(); await setup(); await mode('MainCamera','SDK'); await mode('Guider','INDI');
  log = []; await conn('MainCamera', true);
  const p = grab(/^DeviceToBeAllocated:/).find(x => /QHY247C/.test(x));
  check('SDK候选含QHY247C', !!p);
  if (p) { log = []; await bind('MainCamera', p.split(':')[2]);
    check('SDK Main绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1); }
  log = []; await conn('Guider', false);
  check('混用: INDI Guider未被锁', grab(/ConnectedInOtherMode/).length === 0, JSON.stringify(grab(/^ConnectDriverFailed/)));
  const g = grab(/^DeviceToBeAllocated:/).find(x => /5III/.test(x));
  if (g) { log = []; await bind('Guider', g.split(':')[2]);
    check('INDI Guider绑定成功', grab(/^ConnectSuccess:Guider/).length === 1); }
}

async function S6() {
  console.log('\n【S6】混用反序: Guider=INDI先 → Main=SDK后');
  await reset(); await setup(); await mode('Guider','INDI'); await mode('MainCamera','SDK');
  log = []; await conn('Guider', false);
  const g = grab(/^DeviceToBeAllocated:/).find(x => /5III/.test(x));
  if (g) { log = []; await bind('Guider', g.split(':')[2]);
    check('INDI Guider绑定成功(先)', grab(/^ConnectSuccess:Guider/).length === 1); }
  log = []; await conn('MainCamera', true);
  check('混用反序: SDK Main未被锁', grab(/ConnectedInOtherMode/).length === 0, JSON.stringify(grab(/^ConnectDriverFailed/)));
  const p = grab(/^DeviceToBeAllocated:/).find(x => /QHY247C/.test(x));
  if (p) { log = []; await bind('MainCamera', p.split(':')[2]);
    check('SDK Main绑定成功(后)', grab(/^ConnectSuccess:MainCamera/).length === 1); }
}

async function S7() {
  console.log('\n【S7】重复点连接3次（幂等/不叠加）');
  await reset(); await setup(); await mode('MainCamera','INDI');
  await conn('MainCamera', false); await conn('MainCamera', false);
  log = []; await conn('MainCamera', false);
  check('第3次连接候选==基线(幂等)', uniq().length === BASE, `${uniq().length} vs ${BASE}`);
  check('重复连接无Failed', grab(/^ConnectDriverFailed/).length === 0);
}

async function S8() {
  console.log('\n【S8】绑定后再重复绑同一角色到另一台（改绑）');
  await reset(); await setup(); await mode('MainCamera','INDI');
  log = []; await conn('MainCamera', false);
  const c = uniq();
  if (c.length >= 2) {
    log = []; await bind('MainCamera', c[0].split(':')[2]);
    check('首次绑定成功', grab(/^ConnectSuccess:MainCamera/).length === 1);
    log = []; await bind('MainCamera', c[1].split(':')[2]);
    check('改绑到另一台不报错', grab(/^ConnectDriverFailed|^BindingDeviceFailed/).length === 0,
          JSON.stringify(grab(/Failed/).slice(0,2)));
  }
}

ws.on('open', async () => {
  console.log('connected', HOST);
  const t0 = Date.now();
  try {
    await S0_baseline();
    await S1(); await S2(); await S3(); await S4(); await S5(); await S6(); await S7(); await S8();
  } catch (e) { console.log('EXC', e.message); }
  const pass = results.filter(r => r.ok).length, fail = results.filter(r => !r.ok).length;
  console.log(`\n${'='.repeat(62)}\n汇总: ${pass} 通过 / ${fail} 失败   (${((Date.now()-t0)/1000).toFixed(0)}s, 基线=${BASE})`);
  results.filter(r => !r.ok).forEach(r => console.log(`  ❌ ${r.name} — ${r.detail}`));
  await reset();
  ws.close(); process.exit(fail ? 1 : 0);
});
ws.on('error', e => { console.log('ERR', e.message); process.exit(1); });
