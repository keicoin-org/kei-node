"""Known-answer vectors for the Kei block hash (decisions-m2.md §7, §14).

The node and the SDK each compute block hashes from their own code, in C++ and
in TypeScript. "They agree" is a claim that neither of them can check, and until
something checks it a signature made by one is not verifiable by the other —
definition-of-done (6).

This prints fixed inputs and their hashes. `core_test` asserts them on the node
side and `packages/core/test/wire.test.ts` asserts them on the SDK side, so the
two are pinned to the same bytes by a third statement of the layout rather than
to each other by agreement.

    python util/keihash.py

The layout mirrored here is `nano/lib/blocks.cpp`: everything big-endian except
the two length prefixes, which are little-endian.
"""

import hashlib

from keigen import encode_account

KEI_DOMAIN = hashlib.blake2b(b"kei-block-v1", digest_size=32).digest()

BLOCK_TYPE_STATE = 6
BLOCK_TYPE_ASSET = 7

ASSET_OP_ISSUE = 0
ASSET_OP_TRANSFER = 3


def preamble(block_type):
    return KEI_DOMAIN + block_type.to_bytes(32, "big")


def payload_string(text):
    encoded = text.encode("utf-8")
    return len(encoded).to_bytes(2, "little") + encoded


def derive_asset_id(issuer_pubkey, symbol):
    """H(issuer_pubkey ‖ symbol) — derived, never assigned (SPEC §5.6.1)."""
    h = hashlib.blake2b(digest_size=32)
    h.update(issuer_pubkey)
    h.update(symbol.encode("utf-8"))
    return h.digest()


def state_hash(account, previous, representative, balance, link):
    h = hashlib.blake2b(digest_size=32)
    h.update(preamble(BLOCK_TYPE_STATE))
    h.update(account)
    h.update(previous)
    h.update(representative)
    h.update(balance.to_bytes(16, "big"))
    h.update(link)
    return h.digest()


def asset_hash(account, previous, representative, balance, op, asset_id, amount, link, payload):
    h = hashlib.blake2b(digest_size=32)
    h.update(preamble(BLOCK_TYPE_ASSET))
    h.update(account)
    h.update(previous)
    h.update(representative)
    h.update(balance.to_bytes(16, "big"))
    h.update(bytes([op]))
    h.update(asset_id)
    h.update(amount.to_bytes(16, "big"))
    h.update(link)
    h.update(len(payload).to_bytes(2, "little"))
    h.update(payload)
    return h.digest()


# Fixed inputs. The account is the inherited dev genesis public key, which
# keigen.verify() already reproduces from its private key, so it is a real key
# rather than a pattern that could hide a byte-order mistake.
ACCOUNT = bytes.fromhex("B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0")
DESTINATION = bytes.fromhex("F5C6E4A1B2938475869708172635445362718293A4B5C6D7E8F901234567890A")
PREVIOUS = bytes.fromhex("00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF")
BALANCE = 1234567890123456789012345678
AMOUNT = 42000000000000000000


def issue_payload():
    return (
        payload_string("Test Gem")
        + payload_string("GEM")
        + bytes([2])  # decimals
        + (1000000).to_bytes(16, "big")  # max supply
        + bytes([1])  # transfer: issuer-only
        + bytes([0])  # swap: two-way
        + payload_string("A gem for testing")
        + payload_string("QmTestCid")
        + bytes([2])  # kind: item
    )


def main():
    print("account       ", encode_account(ACCOUNT))
    print("destination   ", encode_account(DESTINATION))
    print("previous      ", PREVIOUS.hex().upper())
    print("balance       ", BALANCE)
    print("amount        ", AMOUNT)
    print()

    send = state_hash(ACCOUNT, PREVIOUS, ACCOUNT, BALANCE, DESTINATION)
    print("state send    ", send.hex().upper())

    asset_id = derive_asset_id(ACCOUNT, "GEM")
    print("asset id      ", asset_id.hex().upper())

    issue = asset_hash(
        ACCOUNT, PREVIOUS, ACCOUNT, BALANCE,
        ASSET_OP_ISSUE, asset_id, 0, bytes(32), issue_payload(),
    )
    print("asset issue   ", issue.hex().upper())

    transfer = asset_hash(
        ACCOUNT, PREVIOUS, ACCOUNT, BALANCE,
        ASSET_OP_TRANSFER, asset_id, AMOUNT, DESTINATION, payload_string("thanks"),
    )
    print("asset transfer", transfer.hex().upper())


if __name__ == "__main__":
    main()
