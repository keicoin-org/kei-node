#!/usr/bin/env bun
/**
 * Regenerates the M4 claim-hashing cross-language vectors pinned in
 * `nano/core_test/asset_ledger.cpp`, straight from the frozen SDK
 * (kei-transaction's `packages/core/src/merkle.ts`, `blocks.ts`, `crypto.ts`).
 *
 * This exists because the node previously computed a leaf hash that
 * disagreed with the SDK's — a hashed domain-separator label instead of the
 * SDK's literal `0x00`/`0x01` tag byte — and nothing caught it until a real
 * proof from `@keicoin/claims` failed `bad_claim_proof` against a running
 * node. Neither side's own test suite could have caught that on its own:
 * each was internally consistent, just with the other.
 *
 * Usage:
 *   KEI_SDK_DIR=/path/to/kei-transaction bun run conformance/generate-claim-vectors.ts
 *     Prints the current vectors as JSON to stdout. Redirect to
 *     conformance/claim-vectors.json to update the pinned fixture after an
 *     intentional SDK hashing change — and update the matching hex literals
 *     in nano/core_test/asset_ledger.cpp in the same change.
 *
 *   KEI_SDK_DIR=/path/to/kei-transaction bun run conformance/generate-claim-vectors.ts --check
 *     Compares the current vectors against conformance/claim-vectors.json and
 *     exits non-zero if they differ. This is what CI runs (build.yml) against
 *     the same pinned SDK revision the M2 conformance job uses, so a hashing
 *     change on either side of the contract that the other side missed fails
 *     the build instead of failing silently on a real network.
 */
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const sdkDir = process.env.KEI_SDK_DIR
if (!sdkDir) {
  console.error('Set KEI_SDK_DIR to a kei-transaction checkout.')
  process.exit(1)
}

const { leafHash, combineHashes } = await import(join(sdkDir, 'packages/core/src/merkle.ts'))
const { deriveAssetId } = await import(join(sdkDir, 'packages/core/src/blocks.ts'))
const { derivePublicKey } = await import(join(sdkDir, 'packages/core/src/crypto.ts'))

const ACCOUNT_A = 'A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1'
const ACCOUNT_B = 'B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2'
const ACCOUNT_C = 'C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3C3'
const ACCOUNT_D = 'D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4D4'
const ACCOUNT_E = 'E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5E5'
const ASSET = 'FEEDFACE'.repeat(8)
const ASSET2 = 'C0FFEE00'.repeat(8)

// The M2 dev-network issuer key (nano::dev::team_key in nano/secure/common.cpp)
// and a fixed test-only key, so the end-to-end vector below matches an asset
// id and account the node test can derive and sign for at runtime rather than
// needing the vectors to carry an unsigned block.
const TEAM_PRV = '0E74FB6ED22BDDE439F45E0FB154029C6308755EDE04EE20578A7DE0A00EFB04'
const PLAYER_PRV = 'AA00'.repeat(16)

function foldTree(leaves) {
  const levels = [leaves]
  let level = leaves
  while (level.length > 1) {
    const above = []
    for (let i = 0; i < level.length; i += 2) {
      above.push(i + 1 < level.length ? combineHashes(level[i], level[i + 1]) : level[i])
    }
    level = above
    levels.push(level)
  }
  function proofFor(index) {
    const result = []
    let idx = index
    for (let lvl = 0; lvl + 1 < levels.length; lvl++, idx = Math.floor(idx / 2)) {
      const sibling = idx ^ 1
      if (sibling < levels[lvl].length) result.push(levels[lvl][sibling])
    }
    return result
  }
  return { root: level[0], proofFor }
}

const vectors = {}

{
  const leaf = leafHash(ACCOUNT_A, ASSET, 500n)
  vectors.single_leaf = { account: ACCOUNT_A, asset: ASSET, amount: '500', leaf, proof: [], root: leaf }
}

{
  const leafA = leafHash(ACCOUNT_A, ASSET, 500n)
  const leafB = leafHash(ACCOUNT_B, ASSET, 1n)
  const root = combineHashes(leafA, leafB)
  vectors.two_leaf = {
    leafA: { account: ACCOUNT_A, asset: ASSET, amount: '500', leaf: leafA, proof: [leafB] },
    leafB: { account: ACCOUNT_B, asset: ASSET, amount: '1', leaf: leafB, proof: [leafA] },
    root,
  }
}

{
  const accounts = [ACCOUNT_A, ACCOUNT_B, ACCOUNT_C, ACCOUNT_D, ACCOUNT_E]
  const amounts = [100n, 200n, 300n, 400n, 500n]
  const leaves = accounts.map((account, i) => leafHash(account, ASSET, amounts[i]))
  const { root, proofFor } = foldTree(leaves)
  vectors.five_leaf = {
    leaves: leaves.map((leaf, i) => ({ account: accounts[i], amount: amounts[i].toString(), leaf, proof: proofFor(i) })),
    root,
  }
}

{
  const zero = leafHash(ACCOUNT_A, ASSET, 0n)
  const maxAmount = (1n << 128n) - 1n
  const max = leafHash(ACCOUNT_A, ASSET, maxAmount)
  vectors.boundary_amounts = {
    zero: { account: ACCOUNT_A, asset: ASSET, amount: '0', leaf: zero },
    max_uint128: { account: ACCOUNT_A, asset: ASSET, amount: maxAmount.toString(), leaf: max },
  }
}

{
  const leaf1 = leafHash(ACCOUNT_A, ASSET, 500n)
  const leaf2 = leafHash(ACCOUNT_A, ASSET2, 500n)
  vectors.asset_binding = { leaf1, leaf2, equal: leaf1 === leaf2 }
}

{
  const teamPub = await derivePublicKey(TEAM_PRV)
  const playerPub = await derivePublicKey(PLAYER_PRV)
  const assetId = deriveAssetId(teamPub, 'GEM')
  const leaf = leafHash(playerPub, assetId, 500n)
  vectors.end_to_end = {
    team_prv: TEAM_PRV,
    team_pub: teamPub,
    player_prv: PLAYER_PRV,
    player_pub: playerPub,
    asset_id: assetId,
    amount: '500',
    leaf,
    root: leaf,
    proof: [],
  }
}

if (process.argv.includes('--check')) {
  const fixturePath = join(dirname(fileURLToPath(import.meta.url)), 'claim-vectors.json')
  const pinned = JSON.parse(readFileSync(fixturePath, 'utf8'))
  const actual = JSON.stringify(vectors, null, 2) + '\n'
  const expected = JSON.stringify(pinned, null, 2) + '\n'
  if (actual !== expected) {
    console.error('M4 claim vectors have drifted from conformance/claim-vectors.json.')
    console.error('If this SDK change is intentional, regenerate the fixture:')
    console.error('  bun run conformance/generate-claim-vectors.ts > conformance/claim-vectors.json')
    console.error('and update the matching hex literals in nano/core_test/asset_ledger.cpp.')
    console.error('--- pinned ---')
    console.error(expected)
    console.error('--- actual ---')
    console.error(actual)
    process.exit(1)
  }
  console.log('M4 claim vectors match the pinned fixture.')
} else {
  console.log(JSON.stringify(vectors, null, 2))
}
