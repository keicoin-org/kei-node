# M2 decisions

M0 built the SDK against an in-process mock. M1 put a game in front of it and
served that mock over HTTP, so the SDK already talks to a node across a URL. M2
is the node itself: **a Banano fork with a Kei genesis and a native token
primitive**, and it is the first milestone where consensus code is written rather
than modelled.

As with [`decisions-m0.md`](../../kei-transaction/docs/decisions-m0.md) and
[`decisions-m1.md`](../../kei-transaction/docs/decisions-m1.md), nothing here
overrides SPEC.md. Where a decision is provisional it says so.

---

## 0. What M2 is, and what it is not

SPEC §13 defines the milestone as:

> Genesis produces exactly 1T Kei with the §5.7 allocation. Node validates
> issue/mint/burn/transfer. `balanceOf` correct. Single local node.

So M2 is **four asset operations, a genesis block, and one node running on one
machine.** Everything else that a Kei node will eventually do is a later
milestone, and pulling any of it forward is the fastest way to make M2 never
finish:

| Deliberately out of M2 | Where it lands | Why not now |
|---|---|---|
| `commit` / `claim` / `commit_close` | M4 | The claim model is the largest single item after tokens, and it needs `mint` correct underneath it first. |
| `swap_offer` / `swap_accept` / `swap_cancel` | M5 | Needs asset locking, which needs holdings to be trustworthy. |
| Reserve governance: proposals, votes, quorum | Before mainnet | SPEC §5.7 is explicit: *"Genesis can be built without the voting mechanism (M2). Mainnet cannot launch without it."* |
| Public testnet, faucet on Hetzner | M3 | M2's deliverable is a node that runs locally and is correct. |

The reserve rules that are **not** deferred are the ones that must hold from the
first block: reserve accounts are enumerated in genesis, they name the null
representative, and they carry zero weight. Those are structural. Adding them
later means a chain whose history is already wrong.

## 1. The reference implementation already exists, and it is executable

This is the part that makes M2 tractable, and it should be used rather than
admired.

`MockLedger` in `@keicoin/core` enforces the §5.6 / §7 ledger rules — one chain
per account, derived asset ids, receivable arrivals, work tiers, the issuance
burn, circulating-supply caps, transfer policy, and the §5.7 genesis allocation
with reserve exclusions. It is not a stub of the API; it is a model of the
protocol, and it has 113 passing tests against it.

More usefully, M1 made it a **conformance suite** rather than a description:

| File | What it pins |
|---|---|
| [`docs/rpc.md`](../../kei-transaction/docs/rpc.md) | The wire contract, action by action |
| `packages/core/test/mock-server.test.ts` | `HttpNode` driven against `mockRpcHandler` |
| `packages/kei/test/over-http.test.ts` | The whole economy between two clients sharing only a URL |

**The definition of done for M2's RPC layer is that those two test files pass
against `kei-node` with only the URL changed.** That is a checkable finish line,
which is worth more than a prose specification of one.

## 2. Base: Banano V25.1, forked whole

`BananoCoin/banano` at `c1f8405d` (`V25.1`), cloned with full history rather than
squashed. Keeping the upstream history is what makes it possible to rebase onto a
later Banano release instead of hand-porting security fixes forever, and SPEC
§5.6.8 is explicit that the ecosystem is worth staying compatible with wherever
it costs nothing.

The anchors M2 has to touch, located and confirmed:

| What | Where |
|---|---|
| Genesis blocks (dev / beta / live / test), as JSON literals | `nano/secure/common.cpp:33-66` |
| Address prefix decoding — `ban_` today | `nano/lib/numbers.cpp:89` |
| `enum class block_type : uint8_t` — currently `invalid`…`state = 6` | `nano/lib/blocks.hpp:21` |
| `work_thresholds`, and the epoch-keyed difficulty split | `nano/lib/config.hpp:152` |
| Ledger validation | `nano/secure/ledger.cpp` |
| Store | `nano/secure/store.cpp` |

Submodules (LMDB, cryptopp, argon2, gtest, flatbuffers, miniupnp) are not yet
initialised in this checkout.

**"Fork" here means a derivative codebase, not a GitHub fork with a pull request
pending.** Kei is its own project: it diverges permanently, it never merges back,
and no change made here is ever proposed to Banano. That distinction is obvious in
prose and was not obvious in the git configuration, where the checkout had exactly
one remote — `upstream` → `BananoCoin/banano`, with a push URL — and Kei's work
sat on a branch named `m2-start` ahead of `upstream/master`. That is
indistinguishable from a contributor preparing a PR, and a single `git push` would
have aimed at Banano.

Fixed, and the shape is now the rule:

| | |
|---|---|
| `origin` | `keicoin-org/kei-node`, matching the sibling repos. The push default. |
| `upstream` | `BananoCoin/banano`, **fetch only** — push URL set to `no_push`. |
| `master` | Kei's trunk, carrying Banano's history as ancestry and tracking nothing. |

Fetching upstream stays valuable for exactly the reason §2 keeps the history —
rebasing onto a later Banano release rather than hand-porting security fixes.
Pushing to it is never correct, so the configuration now refuses.

The same confusion was inherited in the repository's furniture, and is removed
with it: eight Banano CI workflows that built `nanocurrency/nano-node` artifacts
and deployed to the `bananocoin` DockerHub namespace, and issue templates that
directed anyone filing a bug on Kei to nano's issue tracker, Discord, and forum.
They are replaced by one workflow that builds this node (§3).

## 3. Toolchain — stated plainly, because it currently blocks the build

The fork is cloned and readable, and **it cannot be compiled on this machine as
it stands.** This was first written from a quick scan; it has since been checked
by trying to compile, and both the facts and the recommendation changed.

**What is actually here.** MSVC is present — Visual Studio 2022 Build Tools,
toolset 14.44, `cl.exe` 19.44 — which the first pass missed. It is also
**unusable**, because no Windows SDK is installed: `C:\Program Files (x86)\
Windows Kits\10` contains only `UnionMetadata`, no `Include`, `Lib`, or `bin`.
`cl` starts and then dies on `fatal error C1083: Cannot open include file:
'stdio.h'`. A MinGW `g++` is present and does compile trivial C++, but a
nano-family node is not trivial C++ and upstream supports MSVC on Windows, not
MinGW. There is no `cmake`, `make`, or `ninja`; no Docker; and WSL is installed
with no distribution.

**Boost is not a dependency to install.** V25.1 vendors the Boost superproject at
`submodules/boost` and `add_subdirectory()`s each library it needs
(`CMakeLists.txt:361`, `:448`). So the expensive part this section originally
warned about — a Windows-native Boost build via vcpkg — does not exist. It costs
a large recursive submodule checkout instead, which is bandwidth rather than
hours of compiling. That removes the main argument against option (3).

**The binding constraint is not disk or time, it is that this account is not an
administrator.** Installing a Windows SDK, `wsl --install -d Ubuntu`, and Docker
Desktop all require elevation. None of the three local options can be set up
without the machine's owner doing it. Stated in order:

1. **GitHub Actions** — `.github/workflows/build.yml`, added with this change.
   Ubuntu, gcc, vendored Boost, `NANO_GUI=OFF`, dev network. Needs **no admin and
   no local install**, and it is the only option available as things stand.
   Feedback is minutes per iteration rather than seconds, which is bad for
   iterating on C++ and perfectly adequate for keeping the tree honest.
2. **WSL2 + Ubuntu** — `wsl --install -d Ubuntu` (needs admin, one reboot).
   Closest to how the node is actually deployed on Hetzner at M3, and the best
   local loop once it exists. This is the one to ask for.
3. **Windows SDK + CMake** — makes the already-installed MSVC work. Cheaper than
   it looked now that Boost is vendored, but it is still the least production-like
   path, and it is the environment upstream's Windows CI exercises least.
4. **Docker Desktop** — reproducible, but the heaviest install of the four and it
   still needs admin, so it no longer leads.

**Nothing below this line can be verified locally until (2) or (3) exists**,
which is precisely why the decisions below are written against the mock's
semantics, which *are* verified — and why (1) exists, so that "unverified" means
"not yet run on this machine" rather than "never compiled anywhere".

## 4. Kei is 10^18 raw, fixed here rather than left provisional

decisions-m0 §1 chose 18 decimals and marked it *"the genesis block's number to
fix at M2."* This is that fix: **1 Kei = 10^18 raw, and it does not move again.**

The arithmetic that forces it: Nano uses 10^30 raw per unit and Banano 10^29,
both of which fit because their supplies are small. Kei's supply is 10^12, so
10^30 raw per Kei would need 10^42 raw in total, and a 128-bit balance holds
about 3.4 × 10^38. At 18 decimals total supply is exactly **10^30 raw**, which
fits with eight orders of magnitude of headroom, and 0.001 Kei — the sub-cent
payment the entire pitch rests on — is 10^15 raw.

Consequence for the fork: Banano's `nano::Mxrb_ratio` and friends are redefined,
not reused. Every place that renders or parses an amount goes through them, so
this is one edit in `nano/lib/numbers.hpp` rather than a sweep.

## 5. Genesis: the allocation, and how it is built

Total supply is **1,000,000,000,000 Kei = 10^30 raw**, created once, in the
genesis block, and never again. SPEC §5.7's requirement that new Kei be
*structurally* impossible rather than merely unauthorised is inherited for free:
a Nano-family ledger creates no value outside genesis, because every receive must
name a matching send.

| Allocation | Kei | Raw |
|---|---:|---:|
| Reserve (cold storage) | 900,000,000,000 | 9 × 10^29 |
| ├ Developer grants | 37,000,000,000 | 3.7 × 10^28 |
| ├ Community | 28,000,000,000 | 2.8 × 10^28 |
| ├ Bug bounty | 18,000,000,000 | 1.8 × 10^28 |
| └ Core team, first tranche | 17,000,000,000 | 1.7 × 10^28 |
| **Circulating** | **100,000,000,000** | **10^29** |
| **Total** | **1,000,000,000,000** | **10^30** |

37 + 28 + 18 + 17 = 100, exactly. SPEC §5.7 calls a mismatch a launch blocker, so
the node **asserts this at startup and refuses to run** rather than logging a
warning — the mock already does exactly this, and the C++ must not be laxer than
the model.

**Structure.** Genesis is one `open` block on the reserve account holding the
entire 10^30, followed by four sends to the circulating accounts, which those
accounts open. This is how Nano and Banano already do it (`live_genesis_data` at
`nano/secure/common.cpp:51` is a single `open`), and it keeps the inherited
bootstrap and cemented-block paths correct without special-casing.

**Reserve accounts are enumerated in the genesis block** as a fixed, immutable
set (§5.7). Membership is then a cheap test the node cannot get wrong, rather
than a list maintained somewhere else — which SPEC correctly calls *"a convention
wearing a protocol's clothes."*

**The reserve seed is never a literal in this repository, and neither is the
genesis it signs.** The tree inherited Banano's genesis blocks and would have
launched on them; §14 is where that is fixed and where the placeholder that now
stands in for beta and live is described.

**The reserve seed is never a literal in this repository.** decisions-m0 §13 used
fixed public seeds (`'1'.repeat(64)` and friends) because a mock needs a funded
faucet and nothing is at stake. That property does not survive contact with a
real chain: the reserve is 90% of all Kei, its custody is multisig (§5.7), and
the key is generated offline. What ships in the source is the *address*, which is
published deliberately so the reserve can be audited.

## 6. Weight is Kei-only, and the reserve has none

SPEC §5.6.2 is unambiguous, and it is the single change most likely to be
silently lost in a merge: **representative weight is derived from the
`asset_id = 0` balance exclusively.** A token balance contributes zero weight,
always.

The attack it closes is not subtle. Weight in a Nano-family node comes from
account balances; a native token primitive lets anyone issue a token with a max
supply of 10^30 and hold all of it. If token balances counted, capturing
consensus would cost one issuance block and one mint.

Two rules, both enforced in the weight calculation itself rather than by
convention:

1. Asset balances never enter `rep_weights`. The asset tables are separate
   storage and the weight path must not learn to read them.
2. **Reserve accounts must name a null representative, and a reserve-account
   block naming a real one is invalid.** Excluding the reserve from *governance*
   weight is not enough: representative weight governs transaction consensus, so
   a reserve delegation alone hands an absolute supermajority to whoever receives
   it — no vote required. The mock enforces this today
   (`ledger.ts`, `reserve-representative`), and the fork must too.

## 7. The `asset` block — resolving decisions-m0 §2

decisions-m0 deferred the wire format explicitly: M0's hash is canonical JSON
under a `"kei-block-v0\n"` preamble, chosen to be deterministic and
version-separated, and stated plainly that *"the byte layout of `asset` blocks is
a consensus decision belonging to the node fork."* This is that decision.

**`block_type::asset = 7`.** Values 0–6 are taken (`invalid`, `not_a_block`,
`send`, `receive`, `open`, `change`, `state`), 7 is free, and every inherited
block type keeps its number and its meaning, as §5.6.8 requires.

Proposed layout — fixed header, variable payload:

| Field | Bytes | Notes |
|---|---:|---|
| `account` | 32 | Signer. |
| `previous` | 32 | Links into the account's **one** chain (§5.6.1). |
| `representative` | 32 | Carried, so an asset block never silently changes delegation. |
| `balance` | 16 | **The account's Kei balance, unchanged from its predecessor** — §5.6.1's concession to §5.6.8, so a Banano-derived explorer that ignores the asset payload still tracks Kei correctly instead of reporting a broken balance. Except on `issue`, which burns 1,000 Kei, and where this field is how the burn is expressed. |
| `op` | 1 | `issue` \| `mint` \| `burn` \| `transfer` \| `asset_receive`. |
| `asset_id` | 32 | `H(issuer_pubkey ‖ symbol)` — derived, never assigned (§5.6.1). |
| `amount` | 16 | Units, in the asset's own decimals. Zero for `issue`. |
| `link` | 32 | Counterparty account, or the source block hash for `asset_receive`. |
| `payload_len` | 2 | Little-endian. Zero for everything but `issue`. |
| `payload` | var | Issuance metadata only: name, symbol, decimals, max supply, `transfer` policy, `swap` policy, IPFS CID. |
| `signature` | 64 | |
| `work` | 8 | |

**Hashing is blake2b-256 over the fields under a distinct preamble constant**,
the same shape Nano uses for state blocks (a 32-byte preamble carrying the block
type). A separate constant is what makes it structurally impossible for an
`asset` block to collide with a `state` block, which is the same property M0's
`"kei-block-v0\n"` string was buying in the SDK.

The SDK touches hashing in exactly one function, so this lands as one change on
that side.

> **Provisional until it compiles.** This layout is derived from SPEC §5.6 and
> from the mock's field set, and it has not yet been validated against
> `nano::block` serialisation, the sideband, or the bootstrap path. Expect the
> field *order* to move for alignment reasons; the field *set* is settled.

Two amendments, made while implementing it:

**The payload is op-keyed, not issuance-only.** The table above says
`payload_len` is zero for everything but `issue`. It is not: §8 puts memos on the
asset block, and a `transfer` is where a memo earns its keep. The payload
therefore carries issuance metadata for `issue`, a memo for `mint` and
`transfer`, and nothing for `burn` and `asset_receive`.

**The payload is structured, not opaque bytes.** It is stored parsed and
serialised canonically, and that canonical encoding is what the hash covers.
Keeping a byte blob alongside a parsed copy would mean two representations that
have to agree, and `asset_info` has to read those fields anyway.

**The preamble is the Kei domain, not the block type alone.** §14 replaced the
bare `uint256(block_type::asset)` preamble with the Kei domain followed by the
block type, applied to every block type rather than this one.

## 8. Memos ride on the asset send — decisions-m0 §4, option (a)

decisions-m0 §4 left an either/or and recommended (a). **Taking (a):** memos are
carried on the `asset`-family block, and inherited `state` blocks are left
untouched.

The reason to prefer it over (b) — correlating payments by amount and timing — is
that (b) is not merely less convenient, it is racy. Two players buying the same
5-coin item in the same second are indistinguishable to the issuer, and the shop
in the Button demo already has to do order-then-arrival matching because of it
(decisions-m1 §5). A memo makes that correlation exact instead of probabilistic.

The cost is honest and bounded: a payment carrying a memo is not a `state` block,
so a Banano-derived tool that does not understand `asset` blocks will not show
it. Kei balances still reconcile, because §5.6.1 requires every asset block to
carry a readable Kei balance.

The SDK surface does not move either way — `kei.pay({ memo })` is already written
and already tested.

## 9. Two tables, both prefix-scannable — SPEC §7

| Table | Key | Answers |
|---|---|---|
| `holdings` | `(account, asset_id)` | `balance()`, and `ownedBy(account)` as a prefix scan over one account's range |
| `holders` | `(asset_id, account)` | `balanceOf(asset, account)` in one lookup, and `owner(itemId)` as a one-entry prefix scan for a supply-1 asset |

The same facts indexed both ways, which doubles the write cost of every asset
movement. SPEC §7 says to pay it deliberately, and names what it buys: acceptance
criterion §14.3 (`balanceOf` in a single call) and `items.owner()` existing at
all. Without the reverse index, finding an item's owner means scanning accounts.

Two rules that are easy to get wrong and expensive to fix later:

- **Zero-balance entries are deleted, not kept at zero**, in both tables. A
  player's state footprint shrinks when they spend. History stays on the chain;
  state reflects only what is held now.
- **A hard cap of 1,024 distinct assets per account**, with an error naming the
  fix (burn or transfer something). It cannot be weaponised — §5.6.3 means only
  the account itself can add to its own holdings — but unbounded per-account
  state in consensus code is how nodes run out of memory.

## 10. Assets arrive as receivable, and `asset_receive` collects them

SPEC §5.6.3: a `mint` or `transfer` does not write to the recipient's state. It
creates a receivable that the recipient's own signed block collects — the
inherited two-step, reused rather than rebuilt.

decisions-m0 §3 named the missing operation: SPEC §5.3's table lists the asset
operations but has none for collection, so `asset_receive` was added as the
asset-side twin of `receive`, tier C. It is the mechanism §5.6.3 describes, not a
new capability, and the node must implement it.

What this buys, for free: an attacker minting a junk token to a million addresses
pays tier-A work per block and touches no recipient's state. Unreceived junk is
the sender's storage problem, not the network's permanent per-account cost.

## 11. Work tiers

| Tier | Difficulty | Operations |
|---|---|---|
| **A** | Highest | `issue`, `mint` |
| **B** | Standard (= Banano send) | `send`, `transfer` |
| **C** | Cheap (= Banano receive) | `receive`, `asset_receive`, `burn` |

`commit`, `commit_close`, and the swap legs are in SPEC §5.6.4's table but are
not M2 operations; their tiers are already decided and land with M4 and M5.

Banano's existing send/receive split is the precedent and the mechanism —
`work_thresholds` at `nano/lib/config.hpp:152` already carries per-epoch
difficulty, and tiering by operation is an extension of a distinction the node
already makes rather than a new subsystem.

`mint` is tier A because minting is the cheapest operation to abuse and the one
whose abuse creates permanent state.

## 12. Issuance burns 1,000 Kei

SPEC §5.6.5. Work is a rate limit; this is a cost, and it is the one place Kei is
not free. An asset record is permanent global state that every node stores
forever, and one account can otherwise mint an unbounded number of distinct
symbols.

Expressed as a decrease in the `issue` block's `balance` field with no
corresponding receivable — the Kei is destroyed, not moved. The node must reject
an `issue` whose balance decrease is not exactly 1,000 Kei.

Be precise in the docs about what this does and does not contradict: **the
feeless promise is about transactions**, and it survives intact. Sending Kei,
transferring tokens, minting, and claiming are free forever.

## 13. `kei_` addresses, same encoding

SPEC §5.8: base32 public key plus checksum, unchanged from Nano and Banano, so
tooling ports with a constant change rather than a rewrite. `nano/lib/numbers.cpp:89`
is where `ban_` is decoded and where `kei_` replaces it — and note the inherited
length check, since `ban_` and `kei_` are both four characters and a 64-character
address, which is the property that keeps the change constant-sized.

## 14. The chains are separated structurally, not by convention

A fork starts out as a copy, and the copy included Banano's genesis blocks. They
had been re-prefixed to `kei_`, which made them look Kei-shaped while remaining
Banano's: **Kei's live network would have launched on Banano's genesis, under
Banano's key**, and every Kei in existence would have belonged to whoever holds
it. Nothing about that was intended and nothing about it was visible from the
address, because `kei_1bananobh5rat…` decodes to the same public key either way.

That also made the separation between the two chains weaker than it looked. Two
Nano-family chains are normally kept apart by their genesis and by the network id
in the packet header. Kei had the second but not the first, so the protection was
coming entirely from the transport layer — and *"the two networks happen not to
talk to each other"* is a much weaker statement than *"these are structurally
different chains."* The ledger could not tell a Banano state block from a Kei one,
because the bytes being hashed were identical.

Both are fixed, and the fix has two halves.

**Kei's own genesis.** Dev gets a real one, from a key derived from the published
phrase `blake2b-256("kei-dev-genesis")` so that anyone can regenerate it and
confirm the block is what it claims to be — it is a test key by construction and
holds nothing. Beta and live carry an explicit placeholder, and `ledger_constants`
refuses to construct against one, so the node cannot be started on a chain whose
supply is signed by nobody. Their real blocks come out of the SPEC §5.7 ceremony;
§5 already says the reserve seed is never a literal in this repository, and that
is exactly why they cannot be generated here.

`util/keigen.py` is the generator. It reproduces the *inherited* dev genesis
public key, signature, and address from its known private key before generating
anything, which is the only reason to trust its output.

**A Kei hash domain.** Every block type now hashes under
`blake2b-256("kei-block-v1")` followed by the block type, where the inherited
types hashed their fields directly and only `state` carried a type preamble. An
inherited block therefore does not hash to a value its own signature covers, so
it cannot be replayed onto Kei in any state of the ledger — not merely in the
states a distinct genesis makes unreachable. This is the same device
decisions-m0 §2 chose for the SDK's `"kei-block-v0"` preamble, and it is what
makes §7's separate `asset` preamble a special case of a general rule rather
than a one-off.

The cost is the one §5.6.8 warns about and it is worth naming: a Banano-derived
tool that computes block hashes itself will compute the wrong ones for Kei.
Tools that read hashes from the node are unaffected. That is a real compatibility
loss, taken deliberately, because the alternative is two chains that differ only
by agreement.

## 15. The response encoder, and why one action serves two shapes

§16 used to carry the untyped JSON as the last thing standing between the asset
RPC and the conformance suite. Fixing it turned up a second gap that was quieter
and worse, and both are recorded here because the reasoning generalises past
either one.

**The type has to ride on the key.** `boost::property_tree` stores every value as
a `std::string`, so its writer has nothing to go on and quotes all of them.
`docs/rpc.md` wants `decimals`, `height`, and `receivableCount` as numbers, an
absent record as `null`, and an empty result as `[]`; `HttpNode` parses the body
and passes it through without coercing, so every one of those arrived wrong.

The obvious fix — a marker byte on the *value*, stripped by the writer — is
unsafe here, and the reason is worth stating because it is not obvious. Values
include an asset's `name` and a transfer's `memo`, which are chosen by whoever
signed the block and are length-capped but not otherwise constrained (§7). An
issuer could therefore name a token so that the node emitted it as raw JSON.
Keys carry no such risk: every key in a response is a literal in this source.
So `nano::json` marks the key, and `nano/lib/json_response.hpp` encodes the
response itself.

A tree with no marked keys encodes exactly as boost encoded it, which a test
asserts against `write_json` directly, so no inherited endpoint moves. One
difference is deliberate: bytes at or above `0x80` pass through rather than
being escaped one at a time, because boost's escaper turns a UTF-8 sequence into
a run of `\u00XX` that no longer decodes to the character it came from — and
asset names and memos are user text that has to survive the trip.

**`accounts_receivable` answers two different questions.** Nano's takes an
`accounts` array and answers per account under `blocks`. Kei's takes one
`account` and answers with a flat `receivables` array carrying assets alongside
Kei. The tree served only the first, so `HttpNode.receivables()` found no
`receivables` key and read every account as having nothing to collect.

That is the failure mode to watch for in the rest of this work: **an empty list
rather than an error.** A wrong shape that raises is found the first time it is
called; a wrong shape that returns nothing looks exactly like a correct node with
an idle account, and the receive path would have looked merely slow. It survived
because §1's conformance suite is the thing that would have caught it and the
suite cannot run yet.

Both shapes now answer to the same name, told apart by which parameter the caller
sent, because the name belongs to the contract. Kei receivables and asset
receivables arrive in one list because they are one event to the SDK — something
arrived and needs a block of the recipient's own to collect it (SPEC §5.6.3).

**Absent is null, and that one is a divergence.** Looking for more of the same
turned up two: `account_info` answered `Account not found` and `block_info`
answered `Block not found`, where `docs/rpc.md` says *"anything absent is `null`
or an empty array, not an error"*. `HttpNode` raises any `error` as a thrown
`KeiError`, so a wallet asking its balance before it had ever been paid threw
where it has to read zero — and every account starts there.

Both now answer `null`, and `block_info` also answers with `block` in the shape
`process` accepts rather than only Nano's `contents` string. Unlike
`accounts_receivable` there is no parameter to tell the two callers apart here,
so this is taken from inherited behaviour rather than added beside it. §5.6.8
says to stay compatible wherever it costs nothing; here it costs the contract,
so the contract wins.

**Two shapes are still wrong, and one of them needs a decision rather than
code.** `account_history` returns Nano's history entries — `type`, `amount`,
`account` — where the contract wants blocks in the shape `process` accepts,
under the same `history` key. There is no parameter to dispatch on, so serving
the contract means Nano's `account_history` stops returning what it returns
today, and that is a larger break than the two above: it is the endpoint every
inherited explorer reads. It is left alone deliberately, pending that call.
`faucet` (testnet only) is not implemented at all, which is consistent with §0
putting the public testnet in M3.

## 16. What is not finished, stated plainly

Two things are implemented but not yet demonstrated, and two are not implemented.

**The reserve set is empty.** `ledger_constants::reserve_accounts` is the fixed,
immutable enumeration §5.7 requires, and `is_reserve` enforces the null
representative and the send lock against it — but it has no members until the
genesis ceremony produces them, so on dev those rules currently hold vacuously.
The four circulating allocations and the reserve total are asserted at startup
today; the *blocks* that distribute them are part of the same ceremony.

**Nothing has been executed.** Per §3 there is still no toolchain on this
machine, so everything here is checked by compiling in CI and by having been
written against the mock's semantics. "Compiles" is not "works", and no assertion
in this document should be read as though it were.

**One deliberate divergence from the mock.** §1 makes `MockLedger` the reference
and says that where this node could differ it does not. It differs in exactly
one place, on purpose: the mock checks reserve-locked only on a `send`, so a
reserve account can still `issue`, and issuance destroys 1,000 Kei (§12). That is
a supply change with no vote behind it, which SPEC §5.7 does not permit. The node
refuses it. The cost is nothing today, because the reserve set is empty until
the ceremony, and the mock should adopt the same rule.

**`account_history` still answers in Nano's shape**, and `faucet` does not answer
at all. §15 has both, and the first needs a decision about what inherited
tooling is owed before it is worth writing.

**The SDK's hash has to move.** §7 settled that `asset` blocks hash as binary
fields under a preamble, and §14 extends that to every block type. The SDK still
hashes canonical JSON under `"kei-block-v0"`, so the two disagree and signatures
will not verify across them. §7 already noted this lands as one change on the SDK
side; it has not been made, and until it is, definition-of-done (6) cannot pass
no matter what this node does.

---

## Definition of done for M2

1. `kei-node` builds and runs a single local node.
2. Genesis produces exactly 10^30 raw, the four circulating allocations sum to
   exactly 10^29, and the node refuses to start if either is false.
3. Reserve accounts are enumerated in genesis, name the null representative, and
   contribute zero weight of any kind.
4. The node validates `issue`, `mint`, `burn`, `transfer`, and `asset_receive`,
   including max-supply caps, transfer policy, and the 1,000 Kei issuance burn.
5. `balanceOf` answers in a single call, from `holders`.
6. `packages/core/test/mock-server.test.ts` and `packages/kei/test/over-http.test.ts`
   pass against `kei-node` with only the URL changed.

(6) is the one that matters. The others are things the mock already does.
