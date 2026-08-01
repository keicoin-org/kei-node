# Conformance — definition-of-done (6)

`docs/decisions-m2.md` ends with six acceptance criteria and says the sixth is
the one that matters: the SDK's own conformance suite passing against this node
with only the URL changed. Until now nothing here could run it, because nothing
here ran.

[`against-node.test.ts`](against-node.test.ts) is that suite pointed at a live
`bananode`. It is not a second opinion about the contract — it makes the same
client calls, in the same order, as `kei-transaction`'s
`packages/core/test/mock-server.test.ts` and `packages/kei/test/over-http.test.ts`.
Where a case there reaches into `MockNode` rather than crossing a wire, the file
says so at the top and states what it asks instead.

## Running it

The node must be built and running on a dev network with RPC enabled. The suite
resolves `@keicoin/core` and `kei-transaction` from the SDK workspace, so it runs
from inside a checkout of it:

```sh
git clone https://github.com/keicoin-org/kei-transaction.git
cd kei-transaction && bun install
cp ../kei-node/conformance/against-node.test.ts packages/kei/test/
KEI_NODE_URL=http://127.0.0.1:45000 bun test packages/kei/test/against-node.test.ts
```

`KEI_NODE_URL` defaults to `http://127.0.0.1:45000`.

## Where it stands

Eleven cases, **seven passing**. The four that fail are not a list of bugs to
work through in order — two of them are M4 arriving early, and each of the other
two is a decision rather than a defect.

| Failing case | Why |
|---|---|
| unknown account/asset/root/block are null | `commit_info` and `claim_status` are unimplemented. Both belong to `commit`/`claim`, which the README schedules for **M4**. |
| a batch of drops | The same thing from the other side: a `commit` block has no §7 wire layout, so the SDK signs it under a local hash domain and the node refuses it. **M4.** |
| the full loop | `kei.pay({ memo })` builds a `state` block with a `memo` field. §8 put memos on the *asset* block and the §14 layout has no room for this one, so the SDK hashes it locally on purpose and the node rejects it. Neither side is wrong; **the shape of a Kei payment carrying a memo was never settled.** |
| history comes back as blocks | The oldest block on the genesis chain is a legacy `open` block — `type: "open"`, with a `source` — not the state block the contract expects, so it has no `subtype`. §5 chose that genesis shape deliberately, and changing it moves the genesis hash, so it belongs to the SPEC §5.7 ceremony rather than to a patch. |

That the other seven pass is the part worth stating plainly: `faucet`,
`accounts_receivable`, `account_info`, `block_info`, `work_thresholds`,
`account_history` in both shapes, the receivable-polling subscription, and
`process` — including an `asset` block carrying a real issuance — all answer a
real client over real HTTP against a real ledger.
