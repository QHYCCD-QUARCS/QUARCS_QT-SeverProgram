# 树莓派端部署说明（手把手版）：AP/WAN 模式切换 + ZeroTier + 软件内配置上级 Wi‑Fi

这份文档的目标是：**你只要照着一步步执行命令，就能在树莓派上跑通**以下效果：

- **默认 AP 本地模式**：树莓派开热点，手机连热点访问网页
- **手动进入 WAN 远程模式**：在网页/软件里点按钮进入 WAN（热点会关闭，手机会掉线）
  - **如果插了网线**：自动走网线联网
  - **如果没网线**：自动尝试你保存的上级 Wi‑Fi（包括软件里保存的 `wan-uplink`）
  - **WAN 失败会超时自动回退 AP**，不失联
- **WAN 成功才启动 ZeroTier**；回到 AP 会停止 ZeroTier（避免乱路由）
- **在软件中扫描 Wi‑Fi → 选择 SSID → 输入密码 → 保存**（保存到固定 NM profile：`wan-uplink`）

---

## 0. 先看清楚：这些文件分别干什么

本目录下的文件（你会把它们安装到系统固定路径）：

- **`net-mode.sh`**：核心脚本。提供命令：
  - `net-mode.sh ap`：切回热点本地模式（AP up，WAN down，ZeroTier stop）
  - `net-mode.sh wan`：进入远程模式（AP down，优先网线，否则按 Wi‑Fi profiles 尝试；成功则 ZeroTier start；失败自动回 AP）
  - `net-mode.sh status`：输出一行 JSON（mode/ip/gw/zerotier）
- **`net-mode.conf`**：配置文件（会放到 `/etc/quarcs/net-mode.conf`）。告诉脚本：
  - 热点连接 profile 名是什么（`AP_CON`）
  - 网线连接 profile 名是什么（`WAN_ETH`）
  - 上级 Wi‑Fi profiles 有哪些（`WAN_WIFIS=(...)`，推荐包含 `wan-uplink`）
- **`sudoers.d/quarcs-net`**：sudoers 模板（会放到 `/etc/sudoers.d/quarcs-net`）。作用：
  - 让 systemd 运行 Qt 服务的用户可以**免密码 sudo**执行必要的 `nmcli` / `net-mode.sh` / `systemctl zerotier-one`
  - 这样 Qt 端就不需要保存/硬编码 sudo 密码（安全 + 稳定）
- **`systemd/quarcs-qt-server.service`**：systemd 服务模板。你需要改：
  - `<SERVICE_USER>`：服务用哪个 Linux 用户跑
  - `ExecStart` / `WorkingDirectory`：Qt 程序实际安装路径

---

## 1. 前置条件（必须确认）

在树莓派上执行以下命令确认环境（不改系统，只是查看）：  

```bash
# 1) 必须是 NetworkManager 在管网络
systemctl is-active NetworkManager || true

# 2) 列出连接 profile 名称（后面会用到）
nmcli -p con show

# 3) 查看接口名（一般 wlan0/eth0）
ip -br link

# 4) ZeroTier（可选但推荐）
systemctl status zerotier-one --no-pager || true
```

---

## 2. 决定 systemd 服务用户（你要先选这个）

你需要一个用户来跑 Qt 服务，比如 `quarcs` 或 `pi`。以下两种都可以：

- **方案 A（推荐）**：创建一个专用用户 `quarcs`（权限最小化）
- **方案 B**：直接用现有用户（比如 `pi`）

无论你选哪个，**这个用户名要同时写到**：

1) `/etc/systemd/system/quarcs-qt-server.service` 里的 `User=` / `Group=`  
2) `/etc/sudoers.d/quarcs-net` 里 `<SERVICE_USER>` 的替换结果  

后面文档里用 `SERVICE_USER=xxx` 表示。

---

## 3. 把文件安装到系统目录（逐条照做）

假设你当前就在本目录（`deploy/rpi/`）：

### 3.1 安装脚本（到 `/usr/local/sbin/`）

```bash
sudo install -m 0755 ./net-mode.sh /usr/local/sbin/net-mode.sh
```

验证脚本是否可执行：

```bash
ls -l /usr/local/sbin/net-mode.sh
```

### 3.2 安装配置（到 `/etc/quarcs/`）

```bash
sudo mkdir -p /etc/quarcs
sudo install -m 0644 ./net-mode.conf /etc/quarcs/net-mode.conf
```

### 3.3 配置 sudoers（到 `/etc/sudoers.d/`）

```bash
sudo install -m 0440 ./sudoers.d/quarcs-net /etc/sudoers.d/quarcs-net
```

然后编辑文件，把 `<SERVICE_USER>` 全部替换成你选的用户（例如 `quarcs` 或 `pi`）：

```bash
sudo nano /etc/sudoers.d/quarcs-net
```

最后必须校验：

```bash
sudo visudo -c
```

> 如果 `visudo -c` 报错：**立刻修复**，否则 sudo 可能无法使用。

---

## 4. 配置 `/etc/quarcs/net-mode.conf`（决定能不能切换成功）

打开并按真实连接名修改：

```bash
sudo nano /etc/quarcs/net-mode.conf
```

你需要填写/确认这些变量（从 `nmcli -p con show` 里抄名字，注意大小写/空格/中文都要一致）：

- **`AP_CON`**：热点 profile 名（你当前是 `RaspBerryPi-WiFi`）
- **`WAN_ETH`**：网线 profile 名（例如 `有线连接 1`；没有就留空字符串）
- **`WAN_WIFIS=(...)`**：上级 Wi‑Fi profiles 列表  
  - 必须包含 **`wan-uplink`**（因为软件里保存 Wi‑Fi 默认写入这个 profile）
  - 你也可以加多个：如 `wan-home`、`wan-phone`
- **`WLAN_IF/ETH_IF`**：接口名（一般 `wlan0/eth0`）

---

## 5. ZeroTier（一次性，远程控制要用）

加入网络只需做一次：

```bash
sudo systemctl enable zerotier-one
sudo zerotier-cli join <NETWORK_ID>
```

> 之后脚本会在 WAN 成功时自动 `systemctl start zerotier-one`，回 AP 自动 stop。

---

## 6. （推荐）用 systemd 跑 Qt 服务

### 6.1 安装 service 文件

```bash
sudo install -m 0644 ./systemd/quarcs-qt-server.service /etc/systemd/system/quarcs-qt-server.service
```

### 6.2 编辑 service：替换用户与可执行文件路径

```bash
sudo nano /etc/systemd/system/quarcs-qt-server.service
```

你必须改两类内容：

1) 把 `<SERVICE_USER>` 改成你选的用户  
2) 把 `ExecStart=` 改成 Qt 程序的真实路径；`WorkingDirectory=` 改成真实工作目录

### 6.3 启用并启动

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now quarcs-qt-server
sudo systemctl status quarcs-qt-server --no-pager
```

查看日志：

```bash
journalctl -u quarcs-qt-server -n 200 --no-pager
```

---

## 7. 手动验证（强烈建议先做，再上 UI）

### 7.1 先看状态（应输出一行 JSON）

```bash
sudo /usr/local/sbin/net-mode.sh status
```

### 7.2 强制回 AP（确保不失联）

```bash
sudo /usr/local/sbin/net-mode.sh ap
sudo /usr/local/sbin/net-mode.sh status
```

### 7.3 进入 WAN（失败会自动回退 AP）

```bash
sudo /usr/local/sbin/net-mode.sh wan || true
sudo /usr/local/sbin/net-mode.sh status
journalctl -t net-mode -n 200 --no-pager
```

---

## 8. 在软件/网页里怎么操作（你想要的效果）

打开热点设置弹窗（Wi‑Fi 图标）后，你会看到两块：

### 8.1 网络模式（AP/WAN）

- **Enter WAN**：手动进入远程模式（热点关闭，手机会掉线）
  - 脚本会 **优先尝试网线**（`WAN_ETH` 且 eth 链路 up）
  - 否则按 `WAN_WIFIS` 依次尝试 Wi‑Fi profiles（包含 `wan-uplink`）
  - WAN 失败会超时 **自动回退 AP**
- **Back AP**：回本地热点模式（并停止 ZeroTier）
- **Refresh**：刷新一次状态

### 8.2 上级 Wi‑Fi 配置（软件内完成）

- **Scan**：扫描附近 Wi‑Fi（Qt 调 `nmcli dev wifi list`）
- **Choose SSID**：选择 SSID
- **Password**：输入密码
- **Save**：保存到固定 profile：**`wan-uplink`**

> 关键点：保存成功后，进入 WAN 时才能用到它，前提是 `/etc/quarcs/net-mode.conf` 里 `WAN_WIFIS` 包含 `wan-uplink`（默认模板已包含）。

---

## 9. 常见问题与排错（按症状查）

### 9.1 点 Enter WAN 后网页断开了，正常吗？

正常。因为热点关闭，手机与树莓派的连接会断。  
如果 WAN 失败，脚本会回退 AP，你手机重新连热点就恢复了。

### 9.2 `net-mode.sh status` 报错 / `netStatus` 不工作

按顺序检查：

```bash
ls -l /usr/local/sbin/net-mode.sh
sudo cat /etc/quarcs/net-mode.conf
nmcli -p con show
sudo visudo -c
```

再看脚本日志：

```bash
journalctl -t net-mode -n 200 --no-pager
```

### 9.3 Wi‑Fi 扫描/保存失败（WiFiSaveResult fail）

常见原因：

- sudoers 没放行（或 `<SERVICE_USER>` 没替换正确）
- `wlan0` 接口名不同（不是 `wlan0`）

你可以在系统上直接跑一遍（看错误输出）：

```bash
sudo nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list ifname wlan0 --rescan yes
sudo nmcli -t -f NAME con show
```

---

## 10. 开发机交叉编译后同步到业务机

若你在开发机上完成了树莓派交叉编译，可直接使用仓库内脚本把产物同步到业务机：

```bash
cd /home/q/workspace_origin/QUARCS_QT-SeverProgram
chmod +x deploy/rpi/deploy_build_to_pi.sh
deploy/rpi/deploy_build_to_pi.sh
```

默认会把以下文件同步到业务机：

- `build-rpi/client`
- `build-rpi/guiding_offline_test`
- `build-rpi/qhyccd.ini`

默认目标目录：

```bash
/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD
```

如需覆盖默认值，可在执行时传环境变量：

```bash
PI_HOST=192.168.31.42 \
LOCAL_BUILD_DIR=/home/q/workspace_origin/QUARCS_QT-SeverProgram/build-rpi-verify \
deploy/rpi/deploy_build_to_pi.sh
```

---

## 11. 回退/卸载（需要恢复到原始状态时）

```bash
# 停止并禁用服务
sudo systemctl disable --now quarcs-qt-server || true

# 移除脚本/配置/sudoers（按需）
sudo rm -f /usr/local/sbin/net-mode.sh
sudo rm -f /etc/quarcs/net-mode.conf
sudo rm -f /etc/sudoers.d/quarcs-net
sudo rm -f /etc/systemd/system/quarcs-qt-server.service
sudo systemctl daemon-reload
```
