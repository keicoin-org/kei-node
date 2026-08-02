# M4 decisions — the node side

M2 built the four asset operations and the tables under them. M3 put the SDK on
a real node over RPC. M4 is the mechanism the whole design rests on: **one
issuer block underwriting an unbounded number of player claims** (SPEC §5.5).

This document covers the node half of M4 — `commit`, `commit_close`, `claim`,
the tables they need, and the hashing rule a proof is checked against. The work
server, the SDK surface, and the game-side deliverables in SPEC's M4 row are
elsewhere.

As with [`decisions-m2.md`](decisions-m2.md), nothing here overrides SPEC.md.
Where a decision is provisional it says so.

---

## 0. What this is, and what it is not

SPEC §13 defines M4 as:

> Supply-1 native tokens end to end. `commit`/`claim` working: one issuer root,
> many parallel player claims. Work server running. The NPC shop sells a real
> on-chain item; mobs drop claimable loot.

The node part of that is **three operations and the state they need to be safe
against replay.** Supply-1 tokens need nothing new — a `max_supply` of 1 and a
`transfer` policy were already M2's — which is why this document is about claims
alone.

| Deliberately not here | Where it lands | Why not now |
|---|---|---|
| Pruning closed roots and their claims | Alongside general pruning | §5.5 says a closed root *becomes* prunable, not that M4 prunes it. The `closed` flag is what a pruner will key on, and it exists now so that pruning later needs no new block type. |
| The SDK's tree builder and auto-claim | kei-transaction | The node verifies proofs; it never builds a tree. The two are separable, and the node is the half that cannot be changed later. |
| Partial claims from a pool (§5.5) | Unscheduled | A leaf commits to an exact amount. A pool from which claimants draw portions is a different mechanism, and no game in this repo needs it yet. |
| An unchecked queue for early claims | §5 below | It needs a dependency hash a claim does not carry. |

## 1. The reserved numbers are spent exactly as reserved

[`decisions-m2.md §18`](decisions-m2.md) reserved 5, 6 and 7 for `commit`,
`commit_close` and `claim`, in that order, because the op byte is hashed and
renumbering later invalidates every vector and signature made against the
earlier guess. M4 spends them exactly as reserved, and `asset_op_valid` moves
its bound from `asset_receive` to `claim`. 8 upward stays reserved for the
§5.6.4 swap legs.

Nothing about M2's five ops changes. A block written by an M2 node parses,
hashes, and validates identically.

## 2. The rooted ops need no new layout

A `commit` names a root. A `claim` names the root it proves against. A
`commit_close` names the root it closes. All three are 32 bytes, and the fixed
header already carries a 32-byte field whose meaning is op-dependent: `link`,
which is a counterparty account for `mint` and `transfer` and a source block
hash for `asset_receive` (decisions-m2.md §7, §10).

**The root travels in `link`.** No new header field, no new block type, and the
same parse path for all eight ops.

What each op puts where:

| Op | `asset_id` | `amount` | `link` | payload |
|---|---|---|---|---|
| `commit` | the asset the drop pays | `total` the issuer declares | the root | `count` |
| `commit_close` | zero | zero | the root | empty |
| `claim` | the asset being claimed | the amount claimed | the root | the proof |

`commit_close` requires `asset_id` and `amount` to be zero rather than ignoring
them. The root is the whole statement; a close that also carried an asset id
would be making a second claim about the drop that nothing enforces, and the
next reader would have to guess whether it meant anything.

`count` and `total` are the issuer's own description of the drop. The node
cannot check either — it verifies one claimant's leaf and never learns what the
other leaves say — but they are signed, so an issuer cannot restate the size of
a drop after the fact, and a wallet showing "you claimed 1 of 5,000" has a source
for the 5,000. A `commit` with `count == 0` or `total == 0` is rejected: a drop
that describes nothing is a mistake, not a degenerate case worth supporting.

## 3. The leaf, the tree, and what a proof does not prove

A leaf is `blake2b-256(0x00 ‖ account ‖ asset_id ‖ amount)`.

**The leaf binds the asset id, not just the amount.** Without it, a proof cut
from a drop of one asset could be replayed against a root of another — and roots
are global, so "another asset" includes another issuer's.

Interior nodes are `blake2b-256(0x01 ‖ min(a,b) ‖ max(a,b))`. Two
consequences, both deliberate:

- **Pairs are ordered by value, so a proof is siblings alone.** There are no
  direction bits to encode, to disagree about, or to forge, and the wire format
  is a length byte followed by that many 32-byte siblings.
- **The two domain separators are what make that safe.** Ordering by value is
  only sound if a leaf hash can never be read as an interior hash. Without
  separate domains, a 64-byte "leaf" could be presented as an interior node and
  prove a membership the issuer never committed to. `a_leaf_cannot_be_passed_off_as_an_interior_node`
  is the test that pins this.

**The tag is a single byte, not a hashed label, and that is not a detail this
side gets to choose.** The node verifies proofs the SDK's tree builder
produces (`@keicoin/claims`, built on `packages/core/src/merkle.ts` in
kei-transaction); it is the frozen contract, and `0x00`/`0x01` is its exact
encoding. A node that hashes a longer or different separator computes a
different leaf and a different root from the same inputs — every proof from
that SDK then fails `bad_claim_proof`, silently, because nothing about the
mismatch is visible from either side alone. `conformance/` vectors generated
straight from that SDK module are what catch this; do not re-derive the tag
bytes from this document, read them from the SDK source.

The tree's *shape* is the issuer's business. The node folds whatever siblings it
is given and never learns how many leaves there were, how they were ordered, or
how an odd node was promoted. That is what lets an SDK change its builder without
a protocol change.

**No salt.** An earlier draft had one, to stop an attacker computing a root the
issuer was about to publish and publishing it first. That attack does not exist
here: a `commit` is only valid from the asset's issuer, so nobody else can
publish any root for that asset, whatever they know about its leaves. The
uniqueness check on roots then catches only the case it should — an issuer
republishing a batch it already published, which would otherwise reopen a root
it had closed.

A valid proof proves one thing: *this account is owed this amount of this asset
under this root.* It does not prove the issuer had supply for it (§6), that the
drop was fair, or that anyone else was treated the same way.

## 4. Three tables, and why the claim index is written twice

`asset_commits` maps a root to its record. It is the one asset table not keyed
by an account, and it has to be: a claim block names a root and nothing else
about the drop.

`asset_claims` is keyed `(account, root)`, which SPEC §5.5 settles and gives the
reason for — keyed by account the record is partitioned with the account that
made it and prunes alongside that account's chain, where a root-keyed set would
be global, grow forever, and belong to nobody.

`asset_claim_roots` is the same pair the other way round, and it is **not** a
second opinion about the same question. SPEC §5.5 settles how a claim is looked
up during validation; it does not discuss what happens when the commit block
underneath one loses a fork. Rolling that block back means undoing every claim
written against it — on chains this node cannot enumerate from the commit alone,
because a claim leaves no per-recipient key the way a receivable does. This is
the same problem `pending` solves for a send whose receive already exists
(decisions-m2.md §10), solved the same way: an index in the direction the
rollback needs to walk. Nothing reads it during validation, both orderings are
written by one call so they cannot disagree, and it prunes with the root.

The three tables are created empty on the next open of an existing database.
There is no version bump and no upgrade step, because there is no old data to
reinterpret — only tables that were never written before.

## 5. A claim that arrives before its root is rejected, not held

A claim depends on its commit block, but it names the *root*, not that block's
hash — so there is nothing for `dependent_blocks` to return and nothing to key
an unchecked entry by. A claim whose root this node has not seen yet gets
`no_such_commit` and is dropped.

This is the same trade M2 made for an `asset_receive` that arrives before its
source (`unreceivable`), and the same mitigation applies: the block comes back
on rebroadcast, and an SDK that publishes a root and then claims against it is
publishing to the same network it is about to claim from.

**Stated as a known cost rather than a solved problem.** The fix, if the testnet
shows claims being dropped in practice, is for a claim to carry the commit
block's hash alongside the root so it can be queued like a receive. That is a
payload change and therefore a hash change, so it belongs in a milestone that
can afford one — not in a patch.

## 6. The cap binds at the claim, and the issuer can over-commit

Committing to a drop creates nothing. Units come into existence when someone
claims them, so `max_supply` is enforced at the claim.

An issuer can therefore publish a root committing to more than the cap allows,
and the node cannot tell: it never sees the leaves. What happens instead is that
claims succeed until the cap is reached and then fail with `over_max_supply` —
first come, first served, on a chain with no global ordering to make "first"
mean anything fair.

This is the issuer's mistake surfacing at the only place the node can see it.
The SDK should check a batch against remaining headroom before publishing its
root, and that check is advice, not enforcement — as it must be, since the node
cannot verify it either way.

**The check subtracts, and it runs on uncapped assets too.** A leaf's amount is
whatever the issuer wrote into the tree, and the same goes for a mint's, so
`circulating + amount` is an attacker-chosen uint128 sum that can carry past
2^128 — comparing that sum against the cap would let it wrap, compare small,
and be credited as the remainder. Both the mint and the claim therefore ask
`amount > max_supply - circulating` instead, behind `amount > 2^128 - 1 -
circulating`. That ceiling test runs whether or not the asset is capped: an
uncapped asset has no cap to compare against but still has the arithmetic
ceiling, and that is exactly the case a capped-only check skips.

## 7. `claim` is a tier-C operation

Work tiers (decisions-m2.md §11) put `issue` and `mint` at A, `transfer` at B,
and the cheap ops at C. `commit` and `commit_close` are issuer writes and go at
A. **`claim` goes at C**, with `burn` and `asset_receive`.

The reasoning is SPEC §5.5's own: the mechanism exists so that a thousand players
can claim at once without the issuer's chain being a bottleneck, and a claim that
costs tier-A work on a phone mid-game reintroduces the pause the design set out
to remove. A claim can only materialise an entitlement an issuer already
committed to and signed for, so the spam it enables is bounded by what an issuer
already paid tier-A work to publish.

## 8. What is not finished, stated plainly

1. **Pruning is not implemented.** `closed` exists and is what a pruner will key
   on. Nothing deletes a closed root today.
2. **`stat::detail` is at 225 of the 256 entries magic_enum is configured for.**
   M4 spent five. M5's swap legs will want more, and the failure mode when the
   range is exceeded is silent — `magic_enum::enum_name` returns an empty string
   rather than failing to compile. Worth raising `MAGIC_ENUM_RANGE_MAX` before
   it bites, not after.
3. **A claim's JSON `proof` is an array, and an empty one stays `[]`** via the
   §15 response encoder. A one-leaf drop is the only way to produce one.
4. **The RPC surface is two reads** — `commit_info` and `claim_status`, the
   exact names and shapes `kei-transaction/docs/rpc.md` (§ Claims) specifies —
   and no writes. Blocks are published through `process` as every other op is.
   `asset_commit` and `asset_claims` answer alongside them as deprecated
   aliases: the branch that first shipped this surface used those names, and
   `asset_claims` served a *list* of an account's claims rather than the single
   `(account, root)` boolean the frozen contract calls `claim_status`. Nothing
   in the SDK or a merged node ever depended on that list shape, so the alias
   points at `claim_status` rather than preserving it. The public gateway
   allowlists only `commit_info` and `claim_status`; the old names are for a
   node accessed directly during the transition.

## Definition of done for the node side of M4

1. `commit` from the asset's issuer publishes a root; from anyone else it is
   `not_issuer`.
2. A valid proof against an open root materialises exactly the leaf's amount in
   the claimant's account, on a block the claimant signed.
3. The same claim twice is `already_claimed`; a proof for another account, another
   amount, or no proof at all is `bad_claim_proof`.
4. `commit_close` from the issuer makes further claims `commit_closed`, and what
   was claimed before it stays claimed.
5. Rolling back a claim returns the units, the supply, and the entitlement.
   Rolling back a commit takes every claim against it with it, including claims
   that are no longer their account's frontier.
6. An issuer's chain is one block longer after underwriting a drop of any size.
