#pragma once

#include <nano/crypto/blake2/blake2.h>
#include <nano/lib/epoch.hpp>
#include <nano/lib/errors.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/optional_ptr.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work.hpp>

#include <boost/optional.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include <unordered_map>
#include <vector>

namespace nano
{
class block_visitor;
class mutable_block_visitor;
enum class block_type : uint8_t
{
	invalid = 0,
	not_a_block = 1,
	send = 2,
	receive = 3,
	open = 4,
	change = 5,
	state = 6,
	// Kei's native token primitive (decisions-m2.md §7). Every inherited type
	// keeps its number and its meaning, per decisions-m2.md §2.
	asset = 7
};
// The eleven asset operations this node validates. M2 shipped the first five
// (decisions-m2.md §0, §7, §11) and reserved the next three by number, because
// the op byte is hashed and renumbering later invalidates every vector and
// signature made against the earlier guess. M4 activated them at exactly the
// reserved numbers (decisions-m2.md §18, decisions-m4.md §2), and M5 spends
// 8, 9 and 10 on the swap legs in SPEC §5.6.4's own order (decisions-m5.md §1).
// Nothing above them is reserved, so `asset_op_valid` bounds the enum and a
// number past its end is rejected on the wire rather than parsed.
enum class asset_op : uint8_t
{
	issue = 0,
	mint = 1,
	burn = 2,
	transfer = 3,
	asset_receive = 4,
	commit = 5,
	commit_close = 6,
	claim = 7,
	swap_offer = 8,
	swap_accept = 9,
	swap_cancel = 10
};
/**
 * The domain separator every Kei block hash begins with, so that no Nano or
 * Banano block can ever be a valid Kei block or the reverse. See the definition
 * for why transport-layer separation alone is not enough.
 */
nano::uint256_union const & kei_block_domain ();
/** Writes the Kei domain followed by the block type, in that order. */
void hash_preamble (blake2b_state &, nano::block_type);
/** Domain-separated ORV root for both consumers of one swap offer. */
nano::root swap_election_root (nano::block_hash const & offer_hash);

bool asset_op_valid (uint8_t);
char const * asset_op_to_string (nano::asset_op);
bool asset_op_from_string (std::string const &, nano::asset_op &);

/** Who may move units. Protocol-enforced and immutable once issued (SPEC §5.4). */
enum class transfer_policy : uint8_t
{
	open = 0,
	issuer_only = 1,
	none = 2
};
/** Whether the issuer's own SDK runs a swap desk. Stored, never enforced (SPEC §5.4). */
enum class swap_policy : uint8_t
{
	two_way = 0,
	one_way = 1,
	off = 2
};
/**
 * An SDK-level hint so a wallet can tell a currency from a sword. The protocol
 * does not know or care — an item is a token with supply 1 and 0 decimals.
 */
enum class asset_kind : uint8_t
{
	unspecified = 0,
	token = 1,
	item = 2
};
char const * transfer_policy_to_string (nano::transfer_policy);
char const * swap_policy_to_string (nano::swap_policy);
/** The empty string for `unspecified`, which the RPC omits rather than emits. */
char const * asset_kind_to_string (nano::asset_kind);

/**
 * The op-specific tail of an asset block, and the block's one variable-length
 * field (decisions-m2.md §7).
 *
 * §7 originally said the payload carried issuance metadata only. It carries the
 * memo too, because §8 puts memos on the asset block and a `transfer` is where
 * a memo is worth having — the shop in the Button demo correlates an order to
 * an arrival, and doing that by amount and timing is racy. The payload is
 * therefore op-keyed: issuance metadata for `issue`, a memo for `mint` and
 * `transfer`, and empty for `burn` and `asset_receive`.
 *
 * The encoding is canonical, and it is what gets hashed — there is deliberately
 * no separate "raw bytes" representation to keep in step with the parsed one.
 */
class asset_payload final
{
public:
	bool operator== (nano::asset_payload const &) const;
	/** Write the canonical encoding for `op`. Writes nothing for an op that carries none. */
	void serialize (nano::stream &, nano::asset_op) const;
	/** Read the canonical encoding for `op`, and fail if it does not consume `size` exactly. */
	bool deserialize (nano::stream &, nano::asset_op, std::size_t size);
	/** The canonical encoding as bytes, which is what the block hash covers. */
	std::vector<uint8_t> to_bytes (nano::asset_op) const;

	// `issue` only.
	std::string name;
	std::string symbol;
	uint8_t decimals{ 0 };
	/** Raw units. Zero means uncapped, which is why a cap of zero is rejected. */
	nano::amount max_supply{ 0 };
	nano::transfer_policy transfer{ nano::transfer_policy::open };
	nano::swap_policy swap{ nano::swap_policy::off };
	std::string description;
	/** An IPFS CID. The chain stores the pointer; the asset lives on IPFS (SPEC §7). */
	std::string image;
	nano::asset_kind kind{ nano::asset_kind::unspecified };

	// `mint` and `transfer` only (decisions-m2.md §8).
	std::string memo;

	// `commit` only: how many recipients the root covers. Informational — the
	// node verifies one claimant's leaf and never enumerates the others — but
	// it is signed, so an issuer cannot restate the size of a drop afterwards
	// (decisions-m4.md §2).
	uint32_t count{ 0 };
	// `claim` only: the sibling hashes from the claimant's leaf to the root.
	std::vector<nano::uint256_union> proof;

	// `swap_offer` only: what the offerer wants back for the asset the fixed
	// header locks. The offered side reuses `asset_id` and `amount`, and the
	// optional counterparty reuses `link`, so the wanted side is the only part
	// of an offer with nowhere in the header to live (decisions-m5.md §2).
	nano::uint256_union want_asset{ 0 };
	nano::amount want_amount{ 0 };
	/**
	 * Advisory wall-clock seconds, and never consensus-enforced (SPEC §9.3).
	 * A block-lattice has no clock, so this cannot bind anything; it lets
	 * clients hide stale listings and lets the SDK auto-cancel the player's
	 * own. Zero means the offer states no expiry at all.
	 */
	uint64_t expires_at{ 0 };

	// Bounds, enforced here so a block that cannot be stored cannot be parsed
	// either. The ledger repeats the ones that carry a user-facing message.
	static std::size_t constexpr max_name = 64;
	static std::size_t constexpr max_symbol = 20;
	static std::size_t constexpr max_description = 256;
	static std::size_t constexpr max_image = 128;
	static std::size_t constexpr max_memo = 128;
	/** 2^48 leaves is more than any legitimate drop, and it bounds the payload. */
	static std::size_t constexpr max_proof = 48;
};

/**
 * Asset identity is derived, not assigned (SPEC §5.6.1) — blake2b-256 over the
 * issuer's public key and the normalised symbol. That is what makes issuance
 * idempotent structurally rather than by lookup, and it is why nothing can race
 * an issuance.
 */
nano::uint256_union derive_asset_id (nano::public_key const &, std::string const & symbol);
/** Uppercased and trimmed. Returns false if the symbol is not 1-20 of [A-Z0-9-]. */
bool normalize_symbol (std::string const &, std::string &);

/**
 * One entitlement, hashed: this account, this asset, this amount, and nothing
 * else (decisions-m4.md §3).
 *
 * SPEC §5.5 settles that a root commits to at most one leaf per recipient, so
 * the recipient is the leaf's identity and the double-claim index needs no leaf
 * index to key on. The leaf carries the asset id as well as the amount because a
 * claim states both, and a leaf that bound only the amount would let a proof cut
 * from one asset's drop be replayed against another's.
 */
nano::uint256_union asset_claim_leaf (nano::account const &, nano::uint256_union const & asset_id, nano::amount const &);
/**
 * Fold a leaf up through its siblings and return the root it implies.
 *
 * Interior pairs are ordered by value, so a proof is siblings alone with no
 * direction bits to encode, disagree about, or forge. That trick is only safe
 * when a leaf hash can never be read as an interior hash, which is what the two
 * distinct domain separators in the implementation buy — without them a
 * 64-byte "leaf" could be presented as an interior node and prove a membership
 * the issuer never committed to.
 */
nano::uint256_union asset_claim_root (nano::uint256_union const & leaf, std::vector<nano::uint256_union> const & proof);
class block_details
{
	static_assert (std::is_same<std::underlying_type<nano::epoch>::type, uint8_t> (), "Epoch enum is not the proper type");
	static_assert (static_cast<uint8_t> (nano::epoch::max) < (1 << 5), "Epoch max is too large for the sideband");

public:
	block_details () = default;
	block_details (nano::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a);
	static constexpr size_t size ()
	{
		return 1;
	}
	bool operator== (block_details const & other_a) const;
	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);
	nano::epoch epoch{ nano::epoch::epoch_0 };
	bool is_send{ false };
	bool is_receive{ false };
	bool is_epoch{ false };

private:
	uint8_t packed () const;
	void unpack (uint8_t);
};

std::string state_subtype (nano::block_details const);

class block_sideband final
{
public:
	block_sideband () = default;
	block_sideband (nano::account const &, nano::block_hash const &, nano::amount const &, uint64_t const, nano::seconds_t const local_timestamp, nano::block_details const &, nano::epoch const source_epoch_a);
	block_sideband (nano::account const &, nano::block_hash const &, nano::amount const &, uint64_t const, nano::seconds_t const local_timestamp, nano::epoch const epoch_a, bool const is_send, bool const is_receive, bool const is_epoch, nano::epoch const source_epoch_a);
	void serialize (nano::stream &, nano::block_type) const;
	bool deserialize (nano::stream &, nano::block_type);
	static size_t size (nano::block_type);
	nano::block_hash successor{ 0 };
	nano::account account{};
	nano::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
	nano::block_details details;
	nano::epoch source_epoch{ nano::epoch::epoch_0 };
};
class block
{
public:
	// Return a digest of the hashables in this block.
	nano::block_hash const & hash () const;
	// Return a digest of hashables and non-hashables in this block.
	nano::block_hash full_hash () const;
	nano::block_sideband const & sideband () const;
	void sideband_set (nano::block_sideband const &);
	bool has_sideband () const;
	std::string to_json () const;
	virtual void hash (blake2b_state &) const = 0;
	virtual uint64_t block_work () const = 0;
	virtual void block_work_set (uint64_t) = 0;
	virtual nano::account const & account () const;
	// Previous block in account's chain, zero for open block
	virtual nano::block_hash const & previous () const = 0;
	// Source block for open/receive blocks, zero otherwise.
	virtual nano::block_hash const & source () const;
	// Destination account for send blocks, zero otherwise.
	virtual nano::account const & destination () const;
	// Previous block or account number for open blocks
	virtual nano::root const & root () const = 0;
	// Qualified root value based on previous() and root()
	virtual nano::qualified_root qualified_root () const;
	// Consensus-conflict identity. These are the chain root/qualified root for
	// ordinary blocks. Swap consumers override them with a domain-separated
	// hash of the offer so different account chains share one ORV election.
	virtual nano::root election_root () const;
	virtual nano::qualified_root election_qualified_root () const;
	// Link field for state blocks, zero otherwise.
	virtual nano::link const & link () const;
	virtual nano::account const & representative () const;
	virtual nano::amount const & balance () const;
	virtual void serialize (nano::stream &) const = 0;
	virtual void serialize_json (std::string &, bool = false) const = 0;
	virtual void serialize_json (boost::property_tree::ptree &) const = 0;
	virtual void visit (nano::block_visitor &) const = 0;
	virtual void visit (nano::mutable_block_visitor &) = 0;
	virtual bool operator== (nano::block const &) const = 0;
	virtual nano::block_type type () const = 0;
	virtual nano::signature const & block_signature () const = 0;
	virtual void signature_set (nano::signature const &) = 0;
	virtual ~block () = default;
	virtual bool valid_predecessor (nano::block const &) const = 0;
	static size_t size (nano::block_type);
	virtual nano::work_version work_version () const;
	// If there are any changes to the hashables, call this to update the cached hash
	void refresh ();

protected:
	mutable nano::block_hash cached_hash{ 0 };
	/**
	 * Contextual details about a block, some fields may or may not be set depending on block type.
	 * This field is set via sideband_set in ledger processing or deserializing blocks from the database.
	 * Otherwise it may be null (for example, an old block or fork).
	 */
	nano::optional_ptr<nano::block_sideband> sideband_m;

private:
	nano::block_hash generate_hash () const;
};

using block_list_t = std::vector<std::shared_ptr<nano::block>>;

class send_hashables
{
public:
	send_hashables () = default;
	send_hashables (nano::block_hash const &, nano::account const &, nano::amount const &);
	send_hashables (bool &, nano::stream &);
	send_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	nano::block_hash previous;
	nano::account destination;
	nano::amount balance;
	static std::size_t constexpr size = sizeof (previous) + sizeof (destination) + sizeof (balance);
};
class send_block : public nano::block
{
public:
	send_block () = default;
	send_block (nano::block_hash const &, nano::account const &, nano::amount const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	send_block (bool &, nano::stream &);
	send_block (bool &, boost::property_tree::ptree const &);
	virtual ~send_block () = default;
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::account const & destination () const override;
	nano::root const & root () const override;
	nano::amount const & balance () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::send_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	send_hashables hashables;
	nano::signature signature;
	uint64_t work;
	static std::size_t constexpr size = nano::send_hashables::size + sizeof (signature) + sizeof (work);
};
class receive_hashables
{
public:
	receive_hashables () = default;
	receive_hashables (nano::block_hash const &, nano::block_hash const &);
	receive_hashables (bool &, nano::stream &);
	receive_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	nano::block_hash previous;
	nano::block_hash source;
	static std::size_t constexpr size = sizeof (previous) + sizeof (source);
};
class receive_block : public nano::block
{
public:
	receive_block () = default;
	receive_block (nano::block_hash const &, nano::block_hash const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	receive_block (bool &, nano::stream &);
	receive_block (bool &, boost::property_tree::ptree const &);
	virtual ~receive_block () = default;
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::block_hash const & source () const override;
	nano::root const & root () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::receive_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	receive_hashables hashables;
	nano::signature signature;
	uint64_t work;
	static std::size_t constexpr size = nano::receive_hashables::size + sizeof (signature) + sizeof (work);
};
class open_hashables
{
public:
	open_hashables () = default;
	open_hashables (nano::block_hash const &, nano::account const &, nano::account const &);
	open_hashables (bool &, nano::stream &);
	open_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	nano::block_hash source;
	nano::account representative;
	nano::account account;
	static std::size_t constexpr size = sizeof (source) + sizeof (representative) + sizeof (account);
};
class open_block : public nano::block
{
public:
	open_block () = default;
	open_block (nano::block_hash const &, nano::account const &, nano::account const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	open_block (nano::block_hash const &, nano::account const &, nano::account const &, std::nullptr_t);
	open_block (bool &, nano::stream &);
	open_block (bool &, boost::property_tree::ptree const &);
	virtual ~open_block () = default;
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::account const & account () const override;
	nano::block_hash const & source () const override;
	nano::root const & root () const override;
	nano::account const & representative () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::open_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	nano::open_hashables hashables;
	nano::signature signature;
	uint64_t work;
	static std::size_t constexpr size = nano::open_hashables::size + sizeof (signature) + sizeof (work);
};
class change_hashables
{
public:
	change_hashables () = default;
	change_hashables (nano::block_hash const &, nano::account const &);
	change_hashables (bool &, nano::stream &);
	change_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	nano::block_hash previous;
	nano::account representative;
	static std::size_t constexpr size = sizeof (previous) + sizeof (representative);
};
class change_block : public nano::block
{
public:
	change_block () = default;
	change_block (nano::block_hash const &, nano::account const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	change_block (bool &, nano::stream &);
	change_block (bool &, boost::property_tree::ptree const &);
	virtual ~change_block () = default;
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::root const & root () const override;
	nano::account const & representative () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::change_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	nano::change_hashables hashables;
	nano::signature signature;
	uint64_t work;
	static std::size_t constexpr size = nano::change_hashables::size + sizeof (signature) + sizeof (work);
};
class state_hashables
{
public:
	state_hashables () = default;
	state_hashables (nano::account const &, nano::block_hash const &, nano::account const &, nano::amount const &, nano::link const &);
	state_hashables (bool &, nano::stream &);
	state_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	// Account# / public key that operates this account
	// Uses:
	// Bulk signature validation in advance of further ledger processing
	// Arranging uncomitted transactions by account
	nano::account account;
	// Previous transaction in this chain
	nano::block_hash previous;
	// Representative of this account
	nano::account representative;
	// Current balance of this account
	// Allows lookup of account balance simply by looking at the head block
	nano::amount balance;
	// Link field contains source block_hash if receiving, destination account if sending
	nano::link link;
	// Serialized size
	static std::size_t constexpr size = sizeof (account) + sizeof (previous) + sizeof (representative) + sizeof (balance) + sizeof (link);
};
class state_block : public nano::block
{
public:
	state_block () = default;
	state_block (nano::account const &, nano::block_hash const &, nano::account const &, nano::amount const &, nano::link const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	state_block (bool &, nano::stream &);
	state_block (bool &, boost::property_tree::ptree const &);
	virtual ~state_block () = default;
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::account const & account () const override;
	nano::root const & root () const override;
	nano::link const & link () const override;
	nano::account const & representative () const override;
	nano::amount const & balance () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::state_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	nano::state_hashables hashables;
	nano::signature signature;
	uint64_t work;
	static std::size_t constexpr size = nano::state_hashables::size + sizeof (signature) + sizeof (work);
};
class asset_hashables
{
public:
	asset_hashables () = default;
	asset_hashables (nano::account const &, nano::block_hash const &, nano::account const &, nano::amount const &, nano::asset_op, nano::uint256_union const &, nano::amount const &, nano::link const &, nano::asset_payload const &);
	asset_hashables (bool &, nano::stream &);
	asset_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	/** Read the SDK's nested `op` object into the flat fields of the §7 layout. */
	bool deserialize_op_json (boost::property_tree::ptree const &);
	/** Write those fields back out in the shape `process` accepts. */
	void serialize_op_json (boost::property_tree::ptree &) const;
	// Signer. One chain per account (§5.6.1), same as every inherited block type.
	nano::account account;
	// Links into the account's one chain.
	nano::block_hash previous;
	// Carried, so an asset block never silently changes delegation.
	nano::account representative;
	// The account's Kei balance, unchanged from its predecessor — except on
	// `issue`, which burns Kei (decisions-m2.md §7, §12), and where this
	// field is how the burn is expressed.
	nano::amount balance;
	// issue | mint | burn | transfer | asset_receive
	nano::asset_op op;
	// H(issuer_pubkey ‖ symbol) — derived, never assigned (§5.6.1).
	nano::uint256_union asset_id;
	// Units, in the asset's own decimals. Zero for `issue`.
	nano::amount amount;
	// Counterparty account, or the source block hash for `asset_receive`.
	nano::link link;
	// The op-specific tail: issuance metadata for `issue`, a memo for `mint`
	// and `transfer`, empty otherwise. The one variable-length field in an
	// otherwise fixed-size layout — see decisions-m2.md §7.
	nano::asset_payload payload;
	// The fixed-size portion only. payload_len (2 bytes) plus payload itself
	// follow this and are not part of this constant — see serialize().
	static std::size_t constexpr size = sizeof (account) + sizeof (previous) + sizeof (representative) + sizeof (balance) + sizeof (op) + sizeof (asset_id) + sizeof (amount) + sizeof (link);
};
class asset_block : public nano::block
{
public:
	asset_block () = default;
	asset_block (nano::account const &, nano::block_hash const &, nano::account const &, nano::amount const &, nano::asset_op, nano::uint256_union const &, nano::amount const &, nano::link const &, nano::asset_payload const &, nano::raw_key const &, nano::public_key const &, uint64_t);
	asset_block (bool &, nano::stream &);
	asset_block (bool &, boost::property_tree::ptree const &);
	virtual ~asset_block () = default;
	static constexpr std::size_t serialized_length_field_offset = sizeof (nano::account) + sizeof (nano::block_hash) + sizeof (nano::account) + sizeof (nano::amount) + sizeof (nano::asset_op) + sizeof (nano::uint256_union) + sizeof (nano::amount) + sizeof (nano::link);
	static constexpr std::size_t payload_length_bytes = sizeof (uint16_t);
	static constexpr std::size_t serialized_prefix_size = serialized_length_field_offset + payload_length_bytes;
	static constexpr std::size_t serialized_suffix_size = sizeof (nano::signature) + sizeof (uint64_t);
	static constexpr std::size_t serialized_minimum_size () { return serialized_prefix_size + serialized_suffix_size; }
	static constexpr std::size_t serialized_size (std::size_t payload_size) { return serialized_prefix_size + payload_size + serialized_suffix_size; }
	using nano::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	nano::block_hash const & previous () const override;
	nano::account const & account () const override;
	nano::root const & root () const override;
	nano::root election_root () const override;
	nano::qualified_root election_qualified_root () const override;
	nano::link const & link () const override;
	nano::account const & representative () const override;
	nano::amount const & balance () const override;
	void serialize (nano::stream &) const override;
	bool deserialize (nano::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (nano::block_visitor &) const override;
	void visit (nano::mutable_block_visitor &) override;
	nano::block_type type () const override;
	nano::signature const & block_signature () const override;
	void signature_set (nano::signature const &) override;
	bool operator== (nano::block const &) const override;
	bool operator== (nano::asset_block const &) const;
	bool valid_predecessor (nano::block const &) const override;
	nano::asset_hashables hashables;
	nano::signature signature;
	uint64_t work;
	// Unlike send/receive/open/change/state, the payload is variable length
	// (decisions-m2.md §7). Callers that need wire sizes must read the fixed
	// prefix, then the 2-byte payload length before the suffix.
};
class block_visitor
{
public:
	virtual void send_block (nano::send_block const &) = 0;
	virtual void receive_block (nano::receive_block const &) = 0;
	virtual void open_block (nano::open_block const &) = 0;
	virtual void change_block (nano::change_block const &) = 0;
	virtual void state_block (nano::state_block const &) = 0;
	virtual void asset_block (nano::asset_block const &) = 0;
	virtual ~block_visitor () = default;
};
class mutable_block_visitor
{
public:
	virtual void send_block (nano::send_block &) = 0;
	virtual void receive_block (nano::receive_block &) = 0;
	virtual void open_block (nano::open_block &) = 0;
	virtual void change_block (nano::change_block &) = 0;
	virtual void state_block (nano::state_block &) = 0;
	virtual void asset_block (nano::asset_block &) = 0;
	virtual ~mutable_block_visitor () = default;
};
/**
 * This class serves to find and return unique variants of a block in order to minimize memory usage
 */
class block_uniquer
{
public:
	using value_type = std::pair<nano::uint256_union const, std::weak_ptr<nano::block>>;

	std::shared_ptr<nano::block> unique (std::shared_ptr<nano::block> const &);
	size_t size ();

private:
	nano::mutex mutex{ mutex_identifier (mutexes::block_uniquer) };
	std::unordered_map<std::remove_const_t<value_type::first_type>, value_type::second_type> blocks;
	static unsigned constexpr cleanup_count = 2;
};

std::unique_ptr<container_info_component> collect_container_info (block_uniquer & block_uniquer, std::string const & name);

std::shared_ptr<nano::block> deserialize_block (nano::stream &);
std::shared_ptr<nano::block> deserialize_block (nano::stream &, nano::block_type, nano::block_uniquer * = nullptr);
std::shared_ptr<nano::block> deserialize_block_json (boost::property_tree::ptree const &, nano::block_uniquer * = nullptr);
/**
 * Serialize block type as an 8-bit value
 */
void serialize_block_type (nano::stream &, nano::block_type const &);
/**
 * Serialize a block prefixed with an 8-bit typecode
 */
void serialize_block (nano::stream &, nano::block const &);

void block_memory_pool_purge ();
}
