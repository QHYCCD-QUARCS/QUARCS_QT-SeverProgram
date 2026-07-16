# WS 层集成测试（相机连接/分配）

对着**真实业务机**跑的端到端测试，不需要浏览器、不需要 GUI。用于回归"连接/绑定/断开/切模式"
各种顺序下的一致性。

## 前提

- 业务机已部署最新后端（`./deploy_all_to_pi.sh --backend-only`）。
- **测试期间不要开前端页面**：前端也是 WS 客户端，会响应广播消息并发命令，
  污染测试数据（实测曾把候选数从 4 放大到 48）。开着的浏览器请整页关闭
  （只刷新无效——前端会自动重连）。
- 在开发机上跑（node 18 + 仓库 node_modules 里的 ws）。

## 协议

QT client 期望 JSON，**裸文本发不通**：

```json
{"type":"Vue_Command","message":"ConnectDriver:indi_qhy_ccd:MainCamera","msgid":"任意"}
```

转发器（NodeJs-Transponder，:8600）把消息广播给所有客户端；`msgid` 的格式也是天然的
客户端指纹（前端用 `<时间戳>-<序号>`）。

## 脚本

| 脚本 | 用途 |
|---|---|
| `ws_cmd.js <host> '<命令>' [秒]` | 发任意一条命令并打印关键回包。最常用的排查工具。 |
| `order_coverage.js` | **顺序覆盖测试**（9 场景 / 28 断言，约 9 分钟）。断言配置无关：先测基线台数，再相对基线断言。 |
| `verify_m2_lazy_open.js` | 验证 M2：SDK 连接后 0 台被 open；绑定后只 open 选中那台。 |
| `verify_mixed.js` | 验证 M3：Main=SDK + Guider=INDI 混用同时成立。 |

```bash
node src/tests/ws/order_coverage.js
node src/tests/ws/ws_cmd.js 172.24.217.51 'ConnectDriver:indi_qhy_ccd:MainCamera' 20
```

## order_coverage 覆盖的顺序

| 场景 | 顺序 | 关键断言 |
|---|---|---|
| S0 | 一次 INDI 连接 | 测出候选基线（不硬编码台数） |
| S1 | 连Main→绑→连Guider→绑 | 绑定后候选 == 基线-1（已绑的移出候选） |
| S2 | 连Main→连Guider→再绑两次 | 两次连接候选 == 基线（**不叠加**） |
| S3 | 反复切模式 ×4 → 再连 | **选模式不触发连接**；每次只回 1 条 Success |
| S4 | 连→绑→断开→再连 | 候选回到基线（**无残留**） |
| S5 | 混用：SDK 先 → INDI 后 | 两角色都绑上，无 ConnectedInOtherMode |
| S6 | 混用：INDI 先 → SDK 后 | 同上（反序） |
| S7 | 重复点连接 ×3 | **幂等**，候选 == 基线 |
| S8 | 改绑同角色到另一台 | 不报错 |

## 断言为什么要"配置无关"

`qhyccd.ini` 的 `[demo] enabled` 决定 SDK 是否合成 DEMO 相机，台数会变；而且
**QHY SDK 在 InitQHYCCDResource（驱动启动）时会把该 ini 写回**，手工改的值会被覆盖。
因此不要硬编码"应该有几台"，只断言相对基线的一致性。

## 已知环境坑

- `BUILD/qhyccd.ini` 每次后端部署都会被 rsync 覆盖。
- INDI 侧的 ini 是 `~/qhyccd.ini`（indiserver 的 cwd = `~`，见 dab9f19）；改它要重启
  INDI 驱动才生效，而驱动启动时 SDK 又可能回写它。
- `pkill indiserver` 会破坏 `/tmp/myFIFO` 控制通道；恢复办法是重启 client
  （`initINDIServer()` 会自己重建 FIFO 并拉起 indiserver）。
