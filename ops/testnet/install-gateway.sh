#!/usr/bin/env bash
set -euo pipefail

# Installs only the public gateway. The node RPC remains loopback-only and the
# ledger directory is never modified by this script.
repo="${KEI_NODE_REPO:-/root/kei-node}"
listen="${KEI_GATEWAY_LISTEN:-0.0.0.0}"
port="${KEI_GATEWAY_PORT:-80}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup="/root/kei-rollbacks/gateway-${stamp}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "install-gateway.sh must run as root" >&2
  exit 1
fi
test -f "${repo}/ops/testnet/rpc_gateway.py"

mkdir -p "${backup}" /opt/kei-rpc-gateway
for existing in /opt/kei-rpc-gateway/rpc_gateway.py /etc/systemd/system/kei-rpc-gateway.service; do
  if [[ -f "${existing}" ]]; then
    cp -a "${existing}" "${backup}/"
  fi
done
install -m 0755 "${repo}/ops/testnet/rpc_gateway.py" /opt/kei-rpc-gateway/rpc_gateway.py

cat > /etc/systemd/system/kei-rpc-gateway.service <<UNIT
[Unit]
Description=Kei public testnet RPC gateway
After=network-online.target kei-node.service
Requires=kei-node.service

[Service]
ExecStart=/usr/bin/python3 /opt/kei-rpc-gateway/rpc_gateway.py --listen ${listen} --port ${port}
Restart=on-failure
RestartSec=2
User=nobody
Group=nogroup
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
systemctl enable --now kei-rpc-gateway
ready=0
for _ in $(seq 1 25); do
  if curl -fsS --max-time 5 "http://127.0.0.1:${port}/healthz"; then
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
