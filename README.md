# kei-node

The Kei node. A fork of [Banano](https://github.com/BananoCoin/banano), itself a
fork of [Nano](https://github.com/nanocurrency/nano-node), adding a **native
token primitive** and a Kei genesis.

> **Status: M2, just started.** This is an unmodified Banano V25.1 checkout plus
> [`docs/decisions-m2.md`](docs/decisions-m2.md). No Kei consensus code has been
> written yet, and it does not build on the machine it was cloned onto — see
> [Building](#building). Nothing here holds value.

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
| `issue` | Create a token: name, symbol, decimals, max supply. Permanent, and its parameters immutable. Burns 1,000 Kei. |
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

Standard Banano/Nano build: CMake, a C++17 toolchain, and Boost, with submodules
initialised first.

```sh
git submodule update --init --recursive
```

**It does not build on the machine this was cloned onto**, which is worth stating
rather than discovering: there is a MinGW `g++` and nothing else — no `cmake`, no
`make`, no `ninja`, no Docker, and WSL is installed with no distribution. In
rough order of how much time they cost:

1. **Docker** — `docker/node/Dockerfile` exists upstream, matches CI, and avoids
   a Windows-native Boost build.
2. **WSL2 + Ubuntu** — `wsl --install -d Ubuntu`. Closest to how the node is
   actually deployed at M3.
3. **Windows-native** — CMake + vcpkg + Boost. Works, and upstream CI does it,
   but it is the slowest path and the least like production.

## Upstream

`upstream` points at BananoCoin/banano. Full upstream history is kept
deliberately, so this can be rebased onto a later Banano release rather than
hand-porting security fixes forever.

Forked at `c1f8405d` — Banano V25.1. Upstream's own README is preserved as
[`README-banano.md`](README-banano.md).

## Licence

Banano and Nano are MIT, and so is this. See [`LICENSE`](LICENSE).
