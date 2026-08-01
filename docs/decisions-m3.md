# M3 — the public testnet boundary

M3 swaps the SDK's default mock transport for a real node without changing the
public API. The canonical endpoint is `https://testnet.keicoin.org/rpc`.
It is one best-effort dev-network node on Hetzner, not a resilient network and
not somewhere to put value (SPEC §5.9, §13).

## The control RPC is never public

`bananode` still listens on `[::ffff:127.0.0.1]:45000` with control enabled.
That is deliberate: the inherited control surface contains wallet operations
which have no place on a public endpoint. `ops/testnet/rpc_gateway.py` is the
only public listener. It forwards the exact SDK actions, refuses every other
action, adds browser CORS, bounds bodies and concurrency, and does not log
request bodies.

Cloudflare terminates the public certificate and connects to the gateway over
TLS in Full mode. The origin key is generated on the host, mode `0640`, and is
never committed or printed. The gateway does not serve plaintext RPC.

General RPC is capped at 1,200 requests per Cloudflare edge per minute and 3,000
globally per minute, with at most 32 requests in flight. The edge ceiling is
deliberately higher than one user's allowance because Cloudflare multiplexes
many users through an origin address. The gateway also applies three faucet
bounds before a request reaches the node:

- at most 10,000 testnet Kei in one grant;
- two grants per destination account per day and twenty per client per hour;
- two hundred total grants per hour.

The limits are operational abuse controls, not consensus rules. They reset with
the gateway process. The ledger supply is still the hard bound, and only the
deterministic dev community allocation funds the faucet.

## Deployment and rollback

The testnet reuses the existing `kei-build` box. It is not deleted or recreated.
Before updating the node, copy the current binary, service/config files, git
revision, and data directory into a timestamped directory under
`/root/kei-rollbacks`. Stop the service before moving the data directory, then
start the final M2 build against a fresh directory. This is necessary because
M2's final reserve ceremony changed genesis; keeping the old directory would
publish a chain that did not satisfy the merged M2 definition of done.

Moving the old directory is reversible and preserves every old block. A
rollback consists of stopping both services, restoring the recorded binary and
data path, restoring the service/config copies, and starting `kei-node` again.

`ops/testnet/install-gateway.sh` changes no ledger data. It backs up any previous
gateway unit and program before installing, enables the service, and proves the
local health endpoint answers.

## Acceptance evidence

M3 is complete only when all of these are recorded against exact commit ids:

1. the host runs the merged M2 node revision and `version` answers locally;
2. HTTPS and browser preflight answer at the canonical endpoint;
3. an inherited control action is refused and an over-cap faucet request is
   refused without reaching the node;
4. the SDK's unchanged M2 suites and its M3 default-client test pass against the
   public endpoint;
5. a fresh wallet receives faucet Kei and sends a real transaction.
