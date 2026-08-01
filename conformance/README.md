# M2 conformance

M2 has one executable SDK contract and two transports. These exact files run
against `MockNode` when `KEI_NODE_URL` is absent and against the native node
when it is set:

- `packages/core/test/m2-node.test.ts`
- `packages/kei/test/over-http.test.ts`

The assertions live only in `kei-transaction`; this repository has no copied
test that can drift from them. [`run-m2.sh`](run-m2.sh) is only a launcher.
The shared history case exercises the faucet's state-open community account;
`rpc.account_history_block_shape` separately covers the reserve's inherited
legacy `open`, so the two wire shapes cannot accidentally be conflated.

Commit and claim are M4 by the boundary in `docs/decisions-m2.md` section 0.
Their SDK coverage remains executable against the mock in `m4-node.test.ts` and
`m4-over-http.test.ts`, but those files are not part of M2's native-node gate.

## Run by hand

Start a dev-network node with RPC enabled, then point the launcher at a checkout
of the SDK:

```sh
KEI_SDK_DIR=/path/to/kei-transaction \
KEI_NODE_URL=http://127.0.0.1:45000 \
bash conformance/run-m2.sh
```

The build workflow does the same thing from a clean temporary node database. It
checks out the SDK at an explicit commit, starts `bananode`, waits for the
`version` action to answer, runs the two files above, and uploads the node log
even when the suite fails.

## Dependency order

The SDK conformance change must merge first. The pinned SDK revision in
`.github/workflows/build.yml` is then advanced to that merge commit before this
node change can leave draft. M3 remains blocked until the live-node job is
green and the other M2 definition-of-done items pass.
