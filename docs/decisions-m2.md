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

**Dev runs that structure completely, with published test keys.** Its genesis
account is the singleton reserve named by the `open` block, starts with the null
representative, makes four fixed state sends, and leaves exactly 9 × 10^29 raw.
The grants, community, bounty and team accounts open those sends for exactly
3.7, 2.8, 1.8 and 1.7 × 10^28 raw. `util/keigen.py` derives every dev key from a
published `kei-dev-*` phrase and reproduces every block; they are test keys by
construction and hold nothing of value. Store initialisation installs those nine
blocks as cemented genesis history, before ordinary reserve locking begins.

**No beta/live seed is a literal in this repository.** Their reserve is 90% of
all valuable Kei, its custody is multisig (§5.7), and its key is generated
offline. What eventually ships is the public address and signed ceremony blocks,
never a seed. Until then their all-zero placeholders still make
`ledger_constants` refuse startup; deterministic dev data is never substituted
for production ceremony output.

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
| `balance` | 16 | **The account's Kei balance, unchanged from its predecessor** — §5.6.1's concession to §5.6.8, so a Banano-derived explorer that ignores the asset payload still tracks Kei correctly instead of reporting a broken balance. Except on `issue`, which burns Kei (§12), and where this field is how the burn is expressed. |
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

## 12. Issuance burns n Kei for an account's nth asset

SPEC §5.6.5. Work is a rate limit; this is a cost, and it is the one place Kei is
not free. An asset record is permanent global state that every node stores
forever, and one account can otherwise mint an unbounded number of distinct
symbols.

Expressed as a decrease in the `issue` block's `balance` field with no
corresponding receivable — the Kei is destroyed, not moved. The node must reject
an `issue` whose balance decrease is not exactly the price of that account's next
asset.

**The price escalates per account, and it is linear per asset.** SPEC used to
charge a flat 1,000 Kei; it now charges 1 Kei for an account's first asset, 2 for
its second, n for its nth. That is a deliberate choice of curve. A flat price
does not bound what the burn exists to bound: the thing to make expensive is one
account creating a great many records, not one account creating its first, which
is the one a developer meets before they have any reason to trust the project.

Exponential was considered and rejected. Doubling reaches 2^500 Kei by SPEC's own
worked example — a game with a currency and five hundred item types — which does
not price spam, it prices the product. Linear-per-asset makes the total quadratic
in the table size, which is the mildest curve that still makes a large table from
one account impossible: 501 assets cost 125,751 Kei where the flat rate charged
501,000, and a million cost five times the circulating supply.

It is also the only anti-spam measure available to this node that does not
require an identity. §5.6.3 means only an account can extend its own chain, so
the count cannot be shed by anyone but the account paying it. Everything else —
rate limits, one-grant-per-human — needs to know who somebody is, and a
permissionless block-lattice never does.

**The count is state, so it needs a table and a rollback.** `issued` maps an
account to how many assets it has issued, in both backends, alongside the §9
tables. Zero is absence, the same rule `holdings` follows: an account that has
never issued has no entry, and rolling back an account's first issuance leaves it
with none again. Rollback walks one chain backwards, so the block being undone is
always that account's most recent issuance and the stored count is its ordinal —
which is why decrementing is correct and does not need to re-derive anything.

Be precise in the docs about what this does and does not contradict: **the
feeless promise is about transactions**, and it survives intact. Sending Kei,
transferring tokens, minting, and claiming are free forever.

**The count has to be readable, or no client can issue.** The burn is a balance
decrease the `issue` block states exactly, so a signer that does not know how
many assets it has already issued cannot construct a valid one. `account_info`
therefore carries `issuedCount`. That is an addition to
[`rpc.md`](../../kei-transaction/docs/rpc.md) rather than something it already
asked for, and the SDK and `MockLedger` need the same field before the
conformance suite can pass — it is listed in §16 with the other SDK-side work.

**Seeding is not part of this, on purpose.** Making the first asset cost 1 Kei
raises the obvious next question — where does a developer get their first Kei —
and the answer is not a consensus rule. Consensus cannot see a new wallet: an
account is a keypair, there is no registration event, and "an account with no
chain may claim once" is unlimited free money to anyone who can run a loop. So
seeding is a faucet, it is testnet-only, and it targets issuers rather than every
wallet, because transactions are feeless and a player needs no Kei to play. SPEC
§5.6.5 now says so; the node's part is the `faucet` action, which §16 lists as
not implemented and which belongs with M3's testnet.

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

**`account_history` dispatches on a parameter that did not exist, which is the
general form of what `accounts_receivable` did by luck.** It returns Nano's
history entries — `type`, `amount`, `account` — where the contract wants blocks
in the shape `process` accepts, under the same `history` key. Neither of the two
devices above works on it:

- **It cannot be replaced the way `block_info`'s error was.** Nano's entries are
  not a worse rendering of the block, they carry information the block does not:
  `amount` and the counterparty `account` are derived by differencing against the
  previous block's balance. An explorer handed raw blocks does not get a new
  shape, it loses fields it would need one extra lookup per entry to rebuild.
  This is also the endpoint inherited tooling reads most, so the §5.6.8 trade is
  at its worst here.
- **It cannot be served beside the inherited answer the way `block_info` serves
  `block` next to `contents`.** That worked because those are two top-level keys.
  Here the collision is *inside* each array element and there is no superset
  entry: `type` is Nano's subtype where a block's is `"state"`, and `account` is
  the counterparty where a block's is the signer. Both keys, both shapes,
  different meanings. Raw mode already resolves the first (`type: "state"` plus
  `subtype`) and does not resolve the second.

So it dispatches, on `shape`: absent is Nano's answer and `shape=block` is the
contract's. The honest difference from `accounts_receivable` is that `account`
versus `accounts` was already there to be read, and this discriminator is
manufactured — which is affordable only because both ends are ours, and costs a
line of `HttpNode` and a line of `rpc.md`. What it buys is that no inherited
caller moves at all, which is the one thing neither alternative could offer.

Four details, each of which could have gone quietly wrong:

**The blocks come from `block->serialize_json`,** the same producer `block_info`
already uses, rather than from a second copy grown inside `history_visitor`. The
visitor is further from the contract than it looks — it emits no `op` for an
asset block and no signer for a state one — so the copy would have been written
from scratch and would then have had to be kept in step with a shape `process`
also parses.

**Except for `subtype`, which the block does not carry.** `process` takes a state
block's subtype *beside* the block rather than within it, so `serialize_json`
has none; `rpc.md`'s entries show one and the SDK's `StateBlockBody` requires
one. The sideband already knows it, so it costs no lookup. One mismatch in
vocabulary is worth naming: an account's first block is an `open` to the SDK
where the sideband says only receive, and epoch blocks have no SDK spelling at
all.

**Every block on the chain appears.** The visitor drops what it cannot derive an
entry for — a change block without `raw`, most visibly — and a history with
holes in it is not something a caller can rebuild state from.

**`account_filter` is refused rather than ignored.** It selects by counterparty,
which only the visitor derives, so block shape cannot honour it. Silently not
applying it would return more than was asked for while looking correct, which is
this section's whole subject.

**The mock requires the parameter it could infer.** `mockRpcHandler` has only
the contract's shape and could serve it unasked, but then an SDK that forgot to
send `shape` would pass against the mock and read Nano's entries as blocks
against this node — entries that parse cleanly and describe a different block.
Holding callers to it at the reference implementation is the only check
available until definition-of-done (6) can run.

`faucet` (testnet only) is still not implemented at all, which is consistent
with §0 putting the public testnet in M3.

## 16. What is not finished, stated plainly

One acceptance blocker remains, and what is implemented is unevenly demonstrated.

**The reserve rules are real on dev, not vacuous.** The genesis account is the
single immutable reserve member and names the null representative from its first
block. Four fixed sends and opens create the exact 100B circulating allocation
and leave 900B in reserve. `genesis_reserve.*` checks the five account records,
the 10^30 total, every ceremony block, both the first-start and rebuilt
representative caches, and the null-representative/send/issue rejection paths.
Beta/live remain deliberately unstartable until their offline ceremony produces
public addresses and signed blocks; no production key was invented to make a
local milestone green.

**What has been executed, and what has only been compiled.** Per §3 there is no
toolchain on this machine, so anything not named below is checked by compiling in
CI and by having been written against the mock's semantics. "Compiles" is not
"works", and no assertion in this document should be read as though it were.

The first exception, from the `account_history` work in §15: `rpc_test` is built
in CI and one case runs there. It stands up a node through `nano::test::system`,
processes a change, a send and a receive through `ledger.cpp` and the store, and
reads them back from a live RPC server. So the ledger and store paths are no
longer entirely untried — a chain is built and queried on every run.

That harness being cheap is what `core_test/asset_ledger.cpp` spends. **All five
asset operations now go through `ledger_processor` and back out through
`ledger::rollback` on every CI run**, against a real store: the escalating burn
and the issuance count it prices from, a mint arriving as a receivable and the
`asset_receive` that collects it, the `holdings` and `holders` entries that
collect writes and the deletion of both when a balance reaches zero, the max
supply cap and the headroom a burn frees (§5.6.6), and both transfer policies
that can refuse a move.

Rollback is covered because rollback is the code whose bugs stay invisible until
a fork actually happens — including the case that is easy to get wrong, where the
recipient has already collected the mint being rolled back and their
`asset_receive` has to come off their own chain first.

**It caught one immediately, and the shape of it is the argument for running
code.** `burn` and `asset_receive` are the only two ops whose canonical payload
is zero bytes — every other op writes at least a length prefix — and a
zero-length payload could not be parsed at all. An empty vector's `data ()` is
null, `nano::bufferstream` is a boost *direct* device, and its first read over a
null buffer throws `bad_read` rather than reporting end-of-stream. So both block
types serialised correctly, stored correctly, and then could not be read back:
unrollable, unbootstrappable, and unservable over RPC. Nothing found it because
nothing had ever read one back — the round-trip test in `block.cpp` uses an
`issue` block, whose payload is never empty. `asset_ledger.cpp` now round-trips
all five ops through the exact record the store writes.

What is still unexecuted after that: `asset_info` and the rest of the asset RPC,
which have no test of their own; and everything in §15 apart from
`account_history`. A ledger test is also not a running node — definition-of-done
(6) remains the acceptance blocker even though (1) now runs on the Linux box (§17).

**One deliberate divergence from the mock.** §1 makes `MockLedger` the reference
and says that where this node could differ it does not. It differs in exactly
one place, on purpose: the mock checks reserve-locked only on a `send`, so a
reserve account can still `issue`, and issuance destroys Kei (§12). That is
a supply change with no vote behind it, which SPEC §5.7 does not permit. The node
refuses it. `genesis_reserve.ordinary_blocks_cannot_delegate_send_or_issue`
executes that stronger rule against the populated dev reserve, and the mock
should adopt the same issue lock.

**`account_history` answers in both shapes, and this one has actually been
run.** §15 records the decision and what it turns on. Both halves are executed
rather than argued:

- The SDK half — `mockRpcHandler` requires `shape`, `HttpNode` sends it, and the
  conformance suite now covers `account_history`, which it did not before. That
  absence is why the shape being wrong went unnoticed in the first place.
- The node half — `rpc.account_history_block_shape` builds a chain, asks for it
  in both shapes, and checks the things that distinguish them: that `type` is
  the block type and not the subtype, that `account` is the signer and not the
  counterparty, that `previous` and `signature` are present where the inherited
  entry has neither, that the change block the inherited shape drops is there,
  and that an unknown `shape` and a `shape` + `account_filter` combination are
  refused rather than answered.

Getting there needed `rpc_test` built in CI at all, which it never had been —
so the §15 shapes that shipped before this one (`accounts_receivable`,
`block_info`, `account_info`) had their tests read rather than compiled. They
now at least compile; none of them has a test of its own yet, and each is worth
one.

`faucet` (testnet only) is the one action still not implemented at all.

**The SDK now knows about the escalating burn.** §12 changed issuance from a flat
1,000 Kei to n Kei for an account's nth asset, and added `issuedCount` to
`account_info` so a signer can compute it. `MockLedger` charges
`issuanceBurn(ordinal - 1)` and `rpc.md` carries the field, so the two agree
about whether a given `issue` block is valid. What remains unchecked is that
they agree *with each other* rather than separately with this document, and only
definition-of-done (6) can settle that.

**The SDK's hash has moved, and both sides are pinned to the same vectors.** §7
settled that `asset` blocks hash as binary fields under a preamble and §14
extended that to every block type, which left the SDK hashing canonical JSON
under `"kei-block-v0"` and the two disagreeing about every signature. The SDK
now hashes as this node does. What makes that more than two implementations
agreeing with this document separately: `block.kei_hash_vectors` here and the
SDK's own vector test assert the same hashes for the same blocks, so a change to
either side that moves a hash fails on both.

---

## 17. What running it found

§3 said the binding constraint was that this tree had no machine to compile on.
It has one now — a Linux box, built from the same recipe as
[`.github/workflows/build.yml`](../.github/workflows/build.yml) — so
definition-of-done (1) is met: **`bananode` builds, starts, and serves RPC**, and
[`conformance/`](../conformance/) drives the SDK's M2 suite against it over HTTP.
The first run used a copied harness and found four failures. That was useful
diagnostically, but a copy that changes assertions is not "only the URL changed",
and two failures were `commit`/`claim` cases that section 0 assigns to M4. The
gate now runs the exact SDK-owned M2 files with `KEI_NODE_URL`; the M4 cases stay
executable in explicitly named M4 suites and are not pulled into this milestone.

**`faucet` existed only in the contract.** It is testnet-only (SPEC §12), pays
from the deterministic dev `community` allocation, matching the mock, and
serialises its read-build-process, because two calls reading one frontier fork
the single account every test funds from.

**`process` could not read a block.** Nano sends `block` as a *string* of JSON
and opts into an object with `json_block`; rpc.md sends the object, with no such
flag, as every other action in that contract does. So `get<std::string>` on a
subtree yielded an empty string, an empty string is not JSON, and every block the
SDK ever signed came back "Block is invalid" without the node looking at it.
`block_impl` now accepts whichever arrived — a subtree has children and a string
does not — which keeps the inherited form working.

**§11's work tiers were advertised but not enforced.** The tiers are not separate
constants: B *is* `epoch_2` and C *is* `epoch_2_receive`, so they govern only
once an account has reached epoch 2. Kei sat at epoch 0, where the single
`epoch_1` threshold covers sends and receives alike — and on dev that is `0xfe00…`
against tier C's `0xf000…`. The node answered `work_thresholds` with a receive
tier its own ledger then refused, so **every client's opening block failed as
"insufficient work"**. Kei starts at epoch 2 now, in both places the fact is
written: the genesis sideband in `common.cpp` and the genesis *account record* in
`store.cpp`, which is the one actually read when the next block is validated and
which had it hardcoded. Fixing only the first changed nothing, which is how the
second was found. No block hash moves — the sideband is not hashed, and the §14
vectors still pass.

**A bad signature cost fifteen seconds and said nothing.** State blocks whose
signature fails verification are dropped rather than queued, so `process_one`
never saw them and the promise `add_blocking` waits on was never settled: the
caller waited out the whole `block_process_timeout` and was told "Stopped", which
names the timeout rather than the fault. It is also the first thing any new
client integration meets, because signing over the wrong bytes — a different hash
domain (§14), a field the layout has no room for (§8) — lands exactly here. Those
promises are now settled with the real reason: **15,001 ms to 1 ms**, and "Bad
signature" instead of "Stopped".

**The one thing left that is a decision rather than a defect** is the shape of a
Kei payment carrying a memo. §8 put memos on the asset block and left `state`
untouched, and `kei.pay({ memo })` builds a `state` block with a `memo` field —
which the §14 layout has no room for. The SDK already knows: `wire.ts` names it a
node-layout gap and hashes such a block under a deliberately local domain so that
a node rejects it loudly rather than accepting it with the memo quietly dropped.
So the two implementations agree that this block is not valid; what was never
settled is what the valid one looks like. §8 implies an asset-family block with
asset id 0, which is a wire question this document should answer before M3
depends on it.

## 17. A Kei payment carries no memo, and `pay({ memo })` says so out loud

§8 took decisions-m0 §4 option (a) — memos ride on the `asset`-family block —
and closed with "The SDK surface does not move either way, `kei.pay({ memo })`
is already written and already tested." That sentence is the one part of §8 that
is wrong, and everything below follows from it.

`kei.pay()` is `client.send()`, which builds a `state` block, and §8 left `state`
blocks untouched. `asset`/`transfer` is not a route either: `asset_id` is
`H(issuer_pubkey ‖ symbol)` (§5.6.1) and Kei has no issuance block, so there is
no id to name. A memo'd Kei payment therefore has no valid representation on this
chain. §8 settled where an *asset* memo lives and read as though it had settled
where a *Kei* memo lives.

What that costs today is worse than being unrepresentable.
`state_block::deserialize_json` reads seven named fields and ignores every other
key, so a `state` block carrying `memo` is accepted and the memo is discarded.
Once the SDK's hash moves (§16) it will cover the §14 layout, which has no memo
in it, so the hashes will agree, the signature will verify, the block will
process, and the memo will be gone. Compare `commit`, `commit_close` and `claim`,
which hash under an op byte no node computes and are rejected loudly. Silent
truncation is the failure mode a payments protocol can least afford.

**Taking: memos are asset-only for M2, and `pay({ memo })` is an error.** It fails
the same way and for the same reason the three deferred ops do — the SDK does not
offer a surface this node cannot honour. Definition-of-done (6) drops the memo
from the Kei leg of `over-http.test.ts`; the memo on the *asset* leg stays, is
carried in `asset_pending_info`, and is delivered to the recipient, which is what
§8 actually bought.

Two routes were considered for making it representable, and both are M4 work
rather than an M2 patch:

**Kei as asset 0** is not a reinterpretation, it is a ledger change. Asset
validation requires `new_balance == previous_balance` for every op but `issue`,
so an `asset` block that moves Kei is rejected by construction today. Kei
receivables and asset receivables are also separate stores with different value
types: `pending_info` is `{source, amount, epoch}` and has nowhere to put a memo,
while `asset_pending_info` carries one but is collected by `asset_receive`, which
credits `holders` rather than the Kei balance. The route needs a schema change to
one store or the other, plus a carve-out in §5.6.1's "derived, never assigned".

**A memo on Kei's own `state` block** is the better of the two and the one M4
should take. §14 already domain-separates every block type, so no Nano or Banano
tool can read a Kei `state` hash regardless, and the compatibility §8 was
protecting is not there to lose. It costs exactly what the `asset` block already
pays: `state_hashables::size` stops being a constant, and everything that frames
a `state` block by fixed size follows it. That is real work and it is not M2's.
When it lands, §8's closing sentence should be struck rather than reinterpreted.

It is not urgent, because the correlation problem §8 set out to solve is already
solved for Kei by a weaker means: `onPayment` hands the issuer the block hash,
and matching on a hash is exact where §8's amount-and-timing was racy. What it
needs is an out-of-band channel from payer to issuer to carry that hash — which
the Button demo has, and a shop that knows its customers only on-chain does not.
That gap is the case for the memo, and it is worth one milestone's wait.

SPEC §6.7's worked example is `kei.pay({ to, amount: 0.05, memo: 'Sword of
Testing' })` with a matching `onPayment(({ from, amount, memo }) => …)`. It is the
marquee example of the whole API and it does not run. It has to change now and
change back at M4.

## 18. The deferred op numbers are reserved, not merely deferred

The `asset_op` comment says `commit`/`commit_close` and the swap legs "land with
M4 and M5 and are deliberately not members of this enum yet". It does not say
what numbers they will take, and an op byte is hashed — picking a different order
at M4 silently invalidates every vector and every signature produced against the
earlier guess.

The SDK's `AssetOp` union already orders them `commit`, `commit_close`, `claim`
after `asset_receive`. **Reserving 5, 6 and 7 for exactly those, and 8 upward for
the §5.6.4 swap legs.** `asset_op_valid` stays bounded at `asset_receive`, so a
reserved number is still rejected on the wire and reserving costs nothing until
the op exists.

## 19. A third M4 candidate exists, prototyped and parked

§17 named two candidate routes for making a memo'd Kei payment representable —
reinterpret `asset_id` zero inside `transfer`/`asset_receive`, or give `state`
blocks a memo field — and pushed both to M4. A third was built and considered
for landing in M2 instead: `kei_transfer`, a dedicated `asset_op` sibling to
`transfer` that always names `asset_id` zero, mutates `balance` at send time
like a state send, and carries an explicit discriminator on its receivable
(`asset_pending_info::via_kei_transfer`) so `asset_receive` credits `balance`
directly rather than `holdings` — avoiding the silent-sentinel failure mode
that ruled out the first route, without reopening §8 the way the second does.

**Not taken for M2.** It reuses `asset`-typed blocks and `asset_pending_info` —
storage this doc keeps deliberately separate from Kei's own chain-balance
ledger — to carry balance-moving operations, which is exactly the boundary §6
and §9 lean on: every asset op but `issue` is `new_balance == previous_balance`
by construction, and that invariant is what lets anything reading `balance`
trust it only moves on `state` blocks plus one bounded, well-known exception.
A discriminator field on a shared receivable type is more honest than an
implicit `asset_id == 0` branch, but it is still two ledgers sharing one store,
told apart at runtime instead of by type — and it lands with zero
`ledger_processor` integration coverage on the one path (#8) just finished
proving needs it most: rollback of a balance-moving op. Landing it now would
mean landing that risk untested, right after closing it for the other five ops.

The prototype is real and complete — `feat/kei-transfer-asset-op`
(kei-node) and `sdk-kei-transfer` (kei-transaction), both open as draft PRs,
neither merged. §17 stands for M2: `pay({ memo })` errors, `onPayment`'s block
hash is the correlation mechanism until M4. When M4 actually arrives, this is
a third option to weigh against §17's original two — with the ledger's asset
paths, and their rollback coverage, in a very different state than they were
when this section was written.

---

## Definition of done for M2

1. `kei-node` builds and runs a single local node.
2. Genesis produces exactly 10^30 raw, the four circulating allocations sum to
   exactly 10^29, and the node refuses to start if either is false.
3. Reserve accounts are enumerated in genesis, name the null representative, and
   contribute zero weight of any kind.
4. The node validates `issue`, `mint`, `burn`, `transfer`, and `asset_receive`,
   including max-supply caps, transfer policy, and the escalating issuance burn.
5. `balanceOf` answers in a single call, from `holders`.
6. `packages/core/test/m2-node.test.ts` and `packages/kei/test/over-http.test.ts`
   pass unchanged against both `MockNode` and `kei-node`; `KEI_NODE_URL` is the
   only switch. The node CI starts a clean dev node and runs those exact files.

(6) is the one that matters. The others are things the mock already does.
