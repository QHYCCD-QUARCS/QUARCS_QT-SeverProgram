#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_NAME="$(basename "$0")"

WLAN_IF="wlan0"
AP_IF="uap0"
ETH_IF="eth0"
COUNTRY="CN"
LAN_GW=""
ETH_POLICY_TABLE="100"
WLAN_POLICY_TABLE="101"
ENABLE_DUAL_LAN_POLICY=1
WAN_SSID=""
WAN_PSK=""
AP_SSID="LQ"
AP_PSK=""
AP_OPEN=1
AP_ADDR="10.42.0.1/24"
AP_NET_CIDR="10.42.0.0/24"
AP_DHCP_START="10.42.0.10"
AP_DHCP_END="10.42.0.200"
AP_DHCP_LEASE="24h"
UPLINK_WAIT_SEC="25"
AP_HW_MODE=""
AP_CHANNEL=""
BACKUP_DIR=""

log() {
  printf '[%s] %s\n' "$(date '+%F %T')" "$*"
}

warn() {
  printf '[%s] WARNING: %s\n' "$(date '+%F %T')" "$*" >&2
}

die() {
  printf '[%s] ERROR: %s\n' "$(date '+%F %T')" "$*" >&2
  exit 1
}

on_error() {
  local lineno="$1"
  warn "script failed at line ${lineno}"
  warn "recent service states:"
  systemctl --no-pager --full status \
    "wpa_supplicant@${WLAN_IF}.service" \
    wlan0-dhcp.service \
    uap0-create.service \
    "hostapd@${AP_IF}.service" \
    dnsmasq.service \
    ap-sta-nat.service \
    quarcs-dual-lan-policy.service 2>/dev/null || true
}

trap 'on_error $LINENO' ERR

usage() {
  cat <<EOF
Usage:
  sudo ./${SCRIPT_NAME} --wan-ssid <ssid> --wan-psk <password> [options]

Required:
  --wan-ssid <ssid>          Upstream Wi-Fi SSID for STA mode.
  --wan-psk <password>       Upstream Wi-Fi password.

Optional:
  --ap-ssid <ssid>           Hotspot SSID. Default: ${AP_SSID}
  --ap-psk <password>        Enable WPA2-PSK on hotspot.
  --ap-open                  Keep hotspot open. Default behavior.
  --country <code>           Regulatory domain. Default: ${COUNTRY}
  --wlan-if <name>           STA interface. Default: ${WLAN_IF}
  --ap-if <name>             AP interface. Default: ${AP_IF}
  --eth-if <name>            Wired LAN interface. Default: ${ETH_IF}
  --lan-gw <ip>              Override gateway for wired/wireless policy routing. Default: auto-detect per interface.
  --eth-policy-table <id>    Routing table ID for wired source address. Default: ${ETH_POLICY_TABLE}
  --wlan-policy-table <id>   Routing table ID for wireless source address. Default: ${WLAN_POLICY_TABLE}
  --no-dual-lan-policy       Do not install eth/wlan source policy routing.
  --ap-addr <cidr>           Hotspot gateway address. Default: ${AP_ADDR}
  --ap-net <cidr>            Hotspot subnet for NAT. Default: ${AP_NET_CIDR}
  --dhcp-start <ip>          DHCP pool start. Default: ${AP_DHCP_START}
  --dhcp-end <ip>            DHCP pool end. Default: ${AP_DHCP_END}
  --dhcp-lease <time>        DHCP lease time. Default: ${AP_DHCP_LEASE}
  --ap-hw-mode <g|a>         Force hostapd hw_mode. Auto-detect by uplink if omitted.
  --ap-channel <num>         Force hostapd channel. Auto-detect by uplink if omitted.
  --uplink-wait <sec>        Seconds to wait for STA association. Default: ${UPLINK_WAIT_SEC}
  -h, --help                 Show this help.

Examples:
  sudo ./${SCRIPT_NAME} --wan-ssid QHYCCD503 --wan-psk 'your-password' --ap-ssid LQ
  sudo ./${SCRIPT_NAME} --wan-ssid QHYCCD503 --wan-psk 'your-password' --ap-ssid LQ --ap-psk 'change-me-now'
EOF
}

need_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    exec sudo "$0" "$@"
  fi
}

require_commands() {
  local missing=()
  local cmd
  for cmd in iw ip systemctl wpa_passphrase hostapd dnsmasq dhclient iptables awk sed; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
      missing+=("${cmd}")
    fi
  done
  if ((${#missing[@]} > 0)); then
    die "missing required commands: ${missing[*]}"
  fi
}

parse_args() {
  while (($# > 0)); do
    case "$1" in
      --wan-ssid)
        WAN_SSID="${2:-}"
        shift 2
        ;;
      --wan-psk)
        WAN_PSK="${2:-}"
        shift 2
        ;;
      --ap-ssid)
        AP_SSID="${2:-}"
        shift 2
        ;;
      --ap-psk)
        AP_PSK="${2:-}"
        AP_OPEN=0
        shift 2
        ;;
      --ap-open)
        AP_PSK=""
        AP_OPEN=1
        shift
        ;;
      --country)
        COUNTRY="${2:-}"
        shift 2
        ;;
      --wlan-if)
        WLAN_IF="${2:-}"
        shift 2
        ;;
      --ap-if)
        AP_IF="${2:-}"
        shift 2
        ;;
      --eth-if)
        ETH_IF="${2:-}"
        shift 2
        ;;
      --lan-gw)
        LAN_GW="${2:-}"
        shift 2
        ;;
      --eth-policy-table)
        ETH_POLICY_TABLE="${2:-}"
        shift 2
        ;;
      --wlan-policy-table)
        WLAN_POLICY_TABLE="${2:-}"
        shift 2
        ;;
      --no-dual-lan-policy)
        ENABLE_DUAL_LAN_POLICY=0
        shift
        ;;
      --ap-addr)
        AP_ADDR="${2:-}"
        shift 2
        ;;
      --ap-net)
        AP_NET_CIDR="${2:-}"
        shift 2
        ;;
      --dhcp-start)
        AP_DHCP_START="${2:-}"
        shift 2
        ;;
      --dhcp-end)
        AP_DHCP_END="${2:-}"
        shift 2
        ;;
      --dhcp-lease)
        AP_DHCP_LEASE="${2:-}"
        shift 2
        ;;
      --ap-hw-mode)
        AP_HW_MODE="${2:-}"
        shift 2
        ;;
      --ap-channel)
        AP_CHANNEL="${2:-}"
        shift 2
        ;;
      --uplink-wait)
        UPLINK_WAIT_SEC="${2:-}"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "unknown argument: $1"
        ;;
    esac
  done
}

validate_inputs() {
  [[ -n "${WAN_SSID}" ]] || die "--wan-ssid is required"
  [[ -n "${WAN_PSK}" ]] || die "--wan-psk is required"
  [[ -n "${AP_SSID}" ]] || die "--ap-ssid cannot be empty"
  [[ -n "${COUNTRY}" ]] || die "--country cannot be empty"
  [[ -n "${ETH_IF}" ]] || die "--eth-if cannot be empty"
  [[ "${ETH_POLICY_TABLE}" =~ ^[0-9]+$ ]] || die "--eth-policy-table must be an integer"
  [[ "${WLAN_POLICY_TABLE}" =~ ^[0-9]+$ ]] || die "--wlan-policy-table must be an integer"
  [[ -n "${AP_ADDR}" && "${AP_ADDR}" == */* ]] || die "--ap-addr must be CIDR, e.g. 10.42.0.1/24"
  [[ -n "${AP_NET_CIDR}" && "${AP_NET_CIDR}" == */* ]] || die "--ap-net must be CIDR, e.g. 10.42.0.0/24"
  [[ -n "${AP_DHCP_START}" ]] || die "--dhcp-start cannot be empty"
  [[ -n "${AP_DHCP_END}" ]] || die "--dhcp-end cannot be empty"
  [[ "${UPLINK_WAIT_SEC}" =~ ^[0-9]+$ ]] || die "--uplink-wait must be an integer"
  if [[ -n "${AP_HW_MODE}" && "${AP_HW_MODE}" != "g" && "${AP_HW_MODE}" != "a" ]]; then
    die "--ap-hw-mode must be g or a"
  fi
  if [[ -n "${AP_CHANNEL}" && ! "${AP_CHANNEL}" =~ ^[0-9]+$ ]]; then
    die "--ap-channel must be an integer"
  fi
}

check_interfaces() {
  [[ -e "/sys/class/net/${WLAN_IF}" ]] || die "wireless interface ${WLAN_IF} not found"
}

check_radio_support() {
  local combos
  combos="$(iw list 2>/dev/null || true)"
  [[ -n "${combos}" ]] || die "failed to query wireless capabilities with 'iw list'"
  if ! awk '
    /valid interface combinations:/ {in_block=1; next}
    in_block && NF==0 {in_block=0}
    in_block && /managed/ && /AP/ {found=1}
    END {exit found ? 0 : 1}
  ' <<<"${combos}"; then
    die "wireless card does not advertise simultaneous managed + AP support"
  fi
}

backup_file() {
  local src="$1"
  if [[ -e "${src}" ]]; then
    mkdir -p "${BACKUP_DIR}"
    local dst="${BACKUP_DIR}${src}"
    mkdir -p "$(dirname "${dst}")"
    cp -a "${src}" "${dst}"
  fi
}

backup_existing_files() {
  BACKUP_DIR="/root/apsta-backup-$(date +%Y%m%d-%H%M%S)"
  backup_file "/etc/NetworkManager/conf.d/99-unmanage-wifi.conf"
  backup_file "/etc/wpa_supplicant/wpa_supplicant-${WLAN_IF}.conf"
  backup_file "/etc/systemd/system/wpa_supplicant@${WLAN_IF}.service.d/override.conf"
  backup_file "/etc/systemd/system/wlan0-dhcp.service"
  backup_file "/usr/local/sbin/${AP_IF}-create.sh"
  backup_file "/etc/systemd/system/uap0-create.service"
  backup_file "/etc/hostapd/${AP_IF}.conf"
  backup_file "/etc/dnsmasq.conf"
  backup_file "/etc/dnsmasq.d/ap-${AP_IF}.conf"
  backup_file "/etc/systemd/system/dnsmasq.service.d/after-${AP_IF}.conf"
  backup_file "/etc/systemd/system/dnsmasq.service.d/nm-cleanup.conf"
  backup_file "/etc/sysctl.d/99-ap-sta-forward.conf"
  backup_file "/usr/local/sbin/ap-sta-nat.sh"
  backup_file "/etc/systemd/system/ap-sta-nat.service"
  backup_file "/usr/local/sbin/quarcs-dual-lan-policy.sh"
  backup_file "/etc/systemd/system/quarcs-dual-lan-policy.service"
  backup_file "/etc/systemd/system/quarcs-dual-lan-policy.timer"
  backup_file "/etc/sysctl.d/99-quarcs-dual-lan.conf"
  backup_file "/usr/local/bin/apsta-wifi"
  if [[ -d "${BACKUP_DIR}" ]]; then
    log "backups saved under ${BACKUP_DIR}"
  fi
}

disable_ifupdown_conflicts() {
  if [[ -e /etc/network/interfaces.d/wlan0-uap0 ]]; then
    mv /etc/network/interfaces.d/wlan0-uap0 \
      "/etc/network/interfaces.d/wlan0-uap0.backup-by-apsta-$(date +%Y%m%d-%H%M%S)"
  fi
  systemctl disable networking >/dev/null 2>&1 || true
  systemctl stop networking >/dev/null 2>&1 || true
}

write_nm_unmanaged() {
  mkdir -p /etc/NetworkManager/conf.d
  cat > /etc/NetworkManager/conf.d/99-unmanage-wifi.conf <<EOF
[keyfile]
unmanaged-devices=interface-name:${WLAN_IF};interface-name:${AP_IF}
EOF
  if systemctl is-active NetworkManager.service >/dev/null 2>&1; then
    systemctl reload NetworkManager.service || systemctl restart NetworkManager.service || true
  fi
}

shutdown_nm_wifi_conflicts() {
  if ! command -v nmcli >/dev/null 2>&1; then
    return 0
  fi

  local active_names=()
  mapfile -t active_names < <(nmcli -t -f NAME,DEVICE,TYPE connection show --active 2>/dev/null | \
    awk -F: -v dev="${WLAN_IF}" '$2 == dev && $3 == "802-11-wireless" {print $1}')

  local name mode
  for name in "${active_names[@]:-}"; do
    [[ -n "${name}" ]] || continue
    mode="$(nmcli -g 802-11-wireless.mode connection show "${name}" 2>/dev/null || true)"
    if [[ "${mode}" == "ap" ]]; then
      log "disabling active NetworkManager hotspot: ${name}"
      nmcli connection modify "${name}" connection.autoconnect no >/dev/null 2>&1 || true
      nmcli connection down "${name}" >/dev/null 2>&1 || true
    fi
  done

  nmcli device set "${WLAN_IF}" managed no >/dev/null 2>&1 || true
}

write_wpa_supplicant_conf() {
  local conf="/etc/wpa_supplicant/wpa_supplicant-${WLAN_IF}.conf"
  mkdir -p /etc/wpa_supplicant
  {
    printf 'ctrl_interface=/run/wpa_supplicant\n'
    printf 'update_config=1\n'
    printf 'country=%s\n' "${COUNTRY}"
    wpa_passphrase "${WAN_SSID}" "${WAN_PSK}"
  } > "${conf}.tmp"
  mv "${conf}.tmp" "${conf}"
  chmod 600 "${conf}"
}

write_wpa_override() {
  mkdir -p "/etc/systemd/system/wpa_supplicant@${WLAN_IF}.service.d"
  cat > "/etc/systemd/system/wpa_supplicant@${WLAN_IF}.service.d/override.conf" <<EOF
[Unit]
After=NetworkManager.service
Wants=NetworkManager.service

[Service]
RuntimeDirectory=wpa_supplicant
RuntimeDirectoryMode=0755
ExecStartPre=/bin/sh -c 'rm -f /run/wpa_supplicant/${WLAN_IF}'
EOF
}

write_dhcp_service() {
  cat > /etc/systemd/system/wlan0-dhcp.service <<EOF
[Unit]
Description=DHCP client on ${WLAN_IF} (STA uplink)
After=wpa_supplicant@${WLAN_IF}.service
Wants=wpa_supplicant@${WLAN_IF}.service

[Service]
Type=forking
PIDFile=/run/dhclient.${WLAN_IF}.pid
ExecStart=/sbin/dhclient -4 -pf /run/dhclient.${WLAN_IF}.pid ${WLAN_IF}
ExecStop=/sbin/dhclient -x -pf /run/dhclient.${WLAN_IF}.pid ${WLAN_IF}
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
}

write_uap0_create_script() {
  cat > "/usr/local/sbin/${AP_IF}-create.sh" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail

for _ in \$(seq 1 30); do
  [[ -e /sys/class/net/${WLAN_IF} ]] && break
  sleep 1
done
[[ -e /sys/class/net/${WLAN_IF} ]] || { echo '${WLAN_IF} not found' >&2; exit 1; }

ip link set ${WLAN_IF} up

connected=0
for _ in \$(seq 1 ${UPLINK_WAIT_SEC}); do
  if iw dev ${WLAN_IF} link 2>/dev/null | grep -q '^Connected to '; then
    connected=1
    break
  fi
  sleep 1
done
if [[ "\${connected}" -eq 0 ]]; then
  echo 'warning: ${WLAN_IF} is not associated yet; continuing with ${AP_IF} creation' >&2
fi

if iw dev 2>/dev/null | grep -q 'Interface ${AP_IF}'; then
  ip link set ${AP_IF} down 2>/dev/null || true
  iw dev ${AP_IF} del 2>/dev/null || true
  sleep 1
fi

iw dev ${WLAN_IF} interface add ${AP_IF} type __ap
ip link set ${AP_IF} up
ip addr replace ${AP_ADDR} dev ${AP_IF}
EOF
  chmod 755 "/usr/local/sbin/${AP_IF}-create.sh"

  cat > /etc/systemd/system/uap0-create.service <<EOF
[Unit]
Description=Create ${AP_IF} AP interface (AP+STA)
After=sys-subsystem-net-devices-${WLAN_IF}.device
Wants=sys-subsystem-net-devices-${WLAN_IF}.device
Before=hostapd@${AP_IF}.service dnsmasq.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/${AP_IF}-create.sh

[Install]
WantedBy=multi-user.target
EOF
}

detect_uplink_channel() {
  if [[ -n "${AP_HW_MODE}" && -n "${AP_CHANNEL}" ]]; then
    return 0
  fi

  local freq
  freq="$(iw dev "${WLAN_IF}" link 2>/dev/null | awk '/freq:/ {print $2; exit}')"
  if [[ -z "${freq}" ]]; then
    AP_HW_MODE="${AP_HW_MODE:-a}"
    AP_CHANNEL="${AP_CHANNEL:-36}"
    warn "could not detect uplink frequency; using fallback AP channel ${AP_CHANNEL}/${AP_HW_MODE}"
    return 0
  fi

  if ((freq >= 2412 && freq <= 2484)); then
    AP_HW_MODE="${AP_HW_MODE:-g}"
    if ((freq == 2484)); then
      AP_CHANNEL="${AP_CHANNEL:-14}"
    else
      AP_CHANNEL="${AP_CHANNEL:-$(((freq - 2407) / 5))}"
    fi
  elif ((freq >= 5000 && freq <= 5900)); then
    AP_HW_MODE="${AP_HW_MODE:-a}"
    AP_CHANNEL="${AP_CHANNEL:-$(((freq - 5000) / 5))}"
  else
    AP_HW_MODE="${AP_HW_MODE:-a}"
    AP_CHANNEL="${AP_CHANNEL:-36}"
    warn "unknown uplink frequency ${freq}; using fallback AP channel ${AP_CHANNEL}/${AP_HW_MODE}"
  fi
}

write_hostapd_conf() {
  mkdir -p /etc/hostapd
  {
    printf 'interface=%s\n' "${AP_IF}"
    printf 'driver=nl80211\n'
    printf 'country_code=%s\n' "${COUNTRY}"
    printf 'ssid=%s\n' "${AP_SSID}"
    printf 'hw_mode=%s\n' "${AP_HW_MODE}"
    printf 'channel=%s\n' "${AP_CHANNEL}"
    printf 'auth_algs=1\n'
    printf 'wmm_enabled=1\n'
    printf 'ignore_broadcast_ssid=0\n'
    if [[ "${AP_OPEN}" -eq 1 ]]; then
      printf 'wpa=0\n'
    else
      printf 'wpa=2\n'
      printf 'wpa_key_mgmt=WPA-PSK\n'
      printf 'rsn_pairwise=CCMP\n'
      printf 'wpa_passphrase=%s\n' "${AP_PSK}"
    fi
  } > "/etc/hostapd/${AP_IF}.conf"
}

write_dnsmasq_conf() {
  mkdir -p /etc/dnsmasq.d /etc/systemd/system/dnsmasq.service.d
  cat > /etc/dnsmasq.conf <<EOF
conf-dir=/etc/dnsmasq.d
EOF

  cat > "/etc/dnsmasq.d/ap-${AP_IF}.conf" <<EOF
interface=${AP_IF}
bind-interfaces
dhcp-range=${AP_DHCP_START},${AP_DHCP_END},255.255.255.0,${AP_DHCP_LEASE}
dhcp-option=option:router,${AP_ADDR%/*}
dhcp-option=option:dns-server,${AP_ADDR%/*}
server=8.8.8.8
server=8.8.4.4
EOF

  cat > /etc/systemd/system/dnsmasq.service.d/nm-cleanup.conf <<EOF
[Service]
ExecStartPre=-/bin/sh -c 'pkill -f "dnsmasq -C /tmp/dnsmasq.conf" 2>/dev/null; sleep 1; true'
EOF

  cat > "/etc/systemd/system/dnsmasq.service.d/after-${AP_IF}.conf" <<EOF
[Unit]
After=uap0-create.service hostapd@${AP_IF}.service
Wants=uap0-create.service hostapd@${AP_IF}.service

[Service]
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 10); do ip link show ${AP_IF} >/dev/null 2>&1 && break; sleep 1; done; ip addr replace ${AP_ADDR} dev ${AP_IF}'
EOF
}

write_forwarding_nat() {
  mkdir -p /etc/sysctl.d
  cat > /etc/sysctl.d/99-ap-sta-forward.conf <<EOF
net.ipv4.ip_forward=1
EOF

  cat > /usr/local/sbin/ap-sta-nat.sh <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail

case "\${1:-}" in
  start)
    /sbin/sysctl -w net.ipv4.ip_forward=1 >/dev/null
    /sbin/iptables -t nat -C POSTROUTING -s ${AP_NET_CIDR} -o ${WLAN_IF} -j MASQUERADE 2>/dev/null || \
      /sbin/iptables -t nat -A POSTROUTING -s ${AP_NET_CIDR} -o ${WLAN_IF} -j MASQUERADE
    /sbin/iptables -C FORWARD -i ${AP_IF} -o ${WLAN_IF} -j ACCEPT 2>/dev/null || \
      /sbin/iptables -A FORWARD -i ${AP_IF} -o ${WLAN_IF} -j ACCEPT
    /sbin/iptables -C FORWARD -i ${WLAN_IF} -o ${AP_IF} -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
      /sbin/iptables -A FORWARD -i ${WLAN_IF} -o ${AP_IF} -m state --state RELATED,ESTABLISHED -j ACCEPT
    ;;
  stop)
    /sbin/iptables -t nat -D POSTROUTING -s ${AP_NET_CIDR} -o ${WLAN_IF} -j MASQUERADE 2>/dev/null || true
    /sbin/iptables -D FORWARD -i ${AP_IF} -o ${WLAN_IF} -j ACCEPT 2>/dev/null || true
    /sbin/iptables -D FORWARD -i ${WLAN_IF} -o ${AP_IF} -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    ;;
  *)
    echo 'Usage: ap-sta-nat.sh {start|stop}' >&2
    exit 1
    ;;
esac
EOF
  chmod 755 /usr/local/sbin/ap-sta-nat.sh

  cat > /etc/systemd/system/ap-sta-nat.service <<EOF
[Unit]
Description=Masquerade ${AP_IF} clients via ${WLAN_IF} uplink
After=network-online.target wlan0-dhcp.service
Wants=network-online.target wlan0-dhcp.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/ap-sta-nat.sh start
ExecStop=/usr/local/sbin/ap-sta-nat.sh stop

[Install]
WantedBy=multi-user.target
EOF
}

write_dual_lan_policy() {
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -ne 1 ]]; then
    systemctl disable quarcs-dual-lan-policy.service >/dev/null 2>&1 || true
    return 0
  fi

  mkdir -p /etc/sysctl.d
  cat > /usr/local/sbin/quarcs-dual-lan-policy.sh <<EOF
#!/bin/sh
set -eu

GW=\${GW:-${LAN_GW}}
ETH_DEV=\${ETH_DEV:-${ETH_IF}}
WIFI_DEV=\${WIFI_DEV:-${WLAN_IF}}
ETH_TABLE=\${ETH_TABLE:-${ETH_POLICY_TABLE}}
WIFI_TABLE=\${WIFI_TABLE:-${WLAN_POLICY_TABLE}}

ipv4_of() {
  ip -4 -o addr show dev "\$1" scope global 2>/dev/null | awk 'NR == 1 { split(\$4, a, "/"); print a[1] }'
}

connected_route_of() {
  ip -4 route show dev "\$1" scope link 2>/dev/null | awk 'NR == 1 { print \$1 }'
}

gateway_of() {
  if [ -n "\$GW" ]; then
    printf '%s\\n' "\$GW"
    return 0
  fi
  ip -4 route show default dev "\$1" 2>/dev/null | awk 'NR == 1 { for (i = 1; i <= NF; i++) if (\$i == "via") { print \$(i + 1); exit } }'
}

set_sysctl() {
  path="/proc/sys/net/ipv4/conf/\$1/\$2"
  [ -e "\$path" ] && printf '%s\\n' "\$3" > "\$path"
}

add_policy() {
  dev="\$1"
  table="\$2"
  src=\$(ipv4_of "\$dev")
  connected=\$(connected_route_of "\$dev")
  gw=\$(gateway_of "\$dev")

  [ -n "\$src" ] || return 0
  [ -n "\$connected" ] || return 0
  ip link show dev "\$dev" >/dev/null 2>&1 || return 0

  ip route flush table "\$table" || true
  ip route replace table "\$table" "\$connected" dev "\$dev" src "\$src"
  if [ -n "\$gw" ]; then
    ip route replace table "\$table" default via "\$gw" dev "\$dev" src "\$src"
  fi

  while ip rule show | grep -q "lookup \$table"; do
    ip rule del table "\$table" || break
  done
  ip rule add pref "\$table" from "\$src/32" table "\$table"
}

add_policy "\$ETH_DEV" "\$ETH_TABLE"
add_policy "\$WIFI_DEV" "\$WIFI_TABLE"

for scope in all default "\$ETH_DEV" "\$WIFI_DEV"; do
  set_sysctl "\$scope" arp_filter 1
  set_sysctl "\$scope" arp_ignore 1
  set_sysctl "\$scope" arp_announce 2
  set_sysctl "\$scope" rp_filter 0
done
EOF
  chmod 755 /usr/local/sbin/quarcs-dual-lan-policy.sh

  cat > /etc/systemd/system/quarcs-dual-lan-policy.service <<EOF
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

  cat > /etc/systemd/system/quarcs-dual-lan-policy.timer <<EOF
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

  cat > /etc/sysctl.d/99-quarcs-dual-lan.conf <<EOF
net.ipv4.conf.all.arp_filter = 1
net.ipv4.conf.default.arp_filter = 1
net.ipv4.conf.all.arp_ignore = 1
net.ipv4.conf.default.arp_ignore = 1
net.ipv4.conf.all.arp_announce = 2
net.ipv4.conf.default.arp_announce = 2
net.ipv4.conf.all.rp_filter = 0
net.ipv4.conf.default.rp_filter = 0
EOF
}

write_helper_script() {
  cat > /usr/local/bin/apsta-wifi <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail

WLAN_IF="${WLAN_IF}"
AP_IF="${AP_IF}"
WPA_CONF="/etc/wpa_supplicant/wpa_supplicant-${WLAN_IF}.conf"
HOSTAPD_CONF="/etc/hostapd/${AP_IF}.conf"

need_root() {
  if [[ "\${EUID}" -ne 0 ]]; then
    exec sudo "\$0" "\$@"
  fi
}

read_kv() {
  local file="\$1"
  local key="\$2"
  sudo awk -F= -v key="\${key}" '\$1 == key {print substr(\$0, index(\$0, "=") + 1); exit}' "\${file}"
}

restart_stack() {
  systemctl restart "wpa_supplicant@\${WLAN_IF}.service"
  systemctl restart wlan0-dhcp.service
  systemctl restart uap0-create.service
  systemctl restart "hostapd@\${AP_IF}.service"
  systemctl restart dnsmasq.service
  systemctl restart ap-sta-nat.service
  systemctl restart quarcs-dual-lan-policy.service 2>/dev/null || true
}

set_uplink() {
  local ssid="\$1"
  local psk="\$2"
  install -m 600 /dev/null "\${WPA_CONF}.tmp"
  {
    printf 'ctrl_interface=/run/wpa_supplicant\n'
    printf 'update_config=1\n'
    printf 'country=${COUNTRY}\n'
    wpa_passphrase "\${ssid}" "\${psk}"
  } > "\${WPA_CONF}.tmp"
  mv "\${WPA_CONF}.tmp" "\${WPA_CONF}"
  restart_stack
}

set_ap_ssid() {
  local ssid="\$1"
  python3 - "\${HOSTAPD_CONF}" "\${ssid}" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
ssid = sys.argv[2]
lines = []
for line in path.read_text(errors='ignore').splitlines():
    if line.startswith('ssid='):
        lines.append(f'ssid={ssid}')
    else:
        lines.append(line)
path.write_text('\\n'.join(lines) + '\\n')
PY
  systemctl restart "hostapd@\${AP_IF}.service"
}

scan_wifi() {
  ip link set "\${WLAN_IF}" up
  iw dev "\${WLAN_IF}" scan | awk '
    /^BSS / {bss=\$2; sub(/\\(.*/, "", bss); freq=""; signal=""}
    /^[[:space:]]*freq:/ {freq=\$2}
    /^[[:space:]]*signal:/ {signal=\$2" "\$3}
    /^[[:space:]]*SSID:/ {
      ssid=substr(\$0, index(\$0, "SSID:") + 6)
      printf "SSID: %s | FREQ: %s | SIGNAL: %s | BSS: %s\\n", ssid, freq, signal, bss
    }
  '
}

status_all() {
  echo "STA uplink SSID: \$(read_kv "\${WPA_CONF}" ssid | sed 's/^\"//; s/\"$//')"
  echo "AP SSID: \$(read_kv "\${HOSTAPD_CONF}" ssid)"
  echo
  iw dev || true
  echo
  iw dev "\${WLAN_IF}" link || true
  echo
  ip -4 addr show dev "\${WLAN_IF}" || true
  ip -4 addr show dev "\${AP_IF}" || true
  echo
  systemctl --no-pager --full status \
    "wpa_supplicant@\${WLAN_IF}.service" \
    wlan0-dhcp.service \
    uap0-create.service \
    "hostapd@\${AP_IF}.service" \
    dnsmasq.service \
    ap-sta-nat.service \
    quarcs-dual-lan-policy.service \
    quarcs-dual-lan-policy.timer || true
  echo
  ip rule show || true
  echo
  ip route show table ${ETH_POLICY_TABLE} 2>/dev/null || true
  ip route show table ${WLAN_POLICY_TABLE} 2>/dev/null || true
}

case "\${1:-}" in
  status)
    status_all
    ;;
  scan)
    need_root "\$@"
    scan_wifi
    ;;
  show-uplink)
    read_kv "\${WPA_CONF}" ssid | sed 's/^\"//; s/\"$//'
    ;;
  set-uplink)
    [[ \$# -eq 3 ]] || { echo 'Usage: apsta-wifi set-uplink <ssid> <password>' >&2; exit 1; }
    need_root "\$@"
    set_uplink "\$2" "\$3"
    ;;
  show-ap)
    read_kv "\${HOSTAPD_CONF}" ssid
    ;;
  set-ap-ssid)
    [[ \$# -eq 2 ]] || { echo 'Usage: apsta-wifi set-ap-ssid <ssid>' >&2; exit 1; }
    need_root "\$@"
    set_ap_ssid "\$2"
    ;;
  restart)
    need_root "\$@"
    restart_stack
    ;;
  *)
    cat >&2 <<'USAGE'
Usage:
  apsta-wifi status
  sudo apsta-wifi scan
  apsta-wifi show-uplink
  sudo apsta-wifi set-uplink <ssid> <password>
  apsta-wifi show-ap
  sudo apsta-wifi set-ap-ssid <ssid>
  sudo apsta-wifi restart
USAGE
    exit 1
    ;;
esac
EOF
  chmod 755 /usr/local/bin/apsta-wifi
}

enable_services() {
  systemctl daemon-reload
  systemctl unmask "hostapd@${AP_IF}.service" >/dev/null 2>&1 || true
  systemctl enable \
    "wpa_supplicant@${WLAN_IF}.service" \
    wlan0-dhcp.service \
    uap0-create.service \
    "hostapd@${AP_IF}.service" \
    dnsmasq.service \
    ap-sta-nat.service >/dev/null
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -eq 1 ]]; then
    systemctl enable quarcs-dual-lan-policy.service >/dev/null
    systemctl enable quarcs-dual-lan-policy.timer >/dev/null
  fi
}

start_sta_first() {
  log "starting STA stack"
  systemctl restart "wpa_supplicant@${WLAN_IF}.service"
  systemctl restart wlan0-dhcp.service
}

wait_for_sta_link() {
  local i
  for ((i = 1; i <= UPLINK_WAIT_SEC; i++)); do
    if iw dev "${WLAN_IF}" link 2>/dev/null | grep -q '^Connected to '; then
      log "${WLAN_IF} associated to uplink"
      return 0
    fi
    sleep 1
  done
  warn "${WLAN_IF} did not associate within ${UPLINK_WAIT_SEC}s; AP will still be configured"
  return 1
}

start_ap_stack() {
  log "starting AP stack"
  systemctl restart uap0-create.service
  systemctl restart "hostapd@${AP_IF}.service"
  systemctl restart dnsmasq.service
  systemctl restart ap-sta-nat.service
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -eq 1 ]]; then
    systemctl restart quarcs-dual-lan-policy.service
  fi
}

verify_services() {
  local svc
  for svc in \
    "wpa_supplicant@${WLAN_IF}.service" \
    wlan0-dhcp.service \
    uap0-create.service \
    "hostapd@${AP_IF}.service" \
    dnsmasq.service \
    ap-sta-nat.service; do
    if ! systemctl is-active "${svc}" >/dev/null 2>&1; then
      die "service is not active: ${svc}"
    fi
  done
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -eq 1 ]] && \
    systemctl is-failed quarcs-dual-lan-policy.service >/dev/null 2>&1; then
    die "service failed: quarcs-dual-lan-policy.service"
  fi
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -eq 1 ]] && \
    ! systemctl is-active quarcs-dual-lan-policy.timer >/dev/null 2>&1; then
    die "timer is not active: quarcs-dual-lan-policy.timer"
  fi
}

verify_runtime_state() {
  ip addr show dev "${AP_IF}" | grep -q "inet ${AP_ADDR%/*}/" || die "${AP_IF} is missing ${AP_ADDR}"
  if ! ss -ulnp 2>/dev/null | grep -q "${AP_ADDR%/*}:53"; then
    warn "dnsmasq DNS listener on ${AP_ADDR%/*}:53 not detected"
  fi
  if ! iw dev | grep -q "Interface ${AP_IF}"; then
    die "${AP_IF} interface not present after setup"
  fi
  if [[ "${ENABLE_DUAL_LAN_POLICY}" -eq 1 ]]; then
    if ip -4 addr show dev "${ETH_IF}" scope global >/dev/null 2>&1; then
      ip rule show | grep -q "lookup ${ETH_POLICY_TABLE}" || warn "wired policy route table ${ETH_POLICY_TABLE} is not active"
    fi
    ip rule show | grep -q "lookup ${WLAN_POLICY_TABLE}" || warn "wireless policy route table ${WLAN_POLICY_TABLE} is not active"
  fi
}

show_summary() {
  cat <<EOF

AP+STA deployment complete.
  STA interface : ${WLAN_IF}
  AP interface  : ${AP_IF}
  Uplink SSID   : ${WAN_SSID}
  Hotspot SSID  : ${AP_SSID}
  Hotspot IP    : ${AP_ADDR}
  AP channel    : ${AP_CHANNEL}
  AP hw_mode    : ${AP_HW_MODE}
  Dual LAN route: ${ENABLE_DUAL_LAN_POLICY}

Useful commands:
  apsta-wifi status
  sudo apsta-wifi scan
  sudo apsta-wifi set-uplink "<ssid>" "<password>"
  sudo apsta-wifi set-ap-ssid "<ssid>"
  sudo apsta-wifi restart
EOF
}

main() {
  need_root "$@"
  parse_args "$@"
  validate_inputs
  require_commands
  check_interfaces
  check_radio_support
  backup_existing_files
  disable_ifupdown_conflicts
  write_nm_unmanaged
  shutdown_nm_wifi_conflicts
  write_wpa_supplicant_conf
  write_wpa_override
  write_dhcp_service
  write_uap0_create_script
  write_forwarding_nat
  write_dual_lan_policy
  write_dnsmasq_conf
  write_helper_script
  sysctl -p /etc/sysctl.d/99-ap-sta-forward.conf >/dev/null
  enable_services
  start_sta_first
  wait_for_sta_link || true
  detect_uplink_channel
  write_hostapd_conf
  start_ap_stack
  verify_services
  verify_runtime_state
  show_summary
}

main "$@"
