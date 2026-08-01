/**
 * Definition-of-done (6), executed: the SDK's conformance suite with only the
 * URL changed.
 *
 * `kei-transaction`'s own `packages/core/test/mock-server.test.ts` and
 * `packages/kei/test/over-http.test.ts` drive `MockNode` through
 * `mockRpcHandler` over an in-process `fetch`. This file drives the same client
 * calls, in the same order, over real HTTP against a running `bananode`.
 *
 * Where a case there reaches into the mock, it cannot be carried across
 * verbatim, and pretending otherwise would make this file agree with itself
 * rather than with the node. There are three, and each is replaced by the
 * question it was actually asking:
 *
 * - `mock.ledger.genesisAddresses().community` names the mock's faucet account.
 *   A node has its own, so `faucetAccount()` discovers it from a payout rather
 *   than being told.
 * - `expect(await http.workThresholds()).toEqual(await mock.workThresholds())`
 *   compares a node to the mock it is meant to replace. Here the tiers are
 *   asserted to be decimal strings and correctly ordered (§11) instead.
 * - The `handler(...)` cases — unknown action, missing `shape`, CORS preflight —
 *   call the mock's request handler directly, so they never cross a wire at
 *   all. They are POSTed to the node here.
 *
 * Everything else is the suite as written.
 */

import { beforeAll, describe, expect, test } from 'bun:test'
import { HttpNode, KEI_ASSET, ZERO_HASH, keyPairFromSeed, randomSeed, type Block } from '@keicoin/core'
import { Kei } from 'kei-transaction'

const NODE_URL = process.env.KEI_NODE_URL ?? 'http://127.0.0.1:45000'

/** A client, and nothing shared with any other client but the URL. */
const connect = () => new HttpNode({ url: NODE_URL, network: 'dev', pollInterval: 50 })

/** POST an action the way any other client would, bypassing the SDK. */
async function rpc(body: Record<string, unknown>): Promise<Response> {
  return fetch(NODE_URL, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  })
}

/**
 * Which account the faucet pays from — the node's answer to the mock's
 * `genesisAddresses().community`. Read from a payout rather than configured,
 * so this file does not have to know the genesis it is talking to.
 */
async function faucetAccount(): Promise<string> {
  const http = connect()
  const keys = await keyPairFromSeed(randomSeed())
  await http.faucet(keys.address, (1n * 10n ** 18n).toString())
  const receivables = await http.receivables(keys.address)
  const from = receivables[0]?.from
  if (!from) throw new Error('The faucet paid nothing, so there is no account to name.')
  return from
}

describe('the node over HTTP', () => {
  beforeAll(async () => {
    const response = await rpc({ action: 'work_thresholds' }).catch(() => null)
    if (!response?.ok) throw new Error(`No node is answering at ${NODE_URL}.`)
  })

  test('a faucet, then an account that exists', async () => {
    const http = connect()
    const keys = await keyPairFromSeed(randomSeed())

    expect(await http.accountInfo(keys.address)).toBeNull()

    const { hash } = await http.faucet(keys.address, (5n * 10n ** 18n).toString())
    expect(hash).toMatch(/^[0-9A-F]{64}$/)

    const receivables = await http.receivables(keys.address)
    expect(receivables).toHaveLength(1)
    expect(receivables[0]?.asset).toBe(KEI_ASSET)
    expect(receivables[0]?.amount).toBe((5n * 10n ** 18n).toString())
  })

  test('an unknown account, asset, root and block are null, not errors', async () => {
    const http = connect()
    const keys = await keyPairFromSeed(randomSeed())

    expect(await http.accountInfo(keys.address)).toBeNull()
    expect(await http.assetInfo('A'.repeat(64))).toBeNull()
    expect(await http.commitInfo('B'.repeat(64))).toBeNull()
    expect(await http.blockInfo('C'.repeat(64))).toBeNull()
    expect(await http.holdings(keys.address)).toEqual([])
    expect(await http.holderBalance('A'.repeat(64), keys.address)).toBe('0')
    expect(await http.hasClaimed(keys.address, 'B'.repeat(64))).toBe(false)
  })

  test('history comes back as blocks, not as entries describing them', async () => {
    const http = connect()
    const keys = await keyPairFromSeed(randomSeed())
    const faucet = await faucetAccount()

    await http.faucet(keys.address, (5n * 10n ** 18n).toString())
    // The mock builds a fresh ledger per test, so its faucet chain is two
    // blocks and `limit: 10` reaches the open. A node's is however many times
    // it has ever paid out, so the limit has to clear the whole chain for
    // `at(-1)` to mean "the oldest block" rather than "the tenth newest".
    const history = await http.accountHistory(faucet, { limit: 10_000 })
    expect(history.length).toBeGreaterThan(1)

    const newest = history[0]!
    expect(newest.type).toBe('state')
    expect(newest.account).toBe(faucet)
    expect((newest as { subtype?: string }).subtype).toBe('send')
    for (const field of ['previous', 'representative', 'balance', 'link', 'signature', 'work'] as const) {
      expect(typeof (newest as unknown as Record<string, unknown>)[field]).toBe('string')
    }

    expect((history.at(-1) as { subtype?: string }).subtype).toBe('open')
  })

  // The mock refuses this call, because it has no second shape to serve and
  // would rather hold callers to the parameter than infer it. The node does
  // have one — §15 keeps `account_history` answering Nano's inherited shape
  // when `shape` is absent, so that nothing inherited moves. So the two
  // deliberately differ here, and what is worth checking against the node is
  // that the inherited answer is still the inherited answer: entries that
  // describe blocks rather than blocks themselves.
  test('account_history without a shape gives the inherited answer, not a block', async () => {
    const faucet = await faucetAccount()
    const response = await rpc({ action: 'account_history', account: faucet, count: '10' })
    const body = (await response.json()) as { history?: Array<Record<string, unknown>> }
    expect(Array.isArray(body.history)).toBe(true)
    const newest = body.history![0]!
    // Nano's entry puts the subtype in `type` and carries no signature; a block
    // says "state" and has one. This is exactly the collision §15 describes.
    expect(newest.type).not.toBe('state')
    expect(newest.signature).toBeUndefined()
  })

  test('work thresholds come across as decimal strings', async () => {
    const http = connect()
    const thresholds = await http.workThresholds()
    for (const tier of ['A', 'B', 'C'] as const) {
      expect(thresholds[tier]).toMatch(/^[0-9]+$/)
    }
    // §11: A is the hardest tier and C the cheapest, and a work threshold is
    // an upper bound on the output, so the numbers run the other way.
    expect(BigInt(thresholds.A)).toBeGreaterThan(BigInt(thresholds.B))
    expect(BigInt(thresholds.B)).toBeGreaterThan(BigInt(thresholds.C))
  })

  test('a rejected block is a sentence, not a status code', async () => {
    const http = connect()
    const keys = await keyPairFromSeed(randomSeed())
    const unsigned = {
      type: 'state',
      subtype: 'send',
      account: keys.address,
      previous: ZERO_HASH,
      representative: keys.address,
      balance: '0',
      link: ZERO_HASH,
      work: '0000000000000000',
      signature: '0'.repeat(128),
    } as unknown as Block

    await expect(http.process(unsigned)).rejects.toThrow(/rejected "process"/)
  })

  test('an action the node does not have says so, and points at the list', async () => {
    const response = await rpc({ action: 'definitely_not_an_action' })
    expect(response.status).toBe(200)
    expect((await response.json()) as { error?: string }).toHaveProperty('error')
  })

  test('a browser can reach it — preflight and origin', async () => {
    const preflight = await fetch(NODE_URL, { method: 'OPTIONS' })
    expect(preflight.status).toBeLessThan(400)
    expect(preflight.headers.get('access-control-allow-origin')).toBe('*')

    const answered = await rpc({ action: 'work_thresholds' })
    expect(answered.headers.get('access-control-allow-origin')).toBe('*')
  })

  test('subscribe polls receivables, so an arrival is noticed without a socket', async () => {
    const http = connect()
    const keys = await keyPairFromSeed(randomSeed())

    const seen: string[] = []
    const stop = http.subscribe(keys.address, (event) => seen.push(event.hash))
    const { hash } = await http.faucet(keys.address)

    await Bun.sleep(400)
    stop()
    expect(seen).toContain(hash)
  })
})

describe('an issuer and a player who share only a URL', () => {
  test('the full loop: issue, top up, mint, transfer, and an item', async () => {
    const game = await Kei.server({ seed: randomSeed(), node: connect() })
    const player = await Kei.start({ seed: randomSeed(), node: connect() })

    await game.faucet(2_000)
    const coins = await game.token.issue({
      name: 'Coins',
      symbol: 'COIN',
      decimals: 0,
      maxSupply: 1_000_000,
      transfer: 'open',
      swap: 'one-way',
      rate: 1_000,
    })
    expect(coins.symbol).toBe('COIN')

    const paid = new Promise<void>((resolve) => {
      const stop = game.onPayment(async ({ from, amount }) => {
        await coins.mint(from, amount * 1_000)
        stop()
        resolve()
      })
    })
    await player.faucet(1)
    await player.pay({ to: game.address, amount: 0.05, memo: 'starter pack' })
    await paid

    await player.sync()
    expect(await coins.balanceOf(player.address)).toBe(50)

    const held = await player.token('COIN', game.address)
    expect(await held.balance()).toBe(50)

    await held.transfer(game.address, 20)
    await game.sync()
    expect(await coins.balanceOf(game.address)).toBe(20)
    expect(await coins.balanceOf(player.address)).toBe(30)

    const badge = await game.items.create({ name: 'First Press', description: 'You pressed it.' })
    await game.items.mint(badge.id, player.address)
    await player.sync()
    expect(await player.items.owner(badge.id)).toBe(player.address)
    expect((await player.items.ownedBy()).map((item) => item.name)).toContain('First Press')

    game.close()
    player.close()
  }, 120_000)

  test('a batch of drops is one issuer block and many player claims', async () => {
    const game = await Kei.server({ seed: randomSeed(), node: connect() })
    const players = await Promise.all([
      Kei.start({ seed: randomSeed(), node: connect() }),
      Kei.start({ seed: randomSeed(), node: connect() }),
      Kei.start({ seed: randomSeed(), node: connect() }),
    ])

    await game.faucet(2_000)
    const coins = await game.token.issue({ name: 'Coins', symbol: 'COIN', decimals: 0 })

    const drop = await coins.commit(players.map((player, index) => ({ to: player.address, amount: 10 * (index + 1) })))
    expect((await game.client.node.commitInfo(drop.root))?.count).toBe(3)

    await Promise.all(players.map((player) => player.claims.add(drop.proofFor(player.address))))

    expect(await coins.balanceOf(players[0]!.address)).toBe(10)
    expect(await coins.balanceOf(players[1]!.address)).toBe(20)
    expect(await coins.balanceOf(players[2]!.address)).toBe(30)

    await expect(players[0]!.claims.claim(drop.proofFor(players[0]!.address))).rejects.toThrow()

    game.close()
    for (const player of players) player.close()
  }, 120_000)
})
