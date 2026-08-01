# M4 native items and rooted claims

M4 keeps the M2 asset block and activates the opcodes M2 reserved:

| byte | operation | flat fields | payload |
|---:|---|---|---|
| 5 | `commit` | `asset_id=asset`, `amount=total`, `link=root` | `count` as uint32 big-endian |
| 6 | `commit_close` | `asset_id=0`, `amount=0`, `link=root` | empty |
| 7 | `claim` | `asset_id=asset`, `amount=amount`, `link=root` | uint8 proof length, then 32-byte siblings |

The outer `payload_len` remains uint16 little-endian. Proofs are capped at 48
siblings. The SDK and node hash these exact bytes; opcodes 8 and above remain
reserved for swaps.

## Ledger indexes

`commits[root]` stores issuer, asset, recipient count, declared total, publishing
block, and closed state. Double-claim prevention is the required primary index
`claims[(account, root)]`. A secondary `claims_by_root[(root, account)]` stores
the same claim block hash solely so rollback of an issuer commit can find and
roll back claims on independent account chains. Both indexes move atomically.

Claims verify the §5.5 domain-separated sorted-pair Merkle proof, directly
credit the claimant's holding, increment circulating supply, enforce max-supply
headroom and the 1,024-holding cap, and never write another account's chain.
Only the issuer can publish or close a root. A closed root rejects later claims.

## Items and work

Items need no separate consensus primitive: they are native assets with
`decimals=0`, `maxSupply=1`, and issuance metadata `kind=item`. The M2 holdings
and holders indexes therefore answer ownership and inventory without a registry.

`commit` and `commit_close` use work tier A; `claim` uses tier C. The companion
SDK package runs a `work_generate` HTTP service so a game can precompute claims
off the rendering thread.
