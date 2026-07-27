# QUARCS Raspberry Pi Deployment

This directory now uses the AP+STA network stack:

- `eth0`: wired LAN, kept as the preferred management path.
- `wlan0`: STA uplink managed by `wpa_supplicant@wlan0` and `wlan0-dhcp.service`.
- `uap0`: hotspot interface managed by `hostapd@uap0`, `dnsmasq`, and NAT.

The old NetworkManager AP/WAN switching flow has been removed from this deploy
directory because it conflicts with the current `wlan0/uap0` unmanaged setup.

## Network Setup

Install or refresh the Raspberry Pi AP+STA stack:

```bash
sudo ./apsta-setup.sh \
  --wan-ssid "QHYCCD503" \
  --wan-psk "QHYCCD503" \
  --ap-ssid "LQ" \
  --ap-open
```

The script:

- checks and installs required packages only when missing;
- probes Debian mirrors before installing dependencies;
- leaves the wired `eth0` route preferred;
- creates/refreshes `uap0`, hostapd, dnsmasq, NAT, and dual-LAN source policy routing;
- installs `/usr/local/bin/apsta-wifi` for later maintenance.

Useful checks:

```bash
apsta-wifi status
ip -4 -br addr show
ip route
ip rule show
systemctl status wpa_supplicant@wlan0 wlan0-dhcp uap0-create hostapd@uap0 dnsmasq ap-sta-nat quarcs-dual-lan-policy.timer --no-pager
```

## Application Deployment

`deploy_build_to_pi.sh` is still used for copying built QUARCS artifacts to the
Pi. Override the host when needed:

```bash
PI_HOST=192.168.1.104 ./deploy_build_to_pi.sh
```

`systemd/quarcs-qt-server.service` remains the Qt service template.
