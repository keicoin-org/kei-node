# kei-node

The Kei node. A fork of [Banano](https://github.com/BananoCoin/banano), itself a
fork of [Nano](https://github.com/nanocurrency/nano-node), adding a **native
token primitive** and a Kei genesis.

> **Status: M2, in progress.** This is a Banano V25.1 checkout plus
> [`docs/decisions-m2.md`](docs/decisions-m2.md), its own build, and its own
> repository identity. In: the 10^18-raw ratio, `kei_` addresses, the `asset`
> block primitive (§7), the `holdings`/`holders` tables (§9), ledger validation
> and rollback for all five asset operations, per-operation work tiers (§11),
> Kei's own genesis and a hash domain that separates this chain from Banano's
> structurally rather than by convention (§14), the asset half of the RPC, and a
> response encoder that emits the contract's numbers, nulls, and empty arrays
> instead of quoting everything (§15).
>
> **Not finished** (§16): the reserve set is empty until the SPEC §5.7 genesis
> ceremony fills it, so the reserve rules currently hold vacuously, and the SDK
> still hashes blocks the M0 way, so signatures do not verify across the two
> until it follows §7. **Nothing here has been executed** — it compiles in CI,
> which is not the same claim. It still does not build on the machine it was cloned onto,
> see [Building](#building). Nothing here holds value.

## Why a fork at all

The product promise is that a game developer never runs payment infrastructure.
That promise dies the moment token balances live in a database the developer
operates, because at that point they own custody, security, backups, and fraud —
which is precisely the burden Kei exists to remove.

So **token balances are enforced by consensus**: not in an SDK-managed ledger,
not in the developer's Postgres, and not in an indexer's interpretation of
ordinary transactions. A meta-protocol approach — the technique behind Banano
NFTs and BRC-20, where rules are layered on ordinary transactions and enforced
only by indexers — was considered and rejected for exactly this reason.

The mental model for what is being added is **Stellar, not Ethereum**: you
declare an asset, accounts hold it, transfers are validated by consensus, and no
code is ever deployed.

## Inherited unchanged

Block-lattice architecture (one chain per account), feeless transactions, no
mining, ORV consensus, sub-second confirmation, and the base32 address encoding —
so existing wallets, explorers, and libraries can be adapted rather than
rewritten.

## What M2 adds

| Op | Behaviour |
|---|---|
| `issue` | Create a token: name, symbol, decimals, max supply. Permanent, and its parameters immutable. Burns n Kei for the account's nth token, so a first one costs 1. |
| `mint` | Issuer creates units. Rejected if it would exceed max supply. |
| `burn` | Destroy units permanently. The economic sink. |
| `transfer` | Move units between accounts, subject to the token's immutable transfer policy. |
| `asset_receive` | Collect a receivable asset — the asset-side twin of `receive`. |

Plus a genesis block producing exactly 1,000,000,000,000 Kei with the SPEC §5.7
allocation, and `balanceOf` answerable in a single call.

`commit`/`claim` land at M4, swaps at M5, and reserve governance before mainnet.
[`docs/decisions-m2.md`](docs/decisions-m2.md) says why, and settles the wire
format, the store layout, the work tiers, and the genesis arithmetic.

## The contract this node has to serve

The SDK is already written, tested, and talking to a mock node over HTTP. That
mock is the reference implementation, and the contract is executable rather than
described:

- [`kei-transaction/docs/rpc.md`](../kei-transaction/docs/rpc.md) — the wire
  contract, action by action
- `packages/core/test/mock-server.test.ts` and `packages/kei/test/over-http.test.ts`
  — the conformance suite

**M2 is done when those two files pass against this node with only the URL
changed.** M3 then points the demo game at it, and nothing above the URL moves.

## Building

Standard Banano/Nano build: CMake and a C++ toolchain, with submodules
initialised first. **Boost is not a dependency you install** — the Boost
superproject is vendored at `submodules/boost` and built as part of the tree,
which is why the checkout is large and why there is no vcpkg step.

```sh
git submodule update --init --recursive

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DACTIVE_NETWORK=banano_dev_network \
  -DNANO_GUI=OFF -DNANO_TEST=ON -DPORTABLE=1
cmake --build build --target nano_node
```

**It does not build on the machine this was cloned onto**, which is worth stating
rather than discovering. MSVC 2022 Build Tools is installed but unusable — there
is no Windows SDK, so `cl` fails on `stdio.h` — and MinGW `g++` compiles trivial
C++ but is not a toolchain upstream supports. There is no `cmake`, no `ninja`, no
Docker, and WSL has no distribution. The binding constraint is that the account is
**not an administrator**, so every local fix needs the machine's owner:

1. **GitHub Actions** — [`.github/workflows/build.yml`](.github/workflows/build.yml).
   No admin, no local install, and the only option that works today.
2. **WSL2 + Ubuntu** — `wsl --install -d Ubuntu`. Needs admin. Closest to how the
   node is actually deployed at M3, and the best local loop. Ask for this one.
3. **Windows SDK + CMake** — makes the installed MSVC work. Needs admin.
4. **Docker Desktop** — reproducible, heaviest install, also needs admin.

[`docs/decisions-m2.md`](docs/decisions-m2.md) §3 has the detail.

## Upstream

`upstream` points at BananoCoin/banano and is **fetch only** — its push URL is set
to `no_push`. Full upstream history is kept deliberately, so this can be rebased
onto a later Banano release rather than hand-porting security fixes forever.

**Nothing here is ever proposed back to Banano.** "Fork" means a derivative
codebase that diverges permanently, not a GitHub fork with a pull request pending.
Kei's own remote is `origin` → `keicoin-org/kei-node`, and `master` is Kei's trunk.

Forked at `c1f8405d` — Banano V25.1. Upstream's own README is preserved as
[`README-banano.md`](README-banano.md).

## Licence

Banano and Nano are MIT, and so is this. See [`LICENSE`](LICENSE).
