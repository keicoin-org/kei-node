# M2 and M4 conformance

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
Their SDK coverage is these exact files, run the same way by
[`run-m4.sh`](run-m4.sh):

- `packages/core/test/m4-node.test.ts`
- `packages/kei/test/m4-over-http.test.ts`

The pinned SDK revision in `.github/workflows/build.yml` carries both the M4
tests and the native commit/claim wire layout they exercise. Earlier SDK
revisions already contained deferred M4 test files, but still encoded only the
M2 operations; pinning one of those revisions makes every live M4 block fail as
invalid. Before this, the build workflow ran only `run-m2.sh`: a node could pass
its gate while `commit_info` and `claim_status` answered under the wrong names
and in the wrong shapes, because nothing in CI ever asked them a question
through the SDK. `docs/decisions-m4.md` §8.4 has the history of what those wrong
names were (`asset_commit`, `asset_claims`) and why they still answer, as
deprecated aliases, alongside the frozen ones.

## M4 claim hashing

`asset_claim_leaf` and `asset_claim_root` (nano/lib/blocks.cpp) reimplement,
rather than call, the hashing in the SDK's `packages/core/src/merkle.ts` — the
node has no JS runtime and the SDK has no C++ one, so there is no way to share
the implementation, only the vectors. That gap is exactly how this node once
shipped a domain separator (a hashed 32-byte label) that disagreed with the
SDK's (a literal `0x00`/`0x01` tag byte): every proof `@keicoin/claims`
produced failed `bad_claim_proof` against a real node, and nothing in either
repository's own test suite could have caught it — each was internally
consistent, just with the other.

[`generate-claim-vectors.ts`](generate-claim-vectors.ts) computes a fixed set
of leaves, roots, and proofs straight from the SDK and compares them against
[`claim-vectors.json`](claim-vectors.json), the same values pinned as hex
literals in `nano/core_test/asset_ledger.cpp`. The build workflow runs it
against the pinned SDK revision before compiling anything, so a hashing
change on either side of the contract that the other side missed fails fast
instead of failing silently on a real network.

```sh
KEI_SDK_DIR=/path/to/kei-transaction bun run conformance/generate-claim-vectors.ts --check
```

If the SDK's hashing changes on purpose, regenerate the fixture and update the
matching literals in `asset_ledger.cpp` in the same change:

```sh
KEI_SDK_DIR=/path/to/kei-transaction bun run conformance/generate-claim-vectors.ts > conformance/claim-vectors.json
```

## Run by hand

Start a dev-network node with RPC enabled, then point either launcher at a
checkout of the SDK:

```sh
KEI_SDK_DIR=/path/to/kei-transaction \
KEI_NODE_URL=http://127.0.0.1:45000 \
bash conformance/run-m2.sh

KEI_SDK_DIR=/path/to/kei-transaction \
KEI_NODE_URL=http://127.0.0.1:45000 \
bash conformance/run-m4.sh
```

The build workflow does the same thing from a clean temporary node database. It
checks out the SDK at an explicit commit, starts `bananode`, waits for the
`version` action to answer, runs both sets of files above, and uploads the
node log even when either suite fails.

## Dependency order

The SDK conformance change must merge first. The pinned SDK revision in
`.github/workflows/build.yml` is then advanced to that merge commit before this
node change can leave draft. M3 remains blocked until the live-node job is
green and the other M2 definition-of-done items pass.
