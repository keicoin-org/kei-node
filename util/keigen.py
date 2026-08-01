"""ed25519-blake2b, nano block hashing, address encoding, and dev-threshold work.

Written to generate Kei's genesis blocks. It is only trustworthy if it can
reproduce a block whose signature is already known, so `verify` does exactly
that against the inherited dev genesis before anything is generated.
"""
import hashlib
import json

# ---------------------------------------------------------------- ed25519-blake2b
# Standard ed25519 with SHA-512 replaced by BLAKE2b-512, which is what nano uses.
q = 2 ** 255 - 19
l = 2 ** 252 + 27742317777372353535851937790883648493


def H(m):
    return hashlib.blake2b(m).digest()


def inv(x):
    return pow(x, q - 2, q)


d = -121665 * inv(121666) % q
I = pow(2, (q - 1) // 4, q)


def xrecover(y):
    xx = (y * y - 1) * inv(d * y * y + 1)
    x = pow(xx, (q + 3) // 8, q)
    if (x * x - xx) % q != 0:
        x = (x * I) % q
    if x % 2 != 0:
        x = q - x
    return x


By = 4 * inv(5) % q
Bx = xrecover(By)
B = (Bx % q, By % q, 1, (Bx * By) % q)
ident = (0, 1, 1, 0)


def edwards_add(P, Q):
    (x1, y1, z1, t1) = P
    (x2, y2, z2, t2) = Q
    a = (y1 - x1) * (y2 - x2) % q
    b = (y1 + x1) * (y2 + x2) % q
    c = t1 * 2 * d * t2 % q
    dd = z1 * 2 * z2 % q
    e = b - a
    f = dd - c
    g = dd + c
    h = b + a
    return (e * f % q, g * h % q, f * g % q, e * h % q)


def edwards_double(P):
    (x1, y1, z1, _) = P
    a = x1 * x1 % q
    b = y1 * y1 % q
    c = 2 * z1 * z1 % q
    e = ((x1 + y1) * (x1 + y1) - a - b) % q
    g = -a + b
    f = g - c
    h = -a - b
    return (e * f % q, g * h % q, f * g % q, e * h % q)


def scalarmult(P, e):
    if e == 0:
        return ident
    Q = scalarmult(P, e // 2)
    Q = edwards_double(Q)
    if e & 1:
        Q = edwards_add(Q, P)
    return Q


def encodepoint(P):
    (x, y, z, _) = P
    zi = inv(z)
    x = x * zi % q
    y = y * zi % q
    bits = [(y >> i) & 1 for i in range(255)] + [x & 1]
    return bytes(sum(bits[i * 8 + j] << j for j in range(8)) for i in range(32))


def publickey(sk):
    h = H(sk)
    a = 2 ** 254 + sum(2 ** i * ((h[i // 8] >> (i % 8)) & 1) for i in range(3, 254))
    return encodepoint(scalarmult(B, a))


def Hint(m):
    h = H(m)
    return sum(2 ** i * ((h[i // 8] >> (i % 8)) & 1) for i in range(512))


def signature(m, sk, pk):
    h = H(sk)
    a = 2 ** 254 + sum(2 ** i * ((h[i // 8] >> (i % 8)) & 1) for i in range(3, 254))
    r = Hint(bytes(h[32:64]) + m)
    R = scalarmult(B, r)
    S = (r + Hint(encodepoint(R) + pk + m) * a) % l
    return encodepoint(R) + bytes(sum(((S >> (i * 8 + j)) & 1) << j for j in range(8)) for i in range(32))


# ------------------------------------------------------------------- addresses
ALPHABET = "13456789abcdefghijkmnopqrstuwxyz"


def encode_account(pubkey, prefix="kei_"):
    # 52 base32 characters cover 260 bits: the 256 key bits with four zero pad
    # bits above them, so the key is encoded as-is rather than shifted.
    value = int.from_bytes(pubkey, "big")
    account = ""
    for _ in range(52):
        account = ALPHABET[value & 31] + account
        value >>= 5
    checksum = hashlib.blake2b(pubkey, digest_size=5).digest()[::-1]
    cvalue = int.from_bytes(checksum, "big")
    check = ""
    for _ in range(8):
        check = ALPHABET[cvalue & 31] + check
        cvalue >>= 5
    return prefix + account + check


# ---------------------------------------------------------------------- blocks
def open_hash(source, representative, account, domain=None):
    h = hashlib.blake2b(digest_size=32)
    if domain is not None:
        h.update(domain)
        h.update((4).to_bytes(32, "big"))  # block_type::open
    h.update(source)
    h.update(representative)
    h.update(account)
    return h.digest()


def work_value(root, nonce_le):
    h = hashlib.blake2b(digest_size=8)
    h.update(nonce_le)
    h.update(root)
    return int.from_bytes(h.digest(), "little")


def mine(root, threshold):
    # Start from zero so the checked-in dev ceremony is reproducible byte for
    # byte. Work is not part of a block hash or signature, but deterministic
    # work makes auditing generated literals substantially easier.
    nonce_value = 0
    while True:
        nonce = nonce_value.to_bytes(8, "little")
        if work_value(root, nonce) >= threshold:
            # Nano prints work as the big-endian hex of the little-endian nonce.
            return nonce[::-1].hex()
        nonce_value += 1


def verify():
    """Reproduce the inherited dev genesis from its private key, or fail loudly."""
    sk = bytes.fromhex("34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4")
    expect_pk = "B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0"
    expect_sig = ("ECDA914373A2F0CA1296475BAEE40500A7F0A7AD72A5A80C81D7FAB7F6C802B2"
                  "CC7DB50F5DD0FB25B2EF11761FA7344A158DD5A700B21BD47DE5BD0F63153A02")
    pk = publickey(sk)
    assert pk.hex().upper() == expect_pk, f"public key mismatch: {pk.hex().upper()}"
    h = open_hash(pk, pk, pk)
    sig = signature(h, sk, pk).hex().upper()
    assert sig == expect_sig, f"signature mismatch:\n got {sig}\n want {expect_sig}"
    print("verify: reproduced the inherited dev genesis public key and signature")
    print("verify: address under kei_ prefix is", encode_account(pk))


# ------------------------------------------------------------------ generation
KEI_DOMAIN = hashlib.blake2b(b"kei-block-v1", digest_size=32).digest()


def gen_dev():
    """Kei's dev genesis.

    The private key is derived from a documented, published phrase rather than
    from randomness, so anyone can regenerate it and confirm this block is what
    it claims to be. It is a test key by construction and holds nothing.
    """
    sk = hashlib.blake2b(b"kei-dev-genesis", digest_size=32).digest()
    pk = publickey(sk)
    h = open_hash(pk, pk, pk, domain=KEI_DOMAIN)
    sig = signature(h, sk, pk).hex().upper()
    work = mine(pk, 0xFE00000000000000)  # publish_dev.epoch_1
    return {
        "private": sk.hex().upper(),
        "public": pk.hex().upper(),
        "account": encode_account(pk),
        "hash": h.hex().upper(),
        "signature": sig,
        "work": work,
    }


def state_hash(account, previous, representative, balance, link):
    h = hashlib.blake2b(digest_size=32)
    h.update(KEI_DOMAIN)
    h.update((6).to_bytes(32, "big"))  # block_type::state
    h.update(account)
    h.update(previous)
    h.update(representative)
    h.update(balance.to_bytes(16, "big"))
    h.update(link)
    return h.digest()


def signed_state(keys, previous, representative, balance, link, threshold):
    block_hash = state_hash(keys["public"], previous, representative, balance, link)
    root = previous if previous != bytes(32) else keys["public"]
    return {
        "type": "state",
        "account": encode_account(keys["public"]),
        "previous": previous.hex().upper(),
        "representative": encode_account(representative),
        "balance": str(balance),
        "link": link.hex().upper(),
        "link_as_account": encode_account(link),
        "signature": signature(block_hash, keys["private"], keys["public"]).hex().upper(),
        "work": mine(root, threshold),
        "hash": block_hash.hex().upper(),
    }


def phrase_key(phrase):
    private = hashlib.blake2b(phrase.encode(), digest_size=32).digest()
    return {"private": private, "public": publickey(private)}


def gen_dev_ceremony():
    """The complete deterministic M2 allocation for the valueless dev chain."""
    reserve = phrase_key("kei-dev-genesis")
    allocations = [
        ("grants", 37_000_000_000, phrase_key("kei-dev-grants")),
        ("community", 28_000_000_000, phrase_key("kei-dev-community")),
        ("bounty", 18_000_000_000, phrase_key("kei-dev-bounty")),
        ("team", 17_000_000_000, phrase_key("kei-dev-team")),
    ]
    total = 1_000_000_000_000 * 10**18
    zero = bytes(32)

    genesis_hash = open_hash(reserve["public"], zero, reserve["public"], domain=KEI_DOMAIN)
    genesis = {
        "type": "open",
        "source": reserve["public"].hex().upper(),
        "representative": encode_account(zero),
        "account": encode_account(reserve["public"]),
        "work": mine(reserve["public"], 0xFE00000000000000),
        "signature": signature(genesis_hash, reserve["private"], reserve["public"]).hex().upper(),
        "hash": genesis_hash.hex().upper(),
    }

    previous = genesis_hash
    balance = total
    entries = []
    for role, kei, recipient in allocations:
        amount = kei * 10**18
        balance -= amount
        send = signed_state(
            reserve, previous, zero, balance, recipient["public"], 0xFFC0000000000000
        )
        open_block = signed_state(
            recipient, zero, recipient["public"], amount, bytes.fromhex(send["hash"]),
            0xF000000000000000,
        )
        entries.append({
            "role": role,
            "private": recipient["private"].hex().upper(),
            "public": recipient["public"].hex().upper(),
            "account": encode_account(recipient["public"]),
            "amount": str(amount),
            "send": send,
            "open": open_block,
        })
        previous = bytes.fromhex(send["hash"])

    assert balance == 900_000_000_000 * 10**18
    return {
        "reserve_private": reserve["private"].hex().upper(),
        "reserve_public": reserve["public"].hex().upper(),
        "reserve_account": encode_account(reserve["public"]),
        "genesis": genesis,
        "allocations": entries,
    }


if __name__ == "__main__":
    verify()
    print(json.dumps(gen_dev_ceremony(), indent=2))
