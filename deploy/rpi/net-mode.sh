#!/usr/bin/env bash
set -euo pipefail

# Network mode switcher for Raspberry Pi (NetworkManager)
# - AP mode: hotspot up, uplinks down, zerotier stopped
# - WAN mode: hotspot down, try eth / saved wifi profiles; on success start zerotier
# - status: print JSON status (no extra text)

CONF_FILE="/etc/quarcs/net-mode.conf"
[[ -f "$CONF_FILE" ]] && # shellcheck disable=SC1090
  source "$CONF_FILE"

# Defaults (can be overridden by conf)
AP_CON="${AP_CON:-RaspBerryPi-WiFi}"
WAN_ETH="${WAN_ETH:-}"
# WAN_WIFIS is an array in conf. When unset (with `set -u`) we must initialize safely.
if ! declare -p WAN_WIFIS >/dev/null 2>&1; then
  WAN_WIFIS=()
fi
WLAN_IF="${WLAN_IF:-wlan0}"
ETH_IF="${ETH_IF:-eth0}"
WAN_TIMEOUT_SEC="${WAN_TIMEOUT_SEC:-75}"
PING_TARGET="${PING_TARGET:-1.1.1.1}"
ZT_SERVICE="${ZT_SERVICE:-zerotier-one}"
LOG_TAG="${LOG_TAG:-net-mode}"

log(){ logger -t "$LOG_TAG" "$*"; echo "[$(date +'%F %T')] $*"; }

con_up(){ nmcli -t -f NAME con show --active | grep -Fxq "$1"; }
wlan_has_ip(){ ip -4 addr show dev "$WLAN_IF" | grep -q 'inet '; }
has_default_route(){ ip route | grep -q '^default '; }
ping_ok(){ ping -c 1 -W 1 "$PING_TARGET" >/dev/null 2>&1; }

eth_link_up(){
  [[ -d "/sys/class/net/$ETH_IF" ]] || return 1
  [[ "$(cat "/sys/class/net/$ETH_IF/carrier" 2>/dev/null || echo 0)" == "1" ]]
}

zt_start(){ systemctl start "$ZT_SERVICE" >/dev/null 2>&1 || true; }
zt_stop(){ systemctl stop  "$ZT_SERVICE" >/dev/null 2>&1 || true; }
zt_running(){ systemctl is-active "$ZT_SERVICE" >/dev/null 2>&1; }

ap_on(){
  log "Switch -> AP mode"
  # IMPORTANT:
  # In AP mode we keep Ethernet up (if plugged) to avoid killing remote sessions (e.g. VNC/SSH over LAN).
  # We only ensure hotspot is up and stop ZeroTier to avoid remote routing side effects.
  for c in "${WAN_WIFIS[@]:-}"; do nmcli con down "$c" >/dev/null 2>&1 || true; done
  nmcli con up "$AP_CON" >/dev/null 2>&1 || true
  zt_stop
}

wan_try(){
  log "Switch -> WAN mode (try uplink)"
  nmcli con down "$AP_CON" >/dev/null 2>&1 || true

  if [[ -n "${WAN_ETH:-}" ]] && eth_link_up; then
    log "Ethernet link up, try DHCP via ${WAN_ETH}"
    nmcli con up "$WAN_ETH" >/dev/null 2>&1 || true
    local start now; start="$(date +%s)"
    while true; do
      now="$(date +%s)"
      if has_default_route && ping_ok; then
        log "WAN OK via Ethernet"
        zt_start
        return 0
      fi
      if (( now - start > WAN_TIMEOUT_SEC )); then
        log "WAN FAIL via Ethernet (timeout)"
        nmcli con down "$WAN_ETH" >/dev/null 2>&1 || true
        return 1
      fi
      sleep 2
    done
  fi

  for c in "${WAN_WIFIS[@]:-}"; do
    [[ -n "$c" ]] || continue
    log "Trying Wi-Fi uplink: $c"
    nmcli con up "$c" >/dev/null 2>&1 || true
    local start now; start="$(date +%s)"
    while true; do
      now="$(date +%s)"
      if wlan_has_ip && has_default_route && ping_ok; then
        log "WAN OK via $c"
        zt_start
        return 0
      fi
      if (( now - start > WAN_TIMEOUT_SEC )); then
        log "WAN FAIL via $c (timeout)"
        nmcli con down "$c" >/dev/null 2>&1 || true
        break
      fi
      sleep 2
    done
  done

  return 1
}

status_json(){
  local mode="UNKNOWN"
  # Prefer AP when hotspot is active (even if Ethernet is also up).
  if con_up "$AP_CON"; then
    mode="AP"
  else
    if [[ -n "${WAN_ETH:-}" ]] && con_up "$WAN_ETH"; then mode="WAN"; fi
    for c in "${WAN_WIFIS[@]:-}"; do
      [[ -n "$c" ]] || continue
      if con_up "$c"; then mode="WAN"; fi
    done
  fi

  local ip_wlan ip_eth gw zt
  ip_wlan="$(ip -4 -o addr show dev "$WLAN_IF" 2>/dev/null | awk '{print $4}' | head -n1 || true)"
  ip_eth="$(ip -4 -o addr show dev "$ETH_IF" 2>/dev/null | awk '{print $4}' | head -n1 || true)"
  gw="$(ip route | awk '/^default/{print $3; exit}' || true)"
  zt="stopped"; zt_running && zt="running"

  printf '{"mode":"%s","wlan_ip":"%s","eth_ip":"%s","gateway":"%s","zerotier":"%s"}' \
    "$mode" "${ip_wlan:-}" "${ip_eth:-}" "${gw:-}" "$zt"
}

case "${1:-}" in
  ap) ap_on ;;
  wan)
    if ! wan_try; then
      log "WAN failed -> fallback to AP"
      ap_on
      exit 2
    fi
    ;;
  status) status_json ;;
  *) echo "Usage: $0 {ap|wan|status}"; exit 1 ;;
esac

