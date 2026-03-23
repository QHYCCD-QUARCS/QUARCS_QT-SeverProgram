# QUARCS 树莓派：AP / WAN / ZeroTier 方案实现笔记（含手把手步骤）

> 适用对象：树莓派 4（单 Wi‑Fi）、NetworkManager 管热点（shared），QUARCS 现有架构为 **Vue(Web) → WebSocket(Hub) → Qt → 系统命令**。  
> 目标：不改硬件，兼容三种联网方式，并在需要远程控制时接入外网（ZeroTier），同时保证“不失联”。  

---

## 1. 问题是什么（Why）

你需要同时覆盖三种使用场景：

1) **同一局域网**：盒子和手机连同一路由器  
2) **盒子开热点（默认）**：手机连盒子热点访问本地网页（手机可能不上网）  
3) **手机开热点**：盒子连手机热点（双方可上网，适合外出）

关键硬约束：

- 树莓派 4 **只有一块 Wi‑Fi** 时，长期稳定的 **AP+STA 并发**（既开热点又连上级 Wi‑Fi）通常不可控/不稳定。

因此要满足“不失联”，必须保证：

- **默认 AP 本地模式**（手机随时能连回盒子热点）
- **远程 WAN 模式必须手动进入**（避免自动切换导致用户突然连不上）
- **WAN 失败自动回退 AP**（防止用户以为死机/变砖）
- **ZeroTier 只在 WAN 成功时启用**（避免 AP 模式乱路由、无意义重连）

---

## 2. 解决思路（What）

### 2.1 方案选择

我们最终采用“双模式切换”：

- **AP 模式（默认）**：热点开启，手机连热点访问 `10.42.0.1`；ZeroTier 停止  
- **WAN 模式（手动）**：热点关闭；优先走网线，否则走已保存的上级 Wi‑Fi；WAN 成功后启动 ZeroTier；失败超时回退 AP

> 实际落地时，为了避免远程会话（VNC/SSH）被“切回 AP”踢下线，我们做了一个工程化优化：  
> **AP 模式不再 down 掉网线连接（wan-eth）**，只停止 ZeroTier、确保热点 up。  

### 2.2 为什么接入 Qt/WebSocket 而不是 Apache CGI

你的现有产品链路已经完整：

- Vue 前端通过 WebSocket 发 `Vue_Command`
- Qt 收到后执行系统命令，再把结果通过 `QT_Return` 回传给前端

因此把网络模式切换接到这条链路里：

- 能复用现有通信、权限、日志体系
- 不用再引入 Apache CGI/额外端口/额外权限面

---

## 3. 架构与数据流（How）

### 3.1 架构图

```mermaid
flowchart TD
  WebUI[Vue WebUI] -->|WebSocket JSON(type=Vue_Command)| Hub[NodeJs-Transponder]
  Hub -->|转发 message| Qt[Qt Server Program]
  Qt -->|sudo -n 执行| NM[NetworkManager/nmcli]
  Qt -->|sudo -n 执行| Script[/usr/local/sbin/net-mode.sh]
  Script -->|systemctl start/stop| ZT[zerotier-one]
  Qt -->|QT_Return: NetStatus| Hub --> WebUI
```

### 3.2 通信协议要点（避免“冒号分割”坑）

前端对 `QT_Return` 默认用 `:` 分割消息；而 JSON 中包含大量 `:`，会被错误拆分。

解决方式：新增 **管道分隔协议**：

- `NetStatus|{json}`
- `WiFiScan|[jsonArray]`
- `NetModeResult|<ap|wan>|<ok|fail>|<detail?>`
- `WiFiSaveResult|<save|scan>|<ok|fail>|<detail?>`

这样前端可以在进入 `switch(messageType)` 之前先用 `startsWith('NetStatus|')` 直接解析 JSON。

---

## 4. 使用的技术与技术栈

### 4.1 技术栈

- **系统网络**：Raspberry Pi OS + **NetworkManager**（热点 shared）
- **网络控制**：`nmcli` + `ip` + `ping` + `systemctl`
- **远程通道**：**ZeroTier**（`zerotier-one` 服务）
- **前端**：Vue2 + Vuetify（QUARCS Web Frontend）
- **通信**：WebSocket（8600/8601），NodeJs-Transponder 作为 Hub
- **后端执行**：Qt（QProcess）执行受控 sudo 命令
- **权限控制**：`/etc/sudoers.d/*`（NOPASSWD 最小放行）

### 4.2 关键安全点

历史实现里存在硬编码 sudo 密码（`echo 'xxx' | sudo -S ...`），我们已迁移为：

- sudoers 放行指定命令
- Qt 执行 `sudo -n ...`（非交互，权限不足就快速失败）
- Qt 调用增加 **超时 kill**（避免卡死无返回）

---

## 5. 核心实现点（代码/脚本层面）

### 5.1 `net-mode.sh`（模式切换脚本）

路径（安装后）：`/usr/local/sbin/net-mode.sh`  
配置（安装后）：`/etc/quarcs/net-mode.conf`

- `ap`：确保热点 up，关掉上级 Wi‑Fi profiles，**停止 ZeroTier**  
  - 工程化优化：**不 down 网线（wan-eth）**，避免远程会话断开  
- `wan`：先 down 热点，再优先尝试网线（WAN_ETH + carrier=1），否则按 `WAN_WIFIS` 依次尝试；成功 start ZeroTier；失败回退 AP  
- `status`：输出 JSON（mode/wlan_ip/eth_ip/gateway/zerotier）

> 你当前实际 profile 名：`ap-hotspot`（wlan0）、`wan-eth`（eth0）。

### 5.2 Qt：新增网络命令 + Wi‑Fi 扫描/保存

前端发：

- `netStatus`
- `netMode:ap` / `netMode:wan`
- `wifiScan`
- `wifiSaveB64|<base64(JSON)>`，JSON 示例：`{"name":"wan-uplink","ssid":"xxx","psk":"yyy"}`

Qt 回：

- `NetStatus|{...}`
- `NetModeResult|...`
- `WiFiScan|[...]`
- `WiFiSaveResult|...`

并且 Qt 执行采用：

- `sudo -n`（不交互）
- 15s 超时（避免 hanging）

### 5.3 前端：热点弹窗扩展

在热点弹窗中增加两块：

1) **网络模式**：状态显示 + `Enter WAN / Back AP / Refresh`  
2) **上级 Wi‑Fi 配置**：`Scan`→选择 SSID→输入密码→`Save`（保存到 `wan-uplink`）

并修复 UI 裁切：

- 弹窗允许纵向滚动（避免小屏裁掉 SSID 列表）
- 调整布局位置与高度

---

## 6. 手把手操作步骤（照顺序做即可复现当前结果）

> 说明：你当前树莓派用户名是 `quarcs`，并且 profile 名为 `ap-hotspot / wan-eth`。

### 6.1 第 0 步：确认环境（只读）

```bash
systemctl is-active NetworkManager || true
nmcli -p con show
ip -br link
systemctl status zerotier-one --no-pager || true
```

期望看到：

- NetworkManager: `active`
- 连接列表里包含 `ap-hotspot`(wifi)、`wan-eth`(ethernet)

### 6.2 第 1 步：把部署文件放到树莓派任意目录（例如 U 盘拷到 `/home/quarcs/rpi/`）

该目录里应至少有：

- `net-mode.sh`
- `net-mode.conf`
- `sudoers.d/quarcs-net`
- `systemd/quarcs-qt-server.service`（可选：如果你要 systemd 托管 Qt）

### 6.3 第 2 步：安装脚本与配置到系统路径

```bash
cd /home/quarcs/rpi
sudo install -m 0755 ./net-mode.sh /usr/local/sbin/net-mode.sh
sudo mkdir -p /etc/quarcs
sudo install -m 0644 ./net-mode.conf /etc/quarcs/net-mode.conf
```

### 6.4 第 3 步：修改 `/etc/quarcs/net-mode.conf`（必须）

```bash
sudo nano /etc/quarcs/net-mode.conf
```

至少设置为：

- `AP_CON="ap-hotspot"`
- `WAN_ETH="wan-eth"`
- `WAN_WIFIS=("wan-uplink")`

> 后续你也可以扩展：`WAN_WIFIS=("wan-uplink" "wan-home" "wan-phone")`

### 6.5 第 4 步：安装 sudoers（让 Qt 免密执行 nmcli/脚本）

```bash
sudo install -m 0440 /home/quarcs/rpi/sudoers.d/quarcs-net /etc/sudoers.d/quarcs-net
sudo nano /etc/sudoers.d/quarcs-net
```

把里面所有 `<SERVICE_USER>` 替换成 `quarcs`。

**注意**：sudoers 里逗号要转义，扫描行应是：

```text
quarcs ALL=(root) NOPASSWD: /usr/bin/nmcli -t -f SSID\,SIGNAL\,SECURITY dev wifi list ifname wlan0 --rescan yes
```

校验：

```bash
sudo visudo -c
```

### 6.6 第 5 步：验证脚本（先别碰 UI）

```bash
sudo /usr/local/sbin/net-mode.sh status
sudo /usr/local/sbin/net-mode.sh ap
nmcli -t -f NAME,DEVICE con show --active
sudo /usr/local/sbin/net-mode.sh status
```

期望：

- AP 模式下 active 同时包含 `ap-hotspot:wlan0` **和** `wan-eth:eth0`（不会再踢掉 VNC）
- `status` 显示 `mode=AP` 且 `zerotier=stopped`

再测试 WAN：

```bash
sudo /usr/local/sbin/net-mode.sh wan || echo "WAN_EXIT=$?"
nmcli -t -f NAME,DEVICE con show --active
sudo /usr/local/sbin/net-mode.sh status
```

期望：

- active 里只有 `wan-eth:eth0`（热点关闭）
- `status` 显示 `mode=WAN` 且 `zerotier=running`

### 6.7 第 6 步：在软件里保存手机热点（上级 Wi‑Fi）

1) 保持 AP 模式（手机连 `ap-hotspot`）打开网页  
2) 在弹窗 **Uplink Wi‑Fi** 区域：`Scan` → 选择 SSID（手机热点）→ 输入密码 → `Save`  
3) 树莓派上验证 `wan-uplink` 已保存：

```bash
nmcli -p con show "wan-uplink" || true
```

### 6.8 第 7 步：用手机热点上网（无网线场景）

1) 拔掉网线（或确保 `wan-eth` 不可用）  
2) 在网页点 `Enter WAN`（热点会关闭，手机会掉线）  
3) 等待脚本连接手机热点成功后，通过 ZeroTier 远程访问

---

## 7. 我们踩过的坑 & 修复记录（非常重要）

1) **AP 模式 down 网线会踢掉 VNC/SSH**  
   - 修复：AP 模式不 down `wan-eth`，只停 ZeroTier；`status` 优先显示 AP
2) **sudoers 里的逗号导致语法错误**  
   - 修复：`SSID\,SIGNAL\,SECURITY` 必须写成转义逗号
3) **Qt 执行 sudo 可能卡死不返回**  
   - 修复：统一改为 `sudo -n` + 15s 超时 kill，保证前端能得到失败提示
4) **前端弹窗裁切导致 Scan 看不到列表**  
   - 修复：弹窗纵向滚动 + 调整 Wi‑Fi 区域位置/高度

---

## 8. 当前实现结果（你已跑通的状态）

- `ap-hotspot` 本地模式可用，且 **不再踢掉网线远程会话**
- `Enter WAN`/`Back AP` 切换可用
- WAN 成功时 ZeroTier 启动、回 AP 停止
- sudoers 免密 nmcli 扫描已验证可用（`sudo -n nmcli ...` 可正常输出）

---

## 9. 后续可选增强（按需）

- Wi‑Fi 扫描解析增强：处理 SSID 含 `:` 的极端情况（目前按 `:` split）
- 将 `wlan0`/`eth0` 也改为从配置读取（避免未来换接口名）
- 增加“保存后自动把 `wan-uplink` 加入 `WAN_WIFIS`”的提示/引导（现在通过文档约束）

