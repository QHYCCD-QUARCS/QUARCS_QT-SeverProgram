# 树莓派 AP+STA + 有线/无线分流配置说明

本文档说明在 **Debian/Raspberry Pi OS** 上，使用 **hostapd + wpa_supplicant + dhclient + dnsmasq + iptables** 实现「一边连上级 WiFi、一边开热点」的手动配置；同时补充 **eth0 有线地址** 与 **wlan0 无线地址** 同时长期使用时的源地址策略路由，避免「访问有线 IP，回包却从无线出去」导致的慢速或不稳定。

> **说明**：下列路径与网段（如 `10.42.0.1/24`、SSID `LQ`、上级 `QHYCCD503`）与当前已部署环境一致；若你的 SSID/密码/网段不同，请在对应文件中替换。

---

## 一、概念与接口分工

| 角色 | 接口 | 典型用途 |
|------|------|----------|
| 有线 LAN | `eth0` | 接入局域网，作为稳定高速入口；地址由现场 DHCP/静态配置决定 |
| STA（客户端） | `wlan0` | 连接上级路由器 WiFi（本例：`QHYCCD503`） |
| AP（热点） | `uap0` | 由 `wlan0` 派生的虚拟接口，供手机/电脑连接（本例：SSID `LQ`，开放） |

热点网关 IP、DHCP 由本机提供；连热点的设备通过 **NAT** 从 `wlan0` 访问上级网络。

若 `eth0` 与 `wlan0` 同时接入同一局域网，例如：

```text
eth0  = <有线当前IP>/<前缀长度>
wlan0 = <无线当前IP>/<前缀长度>
```

则不能只依赖默认路由 metric。推荐使用**源地址策略路由**。下面地址只是现场示例，脚本不能写死该网段；实际部署时应从每个接口当前的 IPv4 地址、直连路由和默认网关自动推导。

```text
from <有线当前IP> -> eth0
from <无线当前IP> -> wlan0
```

这样连接有线地址的会话从有线回包，连接无线地址的会话从无线回包，两者可长期共存，并可适配 `192.168.x.x`、`10.x.x.x`、`172.16.x.x` 等任意现场网段。

---

## 二、WiFi 扫描与连接相关命令

以下命令在树莓派上执行；若登录用户非 root，多数需加 `sudo`。`iw` 一般在 `/sbin/iw`。

### 2.1 查看无线网卡与 PHY

```bash
/sbin/iw dev
```

**作用**：列出无线接口（如 `wlan0`、`uap0`）、类型（managed/AP）、当前 SSID、信道等。

```bash
/sbin/iw phy
```

**作用**：查看物理层能力、支持频段等。

---

### 2.2 扫描附近 WiFi（STA 侧）

```bash
sudo /sbin/ip link set wlan0 up
sudo /sbin/iw dev wlan0 scan | less
```

**作用**：  
- 第一条：把 `wlan0` 置为 UP，否则扫描可能无结果。  
- 第二条：在 `wlan0` 上扫描 BSS；输出含 SSID、BSSID、频率、信号强度等。

**精简查看 SSID（便于快速浏览）**：

```bash
sudo /sbin/iw dev wlan0 scan | grep -E '^BSS|SSID:'
```

**作用**：从扫描结果里只筛出 BSS 与 SSID 行，便于阅读。

---

### 2.3 查看当前 STA 是否已连接、连的是谁

```bash
/sbin/iw dev wlan0 link
```

**作用**：显示当前关联的 AP、SSID、信道、信号、速率等；未连接时无 `SSID` 行。

---

### 2.4 使用 wpa_cli（在已运行 wpa_supplicant 时）

当系统使用 **`wpa_supplicant@wlan0`** 且配置在 `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf` 时：

```bash
sudo wpa_cli -i wlan0 status
```

**作用**：查看 wpa 状态（`wpa_state`、`ssid`、`ip_address` 等）。

```bash
sudo wpa_cli -i wlan0 scan
sudo sleep 3
sudo wpa_cli -i wlan0 scan_results
```

**作用**：触发扫描并查看结果（表格形式）。

```bash
sudo wpa_cli -i wlan0 list_networks
```

**作用**：列出已配置的网络编号与 SSID。

> **注意**：是否允许用 `wpa_cli add_network` 等改配置，取决于 `wpa_supplicant` 是否开启 `update_config=1` 及权限；生产环境更推荐**改配置文件后重启服务**（见下文）。

---

### 2.5 使用 NetworkManager（若仍管理其它网卡）

仅当未将 `wlan0` 设为 unmanaged、且由 NM 管理时适用：

```bash
nmcli device wifi list
```

**作用**：列出可见 WiFi（NM 管理模式下）。

```bash
nmcli device status
```

**作用**：查看各设备与连接状态。

本部署中 **`wlan0`/`uap0` 已设为 NM unmanaged**，日常以 **`iw` / `wpa_cli`** 为准。

---

## 三、修改热点名称（SSID）

热点由 **`hostapd`** 在 **`uap0`** 上提供，SSID 在配置文件中：

**文件**：`/etc/hostapd/uap0.conf`  

**修改项**：

```ini
ssid=你的新热点名
```

**示例**：将 `ssid=LQ` 改为 `ssid=MyHotspot`。

**使配置生效**（重启 hostapd 实例）：

```bash
sudo systemctl restart hostapd@uap0
```

**作用**：重新加载 `uap0` 的 hostapd 配置，手机/电脑会搜到新 SSID（旧 SSID 消失）。

**可选验证**：

```bash
/sbin/iw dev | grep -A2 'Interface uap0'
```

**作用**：确认 `uap0` 当前广播的 `ssid` 与预期一致。

---

## 四、全程配置：文件与 systemd 单元一览

以下为当前方案涉及的主要路径（便于对照与备份）。

| 类型 | 路径 |
|------|------|
| NM 不管理无线 | `/etc/NetworkManager/conf.d/99-unmanage-wifi.conf` |
| 创建 uap0 + 网关 IP | `/usr/local/sbin/uap0-create.sh` |
| uap0 创建服务 | `/etc/systemd/system/uap0-create.service` |
| AP 配置（SSID、开放等） | `/etc/hostapd/uap0.conf` |
| STA 配置 | `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf` |
| wlan0 DHCP 客户端 | `/etc/systemd/system/wlan0-dhcp.service`（`dhclient`） |
| dnsmasq 主配置 | `/etc/dnsmasq.conf`（`conf-dir=/etc/dnsmasq.d`） |
| 热点 DHCP/DNS | `/etc/dnsmasq.d/ap-uap0.conf` |
| IPv4 转发 | `/etc/sysctl.d/99-ap-sta-forward.conf` |
| NAT 脚本与服务 | `/usr/local/sbin/ap-sta-nat.sh`、`/etc/systemd/system/ap-sta-nat.service` |
| 有线/无线源地址策略路由 | `/usr/local/sbin/quarcs-dual-lan-policy.sh`、`/etc/systemd/system/quarcs-dual-lan-policy.service` |
| 策略路由定时刷新 | `/etc/systemd/system/quarcs-dual-lan-policy.timer` |
| 双 LAN ARP/rp_filter 参数 | `/etc/sysctl.d/99-quarcs-dual-lan.conf` |
| wpa 覆盖（RuntimeDirectory 等） | `/etc/systemd/system/wpa_supplicant@wlan0.service.d/override.conf` |
| dnsmasq 依赖与清理 | `/etc/systemd/system/dnsmasq.service.d/after-uap0.conf`、`nm-cleanup.conf` |

**若曾与 ifupdown 冲突**：原 `/etc/network/interfaces.d/wlan0-uap0` 可能已改名为备份（如 `wlan0-uap0.backup-by-systemd-apsta`），且 **`networking.service` 已 disable**，避免与 systemd 栈抢 `wlan0`。

---

## 五、从零到可运行：配置命令逐步说明

下列命令按**逻辑顺序**排列；在已装 `hostapd`、`wpasupplicant`、`dnsmasq`、`isc-dhcp-client`、`iw` 的前提下执行。若包未安装，需先安装（不同镜像源下命令可能略有差异）。

### 5.1 安装依赖包

```bash
sudo apt-get update
sudo apt-get install -y hostapd wpasupplicant dnsmasq isc-dhcp-client iw
```

| 命令 | 作用 |
|------|------|
| `apt-get update` | 更新软件源索引。 |
| `apt-get install -y ...` | 安装 AP、STA、DHCP 服务端/客户端及 `iw` 等工具；`-y` 自动确认。 |

（可选）若计划用 **dhcpcd** 代替 **dhclient** 管 `wlan0`，可安装 `dhcpcd`；本环境曾因网络问题未装上，故用 **`wlan0-dhcp.service` + `dhclient`**。

---

### 5.2 让 NetworkManager 不管 wlan0 / uap0

```bash
sudo tee /etc/NetworkManager/conf.d/99-unmanage-wifi.conf << 'EOF'
[keyfile]
unmanaged-devices=interface-name:wlan0;interface-name:uap0
EOF
sudo systemctl reload NetworkManager
```

| 命令 | 作用 |
|------|------|
| `tee ... << 'EOF'` | 写入 NM 配置：将 `wlan0`、`uap0` 标为 **unmanaged**，避免与手工 hostapd/wpa 冲突。 |
| `systemctl reload NetworkManager` | 重新加载 NM 配置（或 `restart`，视环境而定）。 |

---

### 5.3 停用可能与 systemd 冲突的 legacy 网络（若存在）

若存在 **`/etc/network/interfaces.d/`** 下对 `wlan0`/`uap0` 的 `auto` + `wpa-ssid` 等配置，会与 `wpa_supplicant@wlan0` 抢控制口，应**移走或改名**：

```bash
sudo mv /etc/network/interfaces.d/wlan0-uap0 /etc/network/interfaces.d/wlan0-uap0.backup 2>/dev/null || true
sudo systemctl disable networking 2>/dev/null || true
sudo systemctl stop networking 2>/dev/null || true
```

| 命令 | 作用 |
|------|------|
| `mv ... wlan0-uap0 ...backup` | 移走 ifupdown 对无线的配置，避免开机 `ifup` 启动另一套 wpa。 |
| `systemctl disable/stop networking` | 关闭 legacy **networking.service**，避免再次 `ifup` 无线。 |

---

### 5.4 STA：`wpa_supplicant-wlan0.conf`

```bash
sudo tee /etc/wpa_supplicant/wpa_supplicant-wlan0.conf << 'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=1
country=CN

network={
    ssid="QHYCCD503"
    psk="你的上级WiFi密码"
}
EOF
sudo chmod 600 /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
```

| 项 | 作用 |
|------|------|
| `ctrl_interface=` | wpa 控制套接字目录，供 `wpa_cli` 使用。 |
| `country=CN` | 监管域（与 `hostapd` 中 `country_code` 一致，按需改）。 |
| `network={ ssid psk }` | 定义要连接的 WPA2-PSK 网络。 |

**启用并启动 wpa 服务**：

```bash
sudo systemctl enable --now wpa_supplicant@wlan0
```

| 命令 | 作用 |
|------|------|
| `enable --now wpa_supplicant@wlan0` | 开机自启并立即启动，使用 `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`、接口 `wlan0`。 |

**（推荐）覆盖片段**：增加 `RuntimeDirectory`、清理陈旧 socket、在 NM 之后启动等，见实际文件 `/etc/systemd/system/wpa_supplicant@wlan0.service.d/override.conf`。

---

### 5.5 wlan0 获取上游地址（dhclient）

```bash
sudo tee /etc/systemd/system/wlan0-dhcp.service << 'EOF'
[Unit]
Description=DHCP client on wlan0 (STA uplink)
After=wpa_supplicant@wlan0.service
Wants=wpa_supplicant@wlan0.service

[Service]
Type=forking
PIDFile=/run/dhclient.wlan0.pid
ExecStart=/sbin/dhclient -4 -pf /run/dhclient.wlan0.pid wlan0
ExecStop=/sbin/dhclient -x -pf /run/dhclient.wlan0.pid wlan0
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now wlan0-dhcp.service
```

| 命令 | 作用 |
|------|------|
| `dhclient -4 -pf ... wlan0` | 在 `wlan0` 上跑 **IPv4 DHCP 客户端**，PID 写入指定文件便于停止。 |
| `enable --now wlan0-dhcp` | 开机自启 DHCP 客户端。 |

---

### 5.6 创建 uap0 并设置热点网关 IP

**脚本**（示例网段 **10.42.0.1/24**）：

```bash
sudo tee /usr/local/sbin/uap0-create.sh << 'EOF'
#!/bin/bash
set -e
for i in $(seq 1 45); do
  [ -e /sys/class/net/wlan0 ] && break
  sleep 1
done
[ -e /sys/class/net/wlan0 ] || { echo "wlan0 not found" >&2; exit 1; }
ip link set wlan0 up
for i in $(seq 1 45); do
  iw dev wlan0 link 2>/dev/null | grep -q 'SSID: QHYCCD503' && break
  sleep 1
done
if iw dev 2>/dev/null | grep -q 'Interface uap0'; then
  ip link set uap0 down 2>/dev/null || true
  iw dev uap0 del 2>/dev/null || true
  sleep 1
fi
iw dev wlan0 interface add uap0 type __ap
ip link set uap0 up
ip addr flush dev uap0 2>/dev/null || true
ip addr add 10.42.0.1/24 dev uap0
exit 0
EOF
sudo chmod 755 /usr/local/sbin/uap0-create.sh
```

| 命令/段落 | 作用 |
|------|------|
| `iw dev wlan0 link \| grep SSID` | 等待 STA 先关联到指定上级（同卡 AP+STA 更稳）。 |
| `iw dev wlan0 interface add uap0 type __ap` | 创建 **AP 类型**虚拟接口 **uap0**。 |
| `ip addr add 10.42.0.1/24 dev uap0` | 为热点侧设置网关地址。 |

**systemd 单元**：

```bash
sudo tee /etc/systemd/system/uap0-create.service << 'EOF'
[Unit]
Description=Create uap0 AP interface (AP+STA)
After=wpa_supplicant@wlan0.service sys-subsystem-net-devices-wlan0.device
Wants=wpa_supplicant@wlan0.service
Before=hostapd@uap0.service dnsmasq.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/uap0-create.sh

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now uap0-create.service
```

| 命令 | 作用 |
|------|------|
| `Before=hostapd@uap0.service` | 保证 **先**有 `uap0` 再启动 hostapd。 |
| `enable --now uap0-create` | 开机执行创建脚本。 |

---

### 5.7 hostapd（热点 SSID、开放网络）

```bash
sudo tee /etc/hostapd/uap0.conf << 'EOF'
interface=uap0
driver=nl80211
country_code=CN
ssid=LQ
hw_mode=g
channel=11
auth_algs=1
wmm_enabled=1
ignore_broadcast_ssid=0
wpa=0
EOF

sudo systemctl unmask hostapd@uap0 2>/dev/null || true
sudo systemctl enable --now hostapd@uap0
```

| 项 | 作用 |
|------|------|
| `interface=uap0` | 只在虚拟 AP 口上发信标。 |
| `ssid=LQ` | 热点名称；**改热点名即改此项**后 `restart hostapd@uap0`。 |
| `wpa=0` | 开放网络（无 WPA 密码）。 |
| `channel`/`hw_mode` | 与驱动/监管域有关；同卡 AP+STA 时实际信道常随 STA 侧变化。 |

**Debian 模板** `hostapd@.service` 读取 **`/etc/hostapd/uap0.conf`**（实例名 `uap0`）。

---

### 5.8 dnsmasq（仅 uap0 提供 DHCP）

```bash
sudo tee /etc/dnsmasq.conf << 'EOF'
conf-dir=/etc/dnsmasq.d
EOF

sudo tee /etc/dnsmasq.d/ap-uap0.conf << 'EOF'
interface=uap0
bind-interfaces
dhcp-range=10.42.0.10,10.42.0.200,255.255.255.0,24h
dhcp-option=option:router,10.42.0.1
dhcp-option=option:dns-server,10.42.0.1
server=8.8.8.8
server=8.8.4.4
EOF
```

**（可选）避免与 NM 残留 dnsmasq 抢端口**：

```bash
sudo mkdir -p /etc/systemd/system/dnsmasq.service.d
sudo tee /etc/systemd/system/dnsmasq.service.d/nm-cleanup.conf << 'EOF'
[Service]
ExecStartPre=-/bin/sh -c 'pkill -f "dnsmasq -C /tmp/dnsmasq.conf" 2>/dev/null; sleep 1; true'
EOF
```

**（可选）让 dnsmasq 在 uap0/hostapd 就绪后再起**：

```bash
sudo tee /etc/systemd/system/dnsmasq.service.d/after-uap0.conf << 'EOF'
[Unit]
After=uap0-create.service hostapd@uap0.service
Wants=uap0-create.service hostapd@uap0.service
EOF
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now dnsmasq.service
```

| 项 | 作用 |
|------|------|
| `interface=uap0` + `bind-interfaces` | 只在热点口监听 DHCP/DNS。 |
| `dhcp-range` | 分配给客户端的地址池，需与 **10.42.0.1/24** 一致。 |
| `dhcp-option router/dns-server` | 下发网关与 DNS（指向本机热点 IP）。 |

---

### 5.9 转发与 NAT（热点客户端经 wlan0 上网）

```bash
sudo tee /etc/sysctl.d/99-ap-sta-forward.conf << 'EOF'
net.ipv4.ip_forward=1
EOF
sudo sysctl -p /etc/sysctl.d/99-ap-sta-forward.conf
```

```bash
sudo tee /usr/local/sbin/ap-sta-nat.sh << 'EOF'
#!/bin/bash
set -e
case "$1" in
  start)
    /sbin/sysctl -w net.ipv4.ip_forward=1 >/dev/null
    /sbin/iptables -t nat -C POSTROUTING -s 10.42.0.0/24 -o wlan0 -j MASQUERADE 2>/dev/null || \
      /sbin/iptables -t nat -A POSTROUTING -s 10.42.0.0/24 -o wlan0 -j MASQUERADE
    /sbin/iptables -C FORWARD -i uap0 -o wlan0 -j ACCEPT 2>/dev/null || /sbin/iptables -A FORWARD -i uap0 -o wlan0 -j ACCEPT
    /sbin/iptables -C FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
      /sbin/iptables -A FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
    ;;
  stop)
    /sbin/iptables -t nat -D POSTROUTING -s 10.42.0.0/24 -o wlan0 -j MASQUERADE 2>/dev/null || true
    /sbin/iptables -D FORWARD -i uap0 -o wlan0 -j ACCEPT 2>/dev/null || true
    /sbin/iptables -D FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    ;;
  *)
    exit 1
    ;;
esac
EOF
sudo chmod 755 /usr/local/sbin/ap-sta-nat.sh

sudo tee /etc/systemd/system/ap-sta-nat.service << 'EOF'
[Unit]
Description=Masquerade uap0 clients via wlan0 uplink
After=network-online.target wlan0-dhcp.service
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/ap-sta-nat.sh start
ExecStop=/usr/local/sbin/ap-sta-nat.sh stop

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now ap-sta-nat.service
```

| 项 | 作用 |
|------|------|
| `ip_forward` | 允许内核转发（热点网段 → `wlan0`）。 |
| `iptables -t nat ... MASQUERADE` | 源地址为 **10.42.0.0/24**、从 **wlan0** 出去的流量做伪装。 |
| `FORWARD` 规则 | 允许 **uap0↔wlan0** 转发。 |

> 若修改热点网段，需同步改 **`ap-sta-nat.sh`**、**`uap0-create.sh`**、**`dnsmasq.d/ap-uap0.conf`** 中的地址。

---

### 5.10 有线/无线长期共存：源地址策略路由

当 `eth0` 与 `wlan0` 同时在线，尤其是二者在同一局域网时，系统的主路由表可能让访问有线 IP 的连接从无线口回包。表现为：有线口协商速率正常，但 SSH、Web 或文件传输仍然很慢。

推荐安装一个 oneshot 服务，在每次开机或网络就绪后按当前接口状态自动生成策略路由：

```bash
sudo tee /usr/local/sbin/quarcs-dual-lan-policy.sh << 'EOF'
#!/bin/sh
set -eu

GW=${GW:-}
ETH_DEV=${ETH_DEV:-eth0}
WIFI_DEV=${WIFI_DEV:-wlan0}
ETH_TABLE=${ETH_TABLE:-100}
WIFI_TABLE=${WIFI_TABLE:-101}

ipv4_of() {
  ip -4 -o addr show dev "$1" scope global 2>/dev/null | awk 'NR == 1 { split($4, a, "/"); print a[1] }'
}

connected_route_of() {
  ip -4 route show dev "$1" scope link 2>/dev/null | awk 'NR == 1 { print $1 }'
}

gateway_of() {
  if [ -n "$GW" ]; then
    printf '%s\n' "$GW"
    return 0
  fi
  ip -4 route show default dev "$1" 2>/dev/null | awk 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "via") { print $(i + 1); exit } }'
}

set_sysctl() {
  path="/proc/sys/net/ipv4/conf/$1/$2"
  [ -e "$path" ] && printf '%s\n' "$3" > "$path"
}

add_policy() {
  dev="$1"
  table="$2"
  src=$(ipv4_of "$dev")
  connected=$(connected_route_of "$dev")
  gw=$(gateway_of "$dev")

  [ -n "$src" ] || return 0
  [ -n "$connected" ] || return 0
  ip link show dev "$dev" >/dev/null 2>&1 || return 0

  ip route flush table "$table" || true
  ip route replace table "$table" "$connected" dev "$dev" src "$src"
  if [ -n "$gw" ]; then
    ip route replace table "$table" default via "$gw" dev "$dev" src "$src"
  fi

  while ip rule show | grep -q "lookup $table"; do
    ip rule del table "$table" || break
  done
  ip rule add pref "$table" from "$src/32" table "$table"
}

add_policy "$ETH_DEV" "$ETH_TABLE"
add_policy "$WIFI_DEV" "$WIFI_TABLE"

for scope in all default "$ETH_DEV" "$WIFI_DEV"; do
  set_sysctl "$scope" arp_filter 1
  set_sysctl "$scope" arp_ignore 1
  set_sysctl "$scope" arp_announce 2
  set_sysctl "$scope" rp_filter 0
done
EOF
sudo chmod 755 /usr/local/sbin/quarcs-dual-lan-policy.sh
```

```bash
sudo tee /etc/systemd/system/quarcs-dual-lan-policy.service << 'EOF'
[Unit]
Description=QUARCS dual LAN source policy routing
Wants=network-online.target
After=network-online.target NetworkManager.service networking.service wlan0-dhcp.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/quarcs-dual-lan-policy.sh

[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/quarcs-dual-lan-policy.timer << 'EOF'
[Unit]
Description=Refresh QUARCS dual LAN source policy routing

[Timer]
OnBootSec=20s
OnUnitActiveSec=30s
AccuracySec=5s
Unit=quarcs-dual-lan-policy.service

[Install]
WantedBy=timers.target
EOF

sudo tee /etc/sysctl.d/99-quarcs-dual-lan.conf << 'EOF'
net.ipv4.conf.all.arp_filter = 1
net.ipv4.conf.default.arp_filter = 1
net.ipv4.conf.all.arp_ignore = 1
net.ipv4.conf.default.arp_ignore = 1
net.ipv4.conf.all.arp_announce = 2
net.ipv4.conf.default.arp_announce = 2
net.ipv4.conf.all.rp_filter = 0
net.ipv4.conf.default.rp_filter = 0
EOF

sudo systemctl daemon-reload
sudo systemctl enable quarcs-dual-lan-policy.service
sudo systemctl enable --now quarcs-dual-lan-policy.timer
sudo systemctl restart quarcs-dual-lan-policy.service
```

| 项 | 作用 |
|------|------|
| `connected_route_of` | 从当前接口直连路由自动取得网段，不固定任何现场网段。 |
| `gateway_of` | 优先从该接口默认路由自动取得网关；如现场需要，也可用环境变量 `GW=...` 覆盖。 |
| `ip rule add from ...` | 让某个源地址固定查对应路由表，保证回包从同一接口出去。 |
| `quarcs-dual-lan-policy.timer` | 每 30 秒重跑一次幂等脚本，接口 IP 或网段变化后自动刷新规则。 |
| `arp_filter/arp_ignore/arp_announce` | 降低同网段双网卡 ARP 应答混乱。 |
| `rp_filter=0` | 避免反向路径过滤误伤多出口策略路由。 |

验证示例：

```bash
ip rule show
ip route show table 100
ip route show table 101
ip route get <客户端IP> from <eth0当前IP>
ip route get <客户端IP> from <wlan0当前IP>
```

期望结果是：`from <eth0当前IP>` 走 `eth0`，`from <wlan0当前IP>` 走 `wlan0`。

---

## 六、常用自检命令

```bash
/sbin/iw dev
/sbin/iw dev wlan0 link
ip -4 addr show dev wlan0
ip -4 addr show dev uap0
systemctl status wpa_supplicant@wlan0 wlan0-dhcp uap0-create hostapd@uap0 dnsmasq ap-sta-nat quarcs-dual-lan-policy.timer --no-pager
sudo ss -ulnp | grep -E '10.42.0.1:53|:67'
sudo iptables -t nat -L POSTROUTING -n -v
ip rule show
ip route show table 100
ip route show table 101
```

| 命令 | 作用 |
|------|------|
| `iw dev` | 确认 `wlan0`/`uap0` 与 SSID。 |
| `iw dev wlan0 link` | 确认 STA 已连上级。 |
| `ip -4 addr` | 查看 `wlan0`/`uap0` IPv4。 |
| `systemctl status ...` | 查看各服务是否 active。 |
| `ss ... :53 :67` | 确认 dnsmasq 在热点 IP 上监听 DNS/DHCP（端口因版本可能显示为 `domain`/`bootps`）。 |
| `iptables -t nat -L` | 确认 MASQUERADE 规则。 |
| `ip rule` / `ip route show table ...` | 确认有线/无线源地址策略路由。 |

---

## 七、修改网段（与改 SSID 类似：改配置后重启相关服务）

若将网关从 **10.42.0.1/24** 改为其它网段，需要**一致修改**：

1. **`/usr/local/sbin/uap0-create.sh`** 中 `ip addr add ... dev uap0`  
2. **`/etc/dnsmasq.d/ap-uap0.conf`** 中 `dhcp-range`、`dhcp-option`  
3. **`/usr/local/sbin/ap-sta-nat.sh`** 中 `10.42.0.0/24`  

然后执行：

```bash
sudo systemctl restart uap0-create
sudo systemctl restart hostapd@uap0
sudo systemctl restart dnsmasq
sudo systemctl restart ap-sta-nat
```

**作用**：重新应用地址、DHCP 与 NAT，避免旧网段残留。

---

## 八、附录：命令速查表

| 目的 | 命令 |
|------|------|
| 扫描 WiFi | `sudo /sbin/iw dev wlan0 scan` |
| STA 连接状态 | `/sbin/iw dev wlan0 link` |
| wpa 状态 | `sudo wpa_cli -i wlan0 status` |
| 改热点名 | 编辑 `/etc/hostapd/uap0.conf` 的 `ssid=` → `sudo systemctl restart hostapd@uap0` |
| 列出接口 | `/sbin/iw dev` |

---

## 九、补充说明：部署前必须确认的事项

以下几点是本次实际部署时验证过、但原文档中未充分展开的关键点。

### 9.1 先确认网卡是否支持 `managed + AP` 同时存在

并不是所有无线芯片都支持单卡并发 AP+STA。建议先执行：

```bash
/sbin/iw list
```

重点关注输出中的：

```text
valid interface combinations:
  * #{ managed } <= 1, #{ AP } <= 1, ...
```

若看不到同时包含 **`managed`** 与 **`AP`** 的组合，则本方案大概率无法在该网卡上成立。

### 9.2 单网卡并发时，AP 信道通常会跟随 STA 侧

当 `wlan0` 连接上游 WiFi，而 `uap0` 由同一张物理网卡派生时，驱动通常要求两者工作在**同一信道**或同一无线链路组合内。因此：

1. 若上游连的是 **5GHz**，热点大概率也会跑到 **5GHz**。  
2. 若上游连的是 **2.4GHz**，热点大概率也会落到 **2.4GHz**。  
3. `hostapd` 配置里的 `hw_mode` / `channel` 可能只是“启动参数”，最终实际信道可能被驱动改写。  

建议实际以：

```bash
/sbin/iw dev
/sbin/iw dev wlan0 link
```

为准，而不是只看 `hostapd` 配置文件。

### 9.3 若当前已用 NetworkManager 直接开热点，需要先迁移

若系统当前是通过 **NetworkManager 的共享热点** 在 `wlan0` 上直接开 AP，则需要先把它停掉，否则它会与 `hostapd` / `wpa_supplicant` 抢占 `wlan0`。

典型迁移动作包括：

1. 将 `wlan0`、`uap0` 从 NetworkManager 管理中摘除。  
2. 关闭当前激活的 NM 热点连接。  
3. 关闭旧热点的 `autoconnect`。  

本次提供的一键脚本已自动包含这部分处理。

### 9.4 `dnsmasq` 可能因 `uap0` 时序问题启动失败

即使 `uap0-create.service` 先于 `dnsmasq.service` 启动，也可能出现 `uap0` 已创建、但地址尚未完全就绪的情况，表现为：

```text
dnsmasq: unknown interface uap0
```

因此建议在 `dnsmasq.service` 的 drop-in 中增加 `ExecStartPre`，显式等待 `uap0` 出现，并补一次热点地址。下文的一键脚本已包含该处理。

### 9.5 不建议把开放热点长期作为远程入口

文档前文示例使用的是**开放热点**（`wpa=0`），适合临时调试，但如果后续要作为远程接入入口，建议改成 **WPA2-PSK**。下文脚本同时支持：

1. 开放热点  
2. 受密码保护的 WPA2 热点  

---

## 十、可直接部署的一键脚本

已提供一份可直接执行的脚本：

**路径**：`/home/quarcs/apsta-setup.sh`

脚本功能：

1. 检查是否为 root、是否存在 `wlan0`、以及网卡是否支持 `managed + AP` 并发。  
2. 备份已有配置到 `/root/apsta-backup-时间戳/`。  
3. 自动处理可能冲突的 **NetworkManager 热点** 与 **ifupdown** 旧配置。  
4. 写入 `wpa_supplicant`、`hostapd`、`dnsmasq`、`NAT`、`systemd` 单元。  
5. 写入有线/无线源地址策略路由服务和定时刷新器，按当前接口地址自动适配任意局域网网段。  
6. 自动按顺序拉起 `STA -> uap0 -> AP -> DHCP/DNS -> NAT -> 双 LAN 策略路由`。  
7. 生成后续维护入口 `apsta-wifi`。  

### 10.1 使用方法

先给脚本执行权限：

```bash
chmod +x /home/quarcs/apsta-setup.sh
```

**开放热点示例**：

```bash
sudo /home/quarcs/apsta-setup.sh \
  --wan-ssid "QHYCCD503" \
  --wan-psk "你的上级WiFi密码" \
  --ap-ssid "LQ"
```

**带密码热点示例（推荐）**：

```bash
sudo /home/quarcs/apsta-setup.sh \
  --wan-ssid "QHYCCD503" \
  --wan-psk "你的上级WiFi密码" \
  --ap-ssid "LQ" \
  --ap-psk "你的热点密码"
```

### 10.2 可选参数

```text
--wan-ssid <ssid>        上游 WiFi 名称（必填）
--wan-psk <password>     上游 WiFi 密码（必填）
--ap-ssid <ssid>         热点名，默认 LQ
--ap-psk <password>      启用 WPA2 热点密码
--ap-open                使用开放热点（默认）
--country <code>         国家码，默认 CN
--wlan-if <name>         STA 接口名，默认 wlan0
--ap-if <name>           AP 接口名，默认 uap0
--eth-if <name>          有线 LAN 接口名，默认 eth0
--lan-gw <ip>            双 LAN 策略路由网关覆盖值，默认按接口自动检测
--eth-policy-table <id>  有线源地址策略路由表编号，默认 100
--wlan-policy-table <id> 无线源地址策略路由表编号，默认 101
--no-dual-lan-policy     不安装 eth/wlan 源地址策略路由
--ap-addr <cidr>         热点网关地址，默认 10.42.0.1/24
--ap-net <cidr>          NAT 子网，默认 10.42.0.0/24
--dhcp-start <ip>        DHCP 起始地址
--dhcp-end <ip>          DHCP 结束地址
--dhcp-lease <time>      DHCP 租期，默认 24h
--ap-hw-mode <g|a>       强制 hostapd 模式
--ap-channel <num>       强制信道
--uplink-wait <sec>      等待 STA 关联的秒数，默认 25
```

### 10.3 错误处理能力

脚本已做以下基础错误处理：

1. 缺少依赖命令时直接退出。  
2. `wlan0` 不存在或网卡不支持并发时直接退出。  
3. 启动前自动备份已有配置。  
4. 任一步骤失败时输出出错行号，并打印关键 systemd 服务状态。  
5. `dnsmasq` 启动前会等待 `uap0` 存在，并补一次热点地址。  
6. 若 `STA` 在等待时间内未关联成功，脚本会给出告警，但仍继续把 AP 侧拉起，便于现场排障。  
7. 双 LAN 策略路由不固定 IP 或网段，会从接口当前地址、直连路由、默认路由自动生成规则。  

### 10.4 脚本执行后生成的维护入口

脚本会安装：

**命令**：`/usr/local/bin/apsta-wifi`

常用操作：

```bash
apsta-wifi status
sudo apsta-wifi scan
apsta-wifi show-uplink
sudo apsta-wifi set-uplink "<ssid>" "<password>"
apsta-wifi show-ap
sudo apsta-wifi set-ap-ssid "<ssid>"
sudo apsta-wifi restart
```

`apsta-wifi status` 会同时显示 `ip rule` 与策略路由表，便于检查有线/无线分流是否生效。

---

## 十一、建议的最小验收清单

部署完成后，建议至少确认以下项目：

1. `wlan0` 已关联上级 WiFi，且拿到上游地址。  
2. `uap0` 已创建，并持有 `10.42.0.1/24`。  
3. `hostapd@uap0`、`dnsmasq`、`ap-sta-nat` 均为 `active`，`quarcs-dual-lan-policy.timer` 为 `active`。  
4. 手机/电脑能搜到热点并连接成功。  
5. 热点客户端能拿到 DHCP 地址（如 `10.42.0.x`）。  
6. 热点客户端能访问 `10.42.0.1`，并可进一步访问外网。  
7. 系统重启后，上述状态能够自动恢复。  
8. 从有线地址发出的回包走 `eth0`，从无线地址发出的回包走 `wlan0`，且不依赖固定网段。  

推荐验收命令：

```bash
apsta-wifi status
/sbin/iw dev
/sbin/iw dev wlan0 link
ip -4 addr show dev wlan0
ip -4 addr show dev uap0
systemctl status wpa_supplicant@wlan0 wlan0-dhcp uap0-create hostapd@uap0 dnsmasq ap-sta-nat quarcs-dual-lan-policy.timer --no-pager
sudo ss -ulnp | grep -E '10.42.0.1:53|:67'
sudo iptables -t nat -L POSTROUTING -n -v
ip rule show
ip route show table 100
ip route show table 101
```

---

*文档版本：与仓库内说明同步；实际设备上的密码、SSID、网段请以现场配置为准。*
