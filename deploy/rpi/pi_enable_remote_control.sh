#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "[ERR] 请用 sudo 运行：sudo bash $0 [用户名]"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_USER="${1:-${SUDO_USER:-}}"
TIMEZONE="${TIMEZONE:-Asia/Shanghai}"

if [[ -z "${TARGET_USER}" ]]; then
  echo "[ERR] 未检测到目标用户。请显式传入，例如：sudo bash $0 quarcs"
  exit 1
fi

if ! id "${TARGET_USER}" >/dev/null 2>&1; then
  echo "[ERR] 用户不存在：${TARGET_USER}"
  exit 1
fi

for required in "${SCRIPT_DIR}/net-mode.sh" "${SCRIPT_DIR}/net-mode.conf" "${SCRIPT_DIR}/sudoers.d/quarcs-net"; do
  if [[ ! -f "${required}" ]]; then
    echo "[ERR] 缺少必需文件：${required}"
    exit 1
  fi
done

if ! command -v nmcli >/dev/null 2>&1; then
  echo "[ERR] 未找到 nmcli。当前盒子未安装或未启用 NetworkManager，无法配置远程网络控制。"
  exit 1
fi

echo "[INFO] 目标用户：${TARGET_USER}"
echo "[INFO] 校准系统时间与时区"
if command -v timedatectl >/dev/null 2>&1; then
  timedatectl status || true
  timedatectl set-timezone "${TIMEZONE}" || true
  timedatectl set-ntp true || true
  sleep 5
  timedatectl status || true
else
  echo "[WARN] 未找到 timedatectl，跳过自动校时"
fi

echo "[INFO] 安装/确认 SSH 服务"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends openssh-server ca-certificates
systemctl enable ssh
systemctl restart ssh

HOME_DIR="$(getent passwd "${TARGET_USER}" | awk -F: '{print $6}')"
if [[ -z "${HOME_DIR}" || ! -d "${HOME_DIR}" ]]; then
  echo "[ERR] 无法确定用户 home 目录：${TARGET_USER}"
  exit 1
fi

SSH_DIR="${HOME_DIR}/.ssh"
AUTH_KEYS="${SSH_DIR}/authorized_keys"
install -d -m 700 -o "${TARGET_USER}" -g "${TARGET_USER}" "${SSH_DIR}"
touch "${AUTH_KEYS}"
chown "${TARGET_USER}:${TARGET_USER}" "${AUTH_KEYS}"
chmod 600 "${AUTH_KEYS}"

echo
echo "[INFO] 如需配置 SSH 免密登录，请粘贴本机 public key；若暂时跳过，直接回车。"
read -r PUBKEY || true
if [[ -n "${PUBKEY}" ]]; then
  if grep -Fqx "${PUBKEY}" "${AUTH_KEYS}"; then
    echo "[OK] public key 已存在于 ${AUTH_KEYS}"
  else
    echo "${PUBKEY}" >> "${AUTH_KEYS}"
    chown "${TARGET_USER}:${TARGET_USER}" "${AUTH_KEYS}"
    echo "[OK] 已追加 public key 到 ${AUTH_KEYS}"
  fi
else
  echo "[INFO] 未输入 public key，保留现有 SSH 登录方式"
fi

detect_wlan_if() {
  nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="wifi"{print $1; exit}'
}

detect_eth_if() {
  nmcli -t -f DEVICE,TYPE device status | awk -F: '$2=="ethernet"{print $1; exit}'
}

connection_exists() {
  nmcli -t -f NAME connection show | grep -Fxq "$1"
}

first_ethernet_connection() {
  nmcli -t -f NAME,TYPE connection show | awk -F: '$2=="802-3-ethernet"{print $1; exit}'
}

WLAN_IF="$(detect_wlan_if || true)"
ETH_IF="$(detect_eth_if || true)"
WLAN_IF="${WLAN_IF:-wlan0}"
ETH_IF="${ETH_IF:-eth0}"

AP_CON_DEFAULT="RaspBerryPi-WiFi"
if connection_exists "${AP_CON_DEFAULT}"; then
  AP_CON="${AP_CON_DEFAULT}"
else
  echo
  echo "[WARN] 未找到默认热点连接名：${AP_CON_DEFAULT}"
  echo "[INFO] 当前 NetworkManager 连接列表如下："
  nmcli -t -f NAME,TYPE connection show || true
  read -r -p "请输入热点(AP)连接名: " AP_CON
  if [[ -z "${AP_CON}" ]]; then
    echo "[ERR] 热点连接名不能为空"
    exit 1
  fi
  if ! connection_exists "${AP_CON}"; then
    echo "[ERR] 连接不存在：${AP_CON}"
    exit 1
  fi
fi

WAN_ETH_DEFAULT="$(first_ethernet_connection || true)"
WAN_ETH_DEFAULT="${WAN_ETH_DEFAULT:-}"
if [[ -n "${WAN_ETH_DEFAULT}" ]]; then
  echo
  read -r -p "检测到有线连接名为 [${WAN_ETH_DEFAULT}]，直接回车接受，输入 none 表示留空: " WAN_ETH_INPUT
  WAN_ETH_INPUT="${WAN_ETH_INPUT:-${WAN_ETH_DEFAULT}}"
else
  echo
  read -r -p "未自动检测到有线连接名；如需填写请直接输入，没有则回车留空: " WAN_ETH_INPUT
fi

if [[ "${WAN_ETH_INPUT:-}" == "none" ]]; then
  WAN_ETH=""
else
  WAN_ETH="${WAN_ETH_INPUT:-}"
fi

escape_sudoers_arg() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value// /\\ }"
  printf '%s' "${value}"
}

AP_CON_ESCAPED="$(escape_sudoers_arg "${AP_CON}")"
WLAN_IF_ESCAPED="$(escape_sudoers_arg "${WLAN_IF}")"

echo
echo "[INFO] 安装 net-mode.sh 到 /usr/local/sbin/net-mode.sh"
install -m 0755 "${SCRIPT_DIR}/net-mode.sh" /usr/local/sbin/net-mode.sh

echo "[INFO] 生成 /etc/quarcs/net-mode.conf"
install -d -m 755 /etc/quarcs
cat > /etc/quarcs/net-mode.conf <<EOF
AP_CON="${AP_CON}"
WAN_ETH="${WAN_ETH}"
WAN_WIFIS=("wan-uplink")
WLAN_IF="${WLAN_IF}"
ETH_IF="${ETH_IF}"
WAN_TIMEOUT_SEC=75
PING_TARGET="1.1.1.1"
ZT_SERVICE="zerotier-one"
LOG_TAG="net-mode"
EOF
chmod 644 /etc/quarcs/net-mode.conf

echo "[INFO] 生成 /etc/sudoers.d/quarcs-net"
TMP_SUDOERS="$(mktemp)"
cleanup() {
  rm -f "${TMP_SUDOERS}"
}
trap cleanup EXIT

cat > "${TMP_SUDOERS}" <<EOF
Defaults:${TARGET_USER} !requiretty

${TARGET_USER} ALL=(root) NOPASSWD: /usr/local/sbin/net-mode.sh ap
${TARGET_USER} ALL=(root) NOPASSWD: /usr/local/sbin/net-mode.sh wan
${TARGET_USER} ALL=(root) NOPASSWD: /usr/local/sbin/net-mode.sh status

${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list ifname ${WLAN_IF_ESCAPED} --rescan yes
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli connection modify ${AP_CON_ESCAPED} 802-11-wireless.ssid *
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli connection down ${AP_CON_ESCAPED}
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli connection up ${AP_CON_ESCAPED}
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli -t -f NAME con show
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli con add type wifi ifname ${WLAN_IF_ESCAPED} con-name wan-uplink ssid *
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli con modify wan-uplink wifi-sec.key-mgmt wpa-psk wifi-sec.psk * autoconnect no ipv6.method ignore
${TARGET_USER} ALL=(root) NOPASSWD: /usr/bin/nmcli con modify wan-uplink 802-11-wireless.ssid * wifi-sec.key-mgmt wpa-psk wifi-sec.psk * autoconnect no ipv6.method ignore
${TARGET_USER} ALL=(root) NOPASSWD: /bin/systemctl start zerotier-one
${TARGET_USER} ALL=(root) NOPASSWD: /bin/systemctl stop zerotier-one
${TARGET_USER} ALL=(root) NOPASSWD: /bin/systemctl is-active zerotier-one
EOF

chmod 440 "${TMP_SUDOERS}"
visudo -cf "${TMP_SUDOERS}"
install -m 440 "${TMP_SUDOERS}" /etc/sudoers.d/quarcs-net

echo
echo "[INFO] 验证配置"
/usr/local/sbin/net-mode.sh status >/dev/null
su -s /bin/bash -c "sudo -n /usr/local/sbin/net-mode.sh status >/dev/null" "${TARGET_USER}"
su -s /bin/bash -c "sudo -n /usr/bin/nmcli -t -f NAME con show >/dev/null" "${TARGET_USER}"

echo
echo "[OK] 远程控制所需基础配置已完成"
echo "[OK] 当前热点连接名：${AP_CON}"
echo "[OK] WLAN 接口：${WLAN_IF}；ETH 接口：${ETH_IF}"
if [[ -n "${WAN_ETH}" ]]; then
  echo "[OK] 有线连接名：${WAN_ETH}"
else
  echo "[INFO] 未配置有线连接名，WAN 将只尝试已保存的 Wi-Fi profile"
fi
echo
echo "[NEXT] 现在可以启动你的 QT 程序，再测试手机端远程网络控制页面。"
echo "[NEXT] 如需再次自检，可执行：sudo /usr/local/sbin/net-mode.sh status"
