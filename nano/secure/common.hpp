#pragma once

#include <nano/crypto/blake2/blake2.h>
#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/epoch.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/rep_weights.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/utility.hpp>

#include <boost/iterator/transform_iterator.hpp>
#include <boost/optional/optional.hpp>
#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/variant/variant.hpp>

#include <array>
#include <unordered_map>

namespace boost
{
template <>
struct hash<::nano::uint256_union>
{
	size_t operator() (::nano::uint256_union const & value_a) const
	{
		return std::hash<::nano::uint256_union> () (value_a);
	}
};

template <>
struct hash<::nano::block_hash>
{
	size_t operator() (::nano::block_hash const & value_a) const
	{
		return std::hash<::nano::block_hash> () (value_a);
	}
};

template <>
struct hash<::nano::hash_or_account>
{
	size_t operator() (::nano::hash_or_account const & data_a) const
	{
		return std::hash<::nano::hash_or_account> () (data_a);
	}
};

template <>
struct hash<::nano::public_key>
{
	size_t operator() (::nano::public_key const & value_a) const
	{
		return std::hash<::nano::public_key> () (value_a);
	}
};
template <>
struct hash<::nano::uint512_union>
{
	size_t operator() (::nano::uint512_union const & value_a) const
	{
		return std::hash<::nano::uint512_union> () (value_a);
	}
};
template <>
struct hash<::nano::qualified_root>
{
	size_t operator() (::nano::qualified_root const & value_a) const
	{
		return std::hash<::nano::qualified_root> () (value_a);
	}
};
template <>
struct hash<::nano::root>
{
	size_t operator() (::nano::root const & value_a) const
	{
		return std::hash<::nano::root> () (value_a);
	}
};
}
namespace nano
{
/**
 * A key pair. The private key is generated from the random pool, or passed in
 * as a hex string. The public key is derived using ed25519.
 */
class keypair
{
public:
	keypair ();
	keypair (std::string const &);
	keypair (nano::raw_key &&);
	nano::public_key pub;
	nano::raw_key prv;
};

/**
 * Latest information about an account
 */
class account_info final
{
public:
	account_info () = default;
	account_info (nano::block_hash const &, nano::account const &, nano::block_hash const &, nano::amount const &, nano::seconds_t modified, uint64_t, epoch);
	bool deserialize (nano::stream &);
	bool operator== (nano::account_info const &) const;
	bool operator!= (nano::account_info const &) const;
	size_t db_size () const;
	nano::epoch epoch () const;
	nano::block_hash head{ 0 };
	nano::account representative{};
	nano::block_hash open_block{ 0 };
	nano::amount balance{ 0 };
	/** Seconds since posix epoch */
	nano::seconds_t modified{ 0 };
	uint64_t block_count{ 0 };
	nano::epoch epoch_m{ nano::epoch::epoch_0 };
};

/**
 * Information on an uncollected send
 */
class pending_info final
{
public:
	pending_info () = default;
	pending_info (nano::account const &, nano::amount const &, nano::epoch);
	size_t db_size () const;
	bool deserialize (nano::stream &);
	bool operator== (nano::pending_info const &) const;
	nano::account source{};
	nano::amount amount{ 0 };
	nano::epoch epoch{ nano::epoch::epoch_0 };
};
class pending_key final
{
public:
	pending_key () = default;
	pending_key (nano::account const &, nano::block_hash const &);
	bool deserialize (nano::stream &);
	bool operator== (nano::pending_key const &) const;
	nano::account const & key () const;
	nano::account account{};
	nano::block_hash hash{ 0 };
};

/**
 * The per-account ceiling on distinct assets held (SPEC §7). It cannot be
 * weaponised — §5.6.3 means only the account itself can add to its own
 * holdings — but unbounded per-account state in consensus code is how nodes run
 * out of memory.
 */
size_t constexpr max_assets_per_account = 1024;

/**
 * A key into one of the two asset tables (decisions-m2.md §9, SPEC §7).
 *
 * The same facts are indexed both ways — `holdings` keyed (account, asset_id)
 * and `holders` keyed (asset_id, account) — so one type serves both, and the
 * `holding_key`/`holder_key` factories are what make the ordering explicit at
 * the call site. Both orderings are prefix-scannable, which is the whole point:
 * `ownedBy(account)` and `owner(itemId)` are each one range scan.
 */
class asset_key final
{
public:
	asset_key () = default;
	asset_key (nano::uint256_union const &, nano::uint256_union const &);
	bool deserialize (nano::stream &);
	bool operator== (nano::asset_key const &) const;
	nano::uint256_union const & key () const;
	nano::uint256_union first{ 0 };
	nano::uint256_union second{ 0 };
};
nano::asset_key holding_key (nano::account const &, nano::uint256_union const & asset_id);
nano::asset_key holder_key (nano::uint256_union const & asset_id, nano::account const &);
/**
 * The double-claim index, keyed (account, root) exactly as SPEC §5.5 requires:
 * keyed by account the record is partitioned with the account that made it and
 * prunes alongside that account's chain, where a root-keyed set would be global,
 * grow forever, and belong to nobody.
 */
nano::asset_key claim_key (nano::account const &, nano::uint256_union const & root);
/**
 * The rollback index, keyed (root, account), which is the same pair the other
 * way round and is *not* the double-claim index.
 *
 * SPEC §5.5 settles how a claim is looked up; it does not discuss what happens
 * when the commit block underneath one loses a fork. Rolling that block back
 * means undoing every claim written against it, on chains this node cannot
 * enumerate from the commit alone — the same problem `pending` solves for a send
 * whose receive already exists, solved the same way. Nothing reads it during
 * validation, and it prunes with the root (decisions-m4.md §4).
 */
nano::asset_key claim_root_key (nano::uint256_union const & root, nano::account const &);

/**
 * A token's immutable parameters plus the one thing about it that moves.
 *
 * Everything but `circulating` is fixed at issuance (SPEC §5.6.1): the record is
 * written once by the `issue` block and thereafter only its circulating supply
 * changes, as mints and burns move it.
 */
class asset_info final
{
public:
	asset_info () = default;
	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);
	bool operator== (nano::asset_info const &) const;
	/** True when maxSupply is uncapped, which is stored as zero (SPEC §5.6.6). */
	bool uncapped () const;

	nano::account issuer{};
	std::string name;
	std::string symbol;
	uint8_t decimals{ 0 };
	nano::amount max_supply{ 0 };
	nano::transfer_policy transfer{ nano::transfer_policy::open };
	nano::swap_policy swap{ nano::swap_policy::off };
	std::string description;
	std::string image;
	nano::asset_kind kind{ nano::asset_kind::unspecified };
	/** Capped by max_supply, so burning frees headroom (SPEC §5.6.6). */
	nano::amount circulating{ 0 };
};

/**
 * An uncollected asset arrival — the asset-side twin of `pending_info`.
 *
 * A mint or a transfer writes one of these and touches nothing of the
 * recipient's (SPEC §5.6.3), so junk minted to a million addresses stays the
 * sender's storage problem rather than becoming permanent per-account state.
 */
class asset_pending_info final
{
public:
	asset_pending_info () = default;
	asset_pending_info (nano::account const &, nano::uint256_union const &, nano::amount const &, std::string const &);
	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);
	bool operator== (nano::asset_pending_info const &) const;
	nano::account source{};
	nano::uint256_union asset_id{ 0 };
	nano::amount amount{ 0 };
	/** Carried through so the recipient sees what the sender labelled it (§8). */
	std::string memo;
};

/**
 * A published Merkle root, and the one thing about it that moves.
 *
 * One issuer block underwrites an unbounded number of player claims (SPEC §5.5),
 * so this record is what those claims are checked against: who published it,
 * which asset it pays, and whether it is still open. `count` and `total` are
 * what the issuer declared the drop covers — the node verifies one claimant's
 * leaf and never enumerates the others, so they are read by wallets rather than
 * by consensus, and they are signed so an issuer cannot restate them later.
 */
class asset_commit_info final
{
public:
	asset_commit_info () = default;
	asset_commit_info (nano::account const &, nano::uint256_union const &, uint32_t, nano::amount const &, nano::block_hash const &, bool = false);
	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);
	bool operator== (nano::asset_commit_info const &) const;

	nano::account issuer{};
	nano::uint256_union asset_id{ 0 };
	uint32_t count{ 0 };
	nano::amount total{ 0 };
	/** The `commit` block that published it, so a reader can find the drop. */
	nano::block_hash block{ 0 };
	/** Set by the issuer's `commit_close`; there is no clock (SPEC §5.5). */
	bool closed{ false };
};

class endpoint_key final
{
public:
	endpoint_key () = default;

	/*
	 * @param address_a This should be in network byte order
	 * @param port_a This should be in host byte order
	 */
	endpoint_key (std::array<uint8_t, 16> const & address_a, uint16_t port_a);

	/*
	 * @return The ipv6 address in network byte order
	 */
	std::array<uint8_t, 16> const & address_bytes () const;

	/*
	 * @return The port in host byte order
	 */
	uint16_t port () const;

private:
	// Both stored internally in network byte order
	std::array<uint8_t, 16> address;
	uint16_t network_port{ 0 };
};

enum class no_value
{
	dummy
};

class unchecked_key final
{
public:
	unchecked_key () = default;
	explicit unchecked_key (nano::hash_or_account const & dependency);
	unchecked_key (nano::hash_or_account const &, nano::block_hash const &);
	unchecked_key (nano::uint512_union const &);
	bool deserialize (nano::stream &);
	bool operator== (nano::unchecked_key const &) const;
	bool operator< (nano::unchecked_key const &) const;
	nano::block_hash const & key () const;
	nano::block_hash previous{ 0 };
	nano::block_hash hash{ 0 };
};

/**
 * Information on an unchecked block
 */
class unchecked_info final
{
public:
	unchecked_info () = default;
	unchecked_info (std::shared_ptr<nano::block> const &);
	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);
	nano::seconds_t modified () const;
	std::shared_ptr<nano::block> block;

private:
	/** Seconds since posix epoch */
	uint64_t modified_m{ 0 };
};

class block_info final
{
public:
	block_info () = default;
	block_info (nano::account const &, nano::amount const &);
	nano::account account{};
	nano::amount balance{ 0 };
};

class confirmation_height_info final
{
public:
	confirmation_height_info () = default;
	confirmation_height_info (uint64_t, nano::block_hash const &);

	void serialize (nano::stream &) const;
	bool deserialize (nano::stream &);

	/** height of the cemented frontier */
	uint64_t height{};

	/** hash of the highest cemented block, the cemented/confirmed frontier */
	nano::block_hash frontier{};
};

namespace confirmation_height
{
	/** When the uncemented count (block count - cemented count) is less than this use the unbounded processor */
	uint64_t const unbounded_cutoff{ 16384 };
}

using vote_blocks_vec_iter = std::vector<nano::block_hash>::const_iterator;
class iterate_vote_blocks_as_hash final
{
public:
	iterate_vote_blocks_as_hash () = default;
	nano::block_hash operator() (nano::block_hash const & item) const;
};
class vote final
{
public:
	vote () = default;
	vote (nano::vote const &);
	vote (bool &, nano::stream &);
	vote (nano::account const &, nano::raw_key const &, nano::millis_t timestamp, uint8_t duration, std::vector<nano::block_hash> const &);
	std::string hashes_string () const;
	nano::block_hash hash () const;
	nano::block_hash full_hash () const;
	bool operator== (nano::vote const &) const;
	bool operator!= (nano::vote const &) const;
	void serialize (nano::stream &) const;
	void serialize_json (boost::property_tree::ptree & tree) const;
	/**
	 * Deserializes a vote from the bytes in `stream'
	 * Returns true if there was an error
	 */
	bool deserialize (nano::stream &);
	bool validate () const;
	boost::transform_iterator<nano::iterate_vote_blocks_as_hash, nano::vote_blocks_vec_iter> begin () const;
	boost::transform_iterator<nano::iterate_vote_blocks_as_hash, nano::vote_blocks_vec_iter> end () const;
	std::string to_json () const;
	uint64_t timestamp () const;
	uint8_t duration_bits () const;
	std::chrono::milliseconds duration () const;
	static uint64_t constexpr timestamp_mask = { 0xffff'ffff'ffff'fff0ULL };
	static nano::seconds_t constexpr timestamp_max = { 0xffff'ffff'ffff'fff0ULL };
	static uint64_t constexpr timestamp_min = { 0x0000'0000'0000'0010ULL };
	static uint8_t constexpr duration_max = { 0x0fu };

private:
	// Vote timestamp
	uint64_t timestamp_m;

public:
	// The hashes for which this vote directly covers
	std::vector<nano::block_hash> hashes;
	// Account that's voting
	nano::account account;
	// Signature of timestamp + block hashes
	nano::signature signature;
	static std::string const hash_prefix;

private:
	uint64_t packed_timestamp (uint64_t timestamp, uint8_t duration) const;
};
/**
 * This class serves to find and return unique variants of a vote in order to minimize memory usage
 */
class vote_uniquer final
{
public:
	using value_type = std::pair<nano::block_hash const, std::weak_ptr<nano::vote>>;

	vote_uniquer (nano::block_uniquer &);
	std::shared_ptr<nano::vote> unique (std::shared_ptr<nano::vote> const &);
	size_t size ();

private:
	nano::block_uniquer & uniquer;
	nano::mutex mutex{ mutex_identifier (mutexes::vote_uniquer) };
	std::unordered_map<std::remove_const_t<value_type::first_type>, value_type::second_type> votes;
	static unsigned constexpr cleanup_count = 2;
};

std::unique_ptr<container_info_component> collect_container_info (vote_uniquer & vote_uniquer, std::string const & name);

enum class vote_code
{
	invalid, // Vote is not signed correctly
	replay, // Vote does not have the highest timestamp, it's a replay
	vote, // Vote has the highest timestamp
	indeterminate // Unknown if replay or vote
};

enum class process_result
{
	progress, // Hasn't been seen before, signed correctly
	bad_signature, // Signature was bad, forged or transmission error
	old, // Already seen and was valid
	negative_spend, // Malicious attempt to spend a negative amount
	fork, // Malicious fork based on previous
	unreceivable, // Source block doesn't exist, has already been received, or requires an account upgrade (epoch blocks)
	gap_previous, // Block marked as previous is unknown
	gap_source, // Block marked as source is unknown
	gap_epoch_open_pending, // Block marked as pending blocks required for epoch open block are unknown
	opened_burn_account, // Block attempts to open the burn account
	balance_mismatch, // Balance and amount delta don't match
	representative_mismatch, // Representative is changed when it is not allowed
	block_position, // This block cannot follow the previous block
	insufficient_work, // Insufficient work for this block, even though it passed the minimal validation
	// Asset operations (decisions-m2.md §7). Each names a distinct thing the
	// signer did wrong, because "invalid block" is not an answer a game
	// developer can act on.
	no_such_asset, // The block names an asset that was never issued
	asset_exists, // (issuer, symbol) already names an asset; ids are derived, so re-issuing is a no-op
	not_issuer, // Only the issuer may mint its own asset
	over_max_supply, // Minting this would exceed the asset's circulating supply cap
	transfer_not_permitted, // The asset's immutable transfer policy forbids this move
	insufficient_asset_balance, // The signer does not hold what they tried to move
	issuance_burn_mismatch, // An issue block must burn exactly n Kei, for the account's nth asset (SPEC §5.6.5)
	asset_balance_mismatch, // A non-issue asset block must carry the Kei balance unchanged
	bad_asset_payload, // Malformed issuance parameters: name, decimals, max supply, policy
	too_many_assets, // The account already holds the §7 maximum of 1,024 distinct assets
	reserve_representative, // A reserve account must name the null representative (SPEC §5.7)
	reserve_locked, // Reserve Kei moves only through a passed on-chain vote (SPEC §5.7)
	commit_exists, // That root is already published; a fresh salt makes a fresh root (SPEC §5.5)
	no_such_commit, // Nothing to claim from or close: no commit block published this root
	commit_closed, // The issuer closed this root, and closing is final (SPEC §5.5)
	already_claimed, // One leaf per account per root, claimed once (SPEC §5.5)
	bad_claim_proof // The proof does not put this account in this root for this amount
};
class process_return final
{
public:
	nano::process_result code;
};
enum class tally_result
{
	vote,
	changed,
	confirm
};

nano::stat::detail to_stat_detail (process_result);
/**
 * The RPC error a rejection maps to. Each asset code carries a message naming
 * what the signer did wrong, which is the whole point of having twelve of them
 * rather than one.
 */
nano::error_process to_error_process (process_result);

class network_params;

/**
 * One circulating allocation created by the genesis ceremony. `send` is on
 * the reserve chain and `open` collects it into the named account. Both are
 * fixed blocks: store initialisation installs the ceremony as immutable
 * history, before ordinary reserve locking begins.
 */
struct genesis_allocation
{
	std::shared_ptr<nano::state_block> send;
	std::shared_ptr<nano::state_block> open;
	nano::amount amount;
};

/** Genesis keys and ledger constants for network variants */
class ledger_constants
{
public:
	ledger_constants (nano::work_thresholds & work, nano::networks network_a);
	nano::work_thresholds & work;
	nano::keypair zero_key;
	nano::account nano_beta_account;
	nano::account nano_live_account;
	nano::account nano_test_account;
	std::shared_ptr<nano::block> nano_dev_genesis;
	std::shared_ptr<nano::block> nano_beta_genesis;
	std::shared_ptr<nano::block> nano_live_genesis;
	std::shared_ptr<nano::block> nano_test_genesis;
	std::shared_ptr<nano::block> genesis;
	nano::uint128_t genesis_amount;
	nano::account burn_account;
	nano::account nano_dev_final_votes_canary_account;
	nano::account nano_beta_final_votes_canary_account;
	nano::account nano_live_final_votes_canary_account;
	nano::account nano_test_final_votes_canary_account;
	nano::account final_votes_canary_account;
	uint64_t nano_dev_final_votes_canary_height;
	uint64_t nano_beta_final_votes_canary_height;
	uint64_t nano_live_final_votes_canary_height;
	uint64_t nano_test_final_votes_canary_height;
	uint64_t final_votes_canary_height;
	nano::epochs epochs;

	/**
	 * The reserve accounts, as a fixed and immutable set (SPEC §5.7). Membership
	 * is a cheap test the node cannot get wrong, which is what SPEC means by
	 * refusing "a convention wearing a protocol's clothes".
	 *
	 * Dev contains the singleton reserve account named by its reproducible test
	 * genesis. Beta/live remain empty and unstartable until their offline
	 * ceremony fills the public address and signed blocks: the reserve is 90%
	 * of all valuable Kei, its custody is multisig, and its seed never ships
	 * (decisions-m2.md §5).
	 */
	std::vector<nano::account> reserve_accounts;
	bool is_reserve (nano::account const &) const;
	/**
	 * The four ceremony sends and matching opens. Dev carries deterministic,
	 * publicly reproducible test blocks; beta/live remain empty until their
	 * offline ceremony output replaces the hard-failing placeholder genesis.
	 */
	std::vector<nano::genesis_allocation> genesis_allocations;

	/**
	 * SPEC §5.7's allocation, in whole Kei. The four circulating allocations
	 * must sum to exactly 100,000,000,000 and the whole to 1,000,000,000,000; a
	 * mismatch is a launch blocker, so `validate_allocation` returns an error
	 * and the node refuses to start rather than logging a warning.
	 */
	// Functions rather than static data members, and deliberately so: a
	// nano::uint128_t needs dynamic initialisation, `nano::dev::network_params`
	// is a global whose constructor calls validate_allocation, and it is
	// defined earlier in the same translation unit. As data members these read
	// back as zero during that constructor, the allocation check failed against
	// a set of zeroes, and every node aborted at startup on an assertion whose
	// message said the allocation was wrong when it was not.
	static nano::uint128_t allocation_reserve ();
	static nano::uint128_t allocation_grants ();
	static nano::uint128_t allocation_community ();
	static nano::uint128_t allocation_bounty ();
	static nano::uint128_t allocation_team ();
	/** Returns true, and fills `message_a`, if the allocation does not add up. */
	static bool validate_allocation (std::string & message_a);
};

namespace dev
{
	extern nano::keypair genesis_key;
	// Published dev-only allocation keys. They fund local tests and must never
	// be copied to beta/live, whose ceremony keys are generated offline.
	extern nano::keypair grants_key;
	extern nano::keypair community_key;
	extern nano::keypair bounty_key;
	extern nano::keypair team_key;
	extern nano::network_params network_params;
	extern nano::ledger_constants & constants;
	extern std::shared_ptr<nano::block> & genesis;
}

/** Constants which depend on random values (always used as singleton) */
class hardened_constants
{
public:
	static hardened_constants & get ();

	nano::account not_an_account;
	nano::uint128_union random_128;

private:
	hardened_constants ();
};

/** Node related constants whose value depends on the active network */
class node_constants
{
public:
	node_constants (nano::network_constants & network_constants);
	std::chrono::minutes backup_interval;
	std::chrono::seconds search_pending_interval;
	std::chrono::minutes unchecked_cleaning_interval;
	std::chrono::milliseconds process_confirmed_interval;

	/** The maximum amount of samples for a 2 week period on live or 1 day on beta */
	uint64_t max_weight_samples;
	uint64_t weight_period;
};

/** Voting related constants whose value depends on the active network */
class voting_constants
{
public:
	voting_constants (nano::network_constants & network_constants);
	size_t const max_cache;
	std::chrono::seconds const delay;
};

/** Port-mapping related constants whose value depends on the active network */
class portmapping_constants
{
public:
	portmapping_constants (nano::network_constants & network_constants);
	// Timeouts are primes so they infrequently happen at the same time
	std::chrono::seconds lease_duration;
	std::chrono::seconds health_check_period;
};

/** Bootstrap related constants whose value depends on the active network */
class bootstrap_constants
{
public:
	bootstrap_constants (nano::network_constants & network_constants);
	uint32_t lazy_max_pull_blocks;
	uint32_t lazy_min_pull_blocks;
	unsigned frontier_retry_limit;
	unsigned lazy_retry_limit;
	unsigned lazy_destinations_retry_limit;
	std::chrono::milliseconds gap_cache_bootstrap_start_interval;
	uint32_t default_frontiers_age_seconds;
};

/** Constants whose value depends on the active network */
class network_params
{
public:
	/** Populate values based on \p network_a */
	network_params (nano::networks network_a);

	unsigned kdf_work;
	nano::work_thresholds work;
	nano::network_constants network;
	nano::ledger_constants ledger;
	nano::voting_constants voting;
	nano::node_constants node;
	nano::portmapping_constants portmapping;
	nano::bootstrap_constants bootstrap;
};

enum class confirmation_height_mode
{
	automatic,
	unbounded,
	bounded
};

/* Holds flags for various cacheable data. For most CLI operations caching is unnecessary
 * (e.g getting the cemented block count) so it can be disabled for performance reasons. */
class generate_cache
{
public:
	bool reps = true;
	bool cemented_count = true;
	bool unchecked_count = true;
	bool account_count = true;
	bool block_count = true;

	void enable_all ();
};

/* Holds an in-memory cache of various counts */
class ledger_cache
{
public:
	nano::rep_weights rep_weights;
	std::atomic<uint64_t> cemented_count{ 0 };
	std::atomic<uint64_t> block_count{ 0 };
	std::atomic<uint64_t> pruned_count{ 0 };
	std::atomic<uint64_t> account_count{ 0 };
	std::atomic<bool> final_votes_confirmation_canary{ false };
};

/* Defines the possible states for an election to stop in */
enum class election_status_type : uint8_t
{
	ongoing = 0,
	active_confirmed_quorum = 1,
	active_confirmation_height = 2,
	inactive_confirmation_height = 3,
	stopped = 5
};

/* Holds a summary of an election */
class election_status final
{
public:
	std::shared_ptr<nano::block> winner;
	nano::amount tally;
	nano::amount final_tally;
	std::chrono::milliseconds election_end;
	std::chrono::milliseconds election_duration;
	unsigned confirmation_request_count;
	unsigned block_count;
	unsigned voter_count;
	election_status_type type;
};

nano::wallet_id random_wallet_id ();
}
