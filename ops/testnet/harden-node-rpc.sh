#!/usr/bin/env bash
set -euo pipefail

# Turns off the inherited control RPC on a node that is about to be published.
#
# The gateway allowlists the actions in `docs/rpc.md` and forwards nothing else,
# so control is already unreachable from outside. This is the second barrier:
# `enable_control` is what `wallet_seed`, `block_create`, `send` and `stop` hang
# off (rpc_handler.cpp), and none of the SDK contract needs it — `faucet`
# included, which signs with a published dev key rather than a wallet. A public
# node should not be one misrouted request away from handing out a wallet.
#
# Non-destructive: the ledger is never touched, the previous config is copied to
# /root/kei-rollbacks first, and the change is one line to put back.
data_dir="${KEI_NODE_DATA_DIR:-/root/keidata}"
rpc_config="${data_dir}/config-rpc.toml"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup="/root/kei-rollbacks/node-rpc-${stamp}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "harden-node-rpc.sh must run as root" >&2
  exit 1
fi
test -f "${rpc_config}"

if grep -qE '^\s*enable_control\s*=\s*false' "${rpc_config}"; then
  echo "control RPC already disabled in ${rpc_config}"
  exit 0
fi

mkdir -p "${backup}"
cp -a "${rpc_config}" "${backup}/"
sed -i -E 's/^\s*enable_control\s*=\s*true\s*$/enable_control = false/' "${rpc_config}"
grep -qE '^enable_control = false' "${rpc_config}"

# The address must stay loopback whatever happens here: the gateway is the only
# thing that should ever hold a public socket.
grep -qE '^\s*address\s*=\s*"::ffff:127\.0\.0\.1"' "${rpc_config}"

systemctl restart kei-node
for _ in $(seq 1 60); do
  if curl -fsS --max-time 3 -X POST http://127.0.0.1:45000 \
    -H 'content-type: application/json' \
    -d '{"action":"work_thresholds"}' >/dev/null; then
    break
  fi
  sleep 1
done

# Prove both halves rather than asserting them: the contract still answers, and
# a control action now does not.
curl -fsS --max-time 5 -X POST http://127.0.0.1:45000 \
  -H 'content-type: application/json' \
  -d '{"action":"work_thresholds"}' >/dev/null
control="$(curl -fsS --max-time 5 -X POST http://127.0.0.1:45000 \
  -H 'content-type: application/json' \
  -d '{"action":"wallet_create"}')"
case "${control}" in
  *"control"*) ;;
  *) echo "wallet_create was not refused: ${control}" >&2; exit 1 ;;
esac

echo "control RPC disabled; rollback copy: ${backup}"
