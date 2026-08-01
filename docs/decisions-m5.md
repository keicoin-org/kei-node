# M5 decisions — the node side

M2 built the five original asset operations. M4 added rooted claims. M5 adds
the third and last of SPEC §5.3's block-level primitives: **the swap pair**
(SPEC §9.2) — `swap_offer`, `swap_accept`, `swap_cancel` — plus the read index
that makes those blocks a market (SPEC §9.3).

This document covers the node half of M5. `@keicoin/market`, the work server's
tiering, and the game-side deliverables in SPEC's M5 row are elsewhere.

As with [`decisions-m2.md`](decisions-m2.md) and
[`decisions-m4.md`](decisions-m4.md), nothing here overrides SPEC.md. Where a
decision is provisional it says so.

---

## 0. What this is, and what it is not

SPEC §13 defines M5 as:

> `swap_offer`/`swap_accept`/`swap_cancel` in the node, with self-locking and
> the accept-vs-cancel conflict rule. `@keicoin/market`: offers (which are
> `swap_offer` blocks), settlement, price history read from the chain with no
> server.

The node's share of that is three block types, the one new piece of consensus
state they need (a lock), and a read index over it. Everything else in that
paragraph — the SDK package, price history queries, a UI — is a client
reading blocks this node already stores.

| Deliberately not here | Where it lands | Why not now |
|---|---|---|
| Price history (`medianPrice`) | `@keicoin/market` | It is a client-side aggregation over ordinary account history (SPEC §9.1); the node adds no new capability for it. |
| Advisory `expires_at` enforcement | Nowhere, ever | SPEC §9.3 is explicit that this is client-side only. A block-lattice has no clock (SPEC §5.5), and this project does not add one for a listing deadline. |
| Cross-chain election prioritisation | Unscheduled — see §9 below | Real consensus work, separately budgeted, not a ledger-validation change. Read §9 before assuming this is finished. |

## 1. The reserved numbers are spent exactly as reserved

[`decisions-m2.md §18`](decisions-m2.md) reserved 8 upward for the swap legs.
M5 spends 8, 9 and 10 on `swap_offer`, `swap_accept` and `swap_cancel`, in
SPEC §5.6.4's own listed order, and `asset_op_valid` moves its bound from
`claim` to `swap_cancel`.

Nothing about M2's five ops or M4's three changes. A block written by an M4
node parses, hashes, and validates identically.

## 2. The offer needs one new field, not a new layout

SPEC §9.2: `swap_offer` "declares `want_asset`, `want_amount`, and an
optional specific counterparty." Three things to place, and the fixed header
already has homes for two of them:

- **The offered side is the block's own `asset_id` and `amount`** — the same
  fields a `transfer` uses, because an offer is what debits this account, same
  as a transfer is.
- **The optional counterparty is `link`** — an account, exactly as `mint` and
  `transfer` already use it. Absent (zero) means open to anyone, which SPEC
  §9.3 is explicit is a different statement than naming the zero account.
- **`want_asset`, `want_amount` and the advisory `expires_at` are new payload
  fields**, because nothing in the fixed header has anywhere else to put them.

`swap_accept` and `swap_cancel` need no new fields at all: `link` carries the
offer's hash (the same "32-byte field whose meaning is op-dependent" pattern
`decisions-m4.md §2` used for the rooted ops), and `swap_accept`'s own
`asset_id`/`amount` restate what the accepter pays (§4 below). Both payloads
serialise to zero bytes on the wire, exactly like `burn` and `asset_receive`.

## 3. Locking Kei is the one thing an asset block had never done

Every asset op before M5 left the account's Kei balance untouched — enforced
by `an_asset_block_may_not_move_kei` since M2. A swap can lock or pay in Kei
itself (`asset_id` or `want_asset` of zero), and when it does, the movement is
the fixed header's own `balance` field, exactly as a `send` would be. The
general "balance unchanged" rule is bypassed for all three swap ops and
replaced with a rule each op checks for itself, once it knows which side (if
either) is Kei.

The same fork shows up in what a `swap_accept` creates. It settles both legs
in one block, and either leg can be Kei:

- **A Kei-denominated leg arrives in the inherited `pending` table**, the same
  table an ordinary `send` uses, so the recipient collects it with an ordinary
  `state`-block receive — the first time an asset-typed block has ever put an
  entry there.
- **An asset-denominated leg arrives in `asset_pending`**, exactly as a `mint`
  or `transfer` always has.

Told apart by the zero asset id, not by a flag: a receivable is a receivable,
and which table it lives in is a property of what it is, not of which op
created it.

## 4. The accept restates the offer's own terms

`swap_accept`'s `asset_id` and `amount` are not new information — they must
equal the lock's `want_asset` and `want_amount`, checked and rejected as
`swap_terms_mismatch` otherwise. This looks redundant until the alternative is
stated plainly: without it, the accepter signs a block that trusts whatever a
hash currently resolves to, with the actual cost written on a record they do
not control. Restating the terms makes the accept block a complete statement
of what the signer agreed to, on its own, the same reason a `transfer` states
its own amount rather than trusting the sender's balance field.

## 5. A swap leg answers to the same transfer policy as a `transfer`

SPEC §5.4's policy is immutable and protocol-enforced; a swap is a transfer
with a second leg attached, and does not get a different answer. Both
`swap_offer` and the paying side of `swap_accept` call the same
`swap_leg_permitted` a `transfer` would, and it treats a swap's optional
open counterparty (`from`/`to` = the zero account) the same way an `open`
policy always has: `issuer_only` passes only when the *known* side is the
issuer, so an `issuer_only` asset cannot be listed for anyone, only sold
directly to or bought directly from the issuer. This is enforced once, at
offer time, against whichever counterparty the offer actually names (or, for
an open offer, against the offerer's own side) — the accept side re-checks
only the leg it is itself responsible for (what the accepter pays), because
transfer policy is immutable and the locked side's policy cannot have changed
between the offer and the accept.

**An offer cannot name its own author as the counterparty.** It could never be
accepted — `swap_accept` refuses a self-accept independently, as
`swap_not_counterparty` — so a self-named offer is refused at the offer
instead of locking an asset forever for nothing. An *open* offer (no named
counterparty) can still be self-accepted in principle; that is the case
`swap_not_counterparty` exists to catch at accept time, as `self_swap` only
covers the named case.

## 6. Two tables, and the second is the whole market

- **`swap_locks`**, keyed by the `swap_offer` block's own hash, holding an
  `asset_lock_info`. This is the only table validation consults for a swap: an
  accept or a cancel looks up the lock by the offer hash in `link`, and
  whichever of the two the ledger applies first is the one that gets to.
- **`swap_offers`**, keyed `(offered asset, offer hash)`, holding the
  offerer's account. This is SPEC §9.3's read model in full: "a scan of
  `swap_offer` blocks that are neither accepted nor cancelled" is exactly a
  prefix scan of this table by asset, because an entry is written once, at the
  offer, and deleted once, the moment its lock is consumed — by an accept or a
  cancel alike. Nothing during validation ever reads it; it exists only to be
  scanned, the same reason `asset_claim_roots` (`decisions-m4.md §4`) exists
  only to be walked backwards on rollback.

## 7. An accept settles the lock; a cancel deletes it — and rollback is exactly the asymmetry

A cancel's lock record is gone the moment it applies, because nothing else
ever needs it again, and the block that would have to be undone to bring it
back — the cancel itself — sits later on the *offerer's own chain*, which
ordinary tip-first rollback reaches on its own.

An accept cannot delete it. The record sits on the *accepter's* chain, a
different chain than the offer's, and nothing orders one against the other.
So an accept keeps the lock and marks it `settled_by` the accepting block's
hash — the same reason `asset_claim_roots` keeps a claim rather than deleting
it (`decisions-m4.md §4`) — and rolling back the *offer* underneath an already
-accepted lock has to roll back the accepter's chain first, exactly as
rolling back a `commit` has to undo every claim against it first
(`decisions-m4.md`, `rolling_back_a_commit_takes_its_claims_with_it`). Both
rollbacks loop rather than call once, for the same reason
`take_back_receivable` does: the dependent chain may have grown further blocks
since, and each pass only removes its current tip.

## 8. The RPC surface

Four read-only actions. Submitting the three new block types needs no new
action at all — it goes through the same generic block-JSON path
`asset_commit`/`claim` already use (`asset_hashables::
deserialize_op_json`/`serialize_op_json`, extended for the three new ops in
`nano/lib/blocks.cpp`), exactly as SPEC §9.2 implies by never describing a
separate write path for a swap leg.

**`asset_offer` / `asset_offers`**, following `asset_commit`'s and
`asset_holders`' conventions exactly, are this node's own market-scan surface,
built directly on the two tables in §6:

- **`asset_offers`** — `{ "asset": "<hex>", "count": N }` → a page of
  `{ "offer", "offerer" }`, a prefix scan of `swap_offers`. This *is* the
  market's listing view (SPEC §9.3): everything it returns is open, and
  nothing it does not return is.
- **`asset_offer`** — `{ "offer": "<hash>" }` → the full `asset_lock_info`
  (offerer, asset, amount, wanted asset and amount, optional counterparty,
  advisory `expiresAt`, and whether it is still open — with `settledBy` when
  it is not). `null` for a hash that was never a `swap_offer`, the same
  "absent, not an error" rule `asset_commit` follows for an unpublished root.

**`swap_info` / `account_swaps`** are a second, distinct surface added
alongside the first, matching `@keicoin/core`'s actual wire contract
(`packages/core/src/node.ts`'s `SwapOffer`, called via `HttpNode.swapOffer` /
`HttpNode.accountSwaps` and mocked in `packages/core/src/mock/server.ts`) —
not a guess at what the SDK might want, but its existing source. They answer
a question `asset_offer`/`asset_offers` structurally cannot: **what happened
to an offer that is no longer open**, including one that was *cancelled*,
which §7 establishes leaves no trace in `swap_locks` at all.

- **`swap_info`** — `{ "hash": "<offer hash>" }` → one full `SwapOffer`
  (`hash`, `from`, `asset`, `amount`, `wantAsset`, `wantAmount`,
  `counterparty`, `expiresAt`, `height`, `seenAt`, `state`, `acceptedBy`,
  `settledBy`, `settledAt`), or `{ "offer": null }` if the hash never named a
  `swap_offer`.
- **`account_swaps`** — `{ "account": "<addr>", "count": N, "state"?:
  "open"|"accepted"|"cancelled" }` → every `swap_offer` that account has ever
  made, in any state, found by walking that account's own chain tip-first —
  a bounded, per-account scan by construction (SPEC §9.1), and the only place
  a *cancelled* offer's outcome can be answered from at all (§7): the record
  in `swap_locks` is gone, so the state and the settling block are recovered
  by walking forward from the offer, on the offerer's own chain, to the
  `swap_cancel` that references it.

Both surfaces read real, disjoint value: `asset_offers` answers "what can I
buy right now" for an asset in one scan with no per-account cost; `swap_info`
/ `account_swaps` answer "what happened to this offer, or to this account's
offers" including settled and cancelled history. Neither is a mock of the
other, and a future pass could fold the first into the second if the
market-scan use case turns out not to need its own shape — nothing in this
milestone required deciding that yet.

`kei-transaction/docs/rpc.md` does not describe either surface yet, which as
of this writing states plainly that "`swap_offer`/`swap_accept`/`swap_cancel`
and the market read model are M5" — future work for that repository, not a
contradiction of it.

## 9. What is not finished: cross-chain conflict prioritisation

Read this section before assuming M5's "accept-vs-cancel conflict rule" is
complete, because the ledger-level work and the network-level work are two
different claims and only the first is done here.

**What is done, and is real consensus code:** `swap_accept` and `swap_cancel`
both consume the same lock record, and the ledger enforces mutual exclusion
exactly — whichever of the two a node validates and writes first succeeds, and
the second deterministically fails as `offer_consumed`, never partially
applies, and rolls back correctly (§7) if the winner is later undone. Every
node that runs this validation code reaches the same answer given the same
processing order. This is not an indexer's guess; it is the block-processing
rule every node in the network runs.

**What is not done:** SPEC §9.2 point 4 asks for more than that — "the node's
conflict detection must key on the consumed lock rather than on `previous`,"
so that when two nodes see a competing `swap_accept` and `swap_cancel` in
different orders, representatives vote and converge on one, "using the
existing fork-resolution path." That path — `nano::active_transactions` and
`nano::election` — keys everything by `qualified_root`, which for every
existing block type is a function of `previous` (or the account, for an open
block). A `swap_accept` and a `swap_cancel` referencing the same offer sit on
two different accounts' chains with two different, unrelated roots: today,
neither block is ever placed in the same election as the other, so nothing
in the existing fork-resolution path currently treats them as competing at
all. Each is independently valid at the moment its own account signs it, and
if two nodes apply them in different orders, both sides locally confirm a
block the other has already rejected, with no vote to reconcile the two —
a real, live network split until one side's chain is manually or
gossip-forced to re-converge.

This is exactly the gap SPEC §9.2 itself calls "the sharp edge of the
design," and closing it means teaching the election system a second kind of
"root" — a resource key, not a chain position — which touches
`active_transactions`, `election`, and plausibly the vote generator. That is
consensus-system surgery on code every other block type also depends on, it
needs a live multi-node network to prove converges rather than a unit test,
and it was judged too large and too risky to attempt inside this change
without that means of verification. It is flagged here, deliberately, rather
than silently shipped as if it were finished: **do not deploy M5 swaps to a
network where an unresolved accept-vs-cancel race has real value on either
side of it until this is closed.** Budget it as its own item, the size SPEC
§5.7 asks the reserve-governance voting work to be budgeted at.
