#!/usr/bin/env bash
set -euo pipefail

# Installs only the public gateway. The node RPC remains loopback-only and the
# ledger directory is never modified by this script.
repo="${KEI_NODE_REPO:-/root/kei-node}"
listen="${KEI_GATEWAY_LISTEN:-0.0.0.0}"
port="${KEI_GATEWAY_PORT:-443}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup="/root/kei-rollbacks/gateway-${stamp}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "install-gateway.sh must run as root" >&2
  exit 1
fi
test -f "${repo}/ops/testnet/rpc_gateway.py"

mkdir -p "${backup}" /opt/kei-rpc-gateway /etc/kei-rpc-gateway
for existing in /opt/kei-rpc-gateway/rpc_gateway.py /etc/systemd/system/kei-rpc-gateway.service; do
  if [[ -f "${existing}" ]]; then
    cp -a "${existing}" "${backup}/"
  fi
done
install -m 0755 "${repo}/ops/testnet/rpc_gateway.py" /opt/kei-rpc-gateway/rpc_gateway.py

# Cloudflare's edge terminates the browser certificate and the zone uses Full
# mode to the origin. A host-local certificate keeps that second leg encrypted;
# the private key is generated on the host, never committed or printed.
if [[ ! -s /etc/kei-rpc-gateway/origin.key || ! -s /etc/kei-rpc-gateway/origin.crt ]]; then
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -subj "/CN=testnet.keicoin.org" \
    -addext "subjectAltName=DNS:testnet.keicoin.org" \
    -keyout /etc/kei-rpc-gateway/origin.key \
    -out /etc/kei-rpc-gateway/origin.crt >/dev/null 2>&1
fi
getent passwd www-data >/dev/null
chown root:www-data /etc/kei-rpc-gateway /etc/kei-rpc-gateway/origin.key /etc/kei-rpc-gateway/origin.crt
chmod 0750 /etc/kei-rpc-gateway
chmod 0640 /etc/kei-rpc-gateway/origin.key
chmod 0644 /etc/kei-rpc-gateway/origin.crt

cat > /etc/systemd/system/kei-rpc-gateway.service <<UNIT
[Unit]
Description=Kei public testnet RPC gateway
After=network-online.target kei-node.service
Requires=kei-node.service

[Service]
ExecStart=/usr/bin/python3 /opt/kei-rpc-gateway/rpc_gateway.py --listen ${listen} --port ${port} --tls-cert /etc/kei-rpc-gateway/origin.crt --tls-key /etc/kei-rpc-gateway/origin.key
Restart=on-failure
RestartSec=2
User=www-data
Group=www-data
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable kei-rpc-gateway
systemctl restart kei-rpc-gateway
ready=0
for _ in $(seq 1 25); do
  if curl -kfsS --max-time 5 "https://127.0.0.1:${port}/healthz"; then
    ready=1
    break
  fi
  sleep 0.2
done
if [[ "${ready}" != 1 ]]; then
  systemctl status kei-rpc-gateway --no-pager -n 30 >&2 || true
  exit 1
fi
echo
echo "gateway installed; rollback copies (if any): ${backup}"
