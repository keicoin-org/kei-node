# M3 — the public testnet boundary

M3 swaps the SDK's default mock transport for a real node without changing the
public API. The canonical endpoint is `https://testnet.keicoin.org/rpc`.
It is one best-effort dev-network node on Hetzner, not a resilient network and
not somewhere to put value (SPEC §5.9, §13).

## The control RPC is never public

`bananode` listens on `[::ffff:127.0.0.1]:45000` and nowhere else.
`ops/testnet/rpc_gateway.py` is the only public listener. It forwards the exact
SDK actions, refuses every other action, adds browser CORS, bounds bodies and
concurrency, and does not log request bodies.

**The node also refuses control itself.** The allowlist already made
`wallet_seed`, `block_create`, `send` and `stop` unreachable, so this is a
second and independent barrier rather than the only one — and it is the barrier
that survives a bug in the first. `enable_control` was left `true` from the
build box, where the node only ever answered an SSH tunnel; nothing in
`docs/rpc.md` needs it, `faucet` included, because that action signs with the
published dev key rather than with a wallet. `ops/testnet/harden-node-rpc.sh`
sets it `false`, restarts the node, and then proves both halves: the contract
still answers and `wallet_create` is refused. It never touches the ledger.

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

## The origin has to speak HTTP/1.1

The gateway first shipped answering HTTP/1.0, which is `http.server`'s default
and says nothing about connection reuse. Cloudflare pools its origin
connections and reused them anyway, so the edge kept writing a request into a
socket this end was closing: a short read, a JSON parse failure, a 400, and
then `SSLEOFError` writing that 400 into a connection with nobody left on it.
It cost about one request in a hundred — 6 failures in 776 during one
conformance run — which the SDK reports as `node-unreachable`.

A network that fails a hundredth of the time is worse to build on than one that
is down, because nothing retries it and the failure lands wherever the developer
happened to be. The handler now answers HTTP/1.1, reaps idle connections after
30 s, and says `Connection: close` on the two paths that do not drain the
request body rather than dropping a socket the client still counts as usable.
`test_rpc_gateway.py` asserts connection reuse and the no-desync close, so the
regression is caught without a CDN in the loop.

## The published network is `dev`, and that is a blocker for M9 rather than M3

`version` answers `"network": "dev"`. It is the only network this tree can
start: `beta_genesis_data` and `live_genesis_data` are still the all-zero
placeholder, and `ledger_constants` refuses to start on either
(`decisions-m2.md` §5). No production key was invented to make a milestone
green, and that decision stands.

State the consequence rather than letting somebody find it. Every dev key is
derived from a published phrase — `blake2b-256("kei-dev-<role>")` — so anyone
can sign as the community account that funds the faucet, and anyone can drain
or fork this chain at will. That is acceptable for M3, whose exit criterion is
a testnet the SDK can reach with a working faucet, and where SPEC §5.9 already
says Kei is a testnet with real branding and no value belongs on it.

It is not acceptable for **M9**, which opens the network to external
developers. M9 needs the beta ceremony: keys generated offline, only the public
addresses and signed ceremony blocks landing in this repository, and
`beta_genesis_data` replacing its placeholder. Budget it as M9 work with a real
key-custody step, not as a config change.

## Acceptance evidence

M3 is complete only when all of these are recorded against exact commit ids.
Each one below is a check that was run, not a claim.

1. **The host runs the merged M2 node.** `bananode` on `kei-build` reports
   `build_info 95904047`, a commit on this branch, and this branch contains no
   `.cpp`, `.hpp` or CMake change against `master` — `git diff origin/master...HEAD
   -- '*.cpp' '*.hpp' 'CMakeLists.txt'` is empty, and merged M2 (`a0a81050`) is
   its merge-base. So the running binary is the merged M2 node, and every commit
   after the build is ops or prose. `version` answers locally.
2. **HTTPS and browser preflight answer** at `https://testnet.keicoin.org/rpc`,
   with `access-control-allow-origin: *` on both the preflight and the answer —
   asserted by `m2-node.test.ts`, "a browser can reach it through preflight and
   CORS", run against the public endpoint.
3. **Control is refused twice and the faucet cap is refused before the node.**
   `wallet_create` returns HTTP 403 `That RPC action is not public` at the
   gateway, and `RPC control is disabled` at the node itself on loopback. An
   over-cap grant is refused by `test_caps_the_public_faucet_before_forwarding`,
   which also asserts nothing reached the upstream.
4. **The unchanged M2 suites pass against the public endpoint.**
   `KEI_NODE_URL=https://testnet.keicoin.org/rpc bun test
   packages/core/test/m2-node.test.ts packages/kei/test/over-http.test.ts`
   — 11 pass, 0 fail, 43 assertions, 55 s.
5. **A fresh wallet is funded and sends.** `npm run test:m3-live` — 2 pass, 0
   fail, including SPEC §6.2's no-argument `Kei.start()` run verbatim against
   this endpoint.
