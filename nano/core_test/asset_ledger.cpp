#include <nano/lib/work.hpp>
#include <nano/node/election.hpp>
#include <nano/secure/buffer.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>
#include <nano/test_common/ledger.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <array>
#include <limits>
#include <sstream>

// The five asset operations, taken through nano::ledger::process and back out
// again through nano::ledger::rollback (decisions-m2.md §7, §10, §12).
//
// Everything here had been written and compiled but never executed: no asset
// block had been through ledger_processor, and the `holdings`, `holders` and
// `issued` tables were written by no test. Rollback especially, because its
// bugs stay invisible until a fork actually happens and then cost real
// balances.
//
// The issuer throughout is the funded dev team allocation. The genesis account
// is the reserve and is deliberately unable to issue or otherwise burn Kei.

namespace
{
/** An issuance payload, varying only what a test actually cares about. */
nano::asset_payload issuance (std::string const & symbol_a, nano::transfer_policy transfer_a, nano::uint128_t const & max_supply_a)
{
	nano::asset_payload payload;
	payload.name = "Gems";
	payload.symbol = symbol_a;
	payload.decimals = 0;
	payload.max_supply = nano::amount (max_supply_a);
	payload.transfer = transfer_a;
	payload.swap = nano::swap_policy::one_way;
	payload.kind = nano::asset_kind::token;
	return payload;
}

/**
 * Sign an asset block and find it work at its own tier (§11).
 *
 * Work comes after construction because the hash a signature covers does not
 * include it, and asking for the block's own root is less error-prone than
 * deriving it a second time here.
 */
std::shared_ptr<nano::asset_block> signed_asset (nano::work_pool & pool_a, nano::keypair const & key_a, nano::block_hash const & previous_a, nano::amount const & balance_a, nano::asset_op op_a, nano::uint256_union const & asset_id_a, nano::amount const & amount_a, nano::link const & link_a, nano::asset_payload const & payload_a = nano::asset_payload{})
{
	auto block (std::make_shared<nano::asset_block> (key_a.pub, previous_a, key_a.pub, balance_a, op_a, asset_id_a, amount_a, link_a, payload_a, key_a.prv, key_a.pub, 0));
	block->block_work_set (*pool_a.generate (block->root (), nano::dev::constants.work.threshold_asset (op_a)));
	return block;
}

/** What the team allocation holds once it has paid for its nth asset. */
nano::uint128_t after_issuing (uint64_t count_a)
{
	nano::uint128_t balance (nano::dev::constants.allocation_team ());
	for (uint64_t issued (0); issued < count_a; ++issued)
	{
		balance -= nano::issuance_burn (issued);
	}
	return balance;
}

/** The team allocation issuing one asset — the starting point below. */
struct issued_asset
{
	nano::asset_payload payload;
	nano::uint256_union id;
	std::shared_ptr<nano::asset_block> block;
};

issued_asset issue_one (nano::ledger & ledger_a, nano::store & store_a, nano::work_pool & pool_a, nano::transfer_policy transfer_a, nano::uint128_t const & max_supply_a)
{
	issued_asset issued;
	issued.payload = issuance ("GEM", transfer_a, max_supply_a);
	issued.id = nano::derive_asset_id (nano::dev::team_key.pub, issued.payload.symbol);
	issued.block = signed_asset (pool_a, nano::dev::team_key, nano::dev::constants.genesis_allocations.back ().open->hash (), nano::amount (after_issuing (1)), nano::asset_op::issue, issued.id, 0, 0, issued.payload);
	auto transaction (store_a.tx_begin_write ());
	EXPECT_EQ (nano::process_result::progress, ledger_a.process (transaction, *issued.block).code);
	return issued;
}

/**
 * The tree an issuer's SDK builds, and the proofs it hands out.
 *
 * Interior nodes are folded with the node's own helper rather than a second copy
 * of the hashing rule, so a test proof is built by exactly the code that will
 * later check it. The tree shape — pairs left to right, an odd node promoted —
 * is this builder's business alone: the node folds whatever siblings it is
 * given and never learns what shape produced them.
 */
class drop final
{
public:
	explicit drop (std::vector<nano::uint256_union> const & leaves_a)
	{
		levels.push_back (leaves_a);
		while (levels.back ().size () > 1)
		{
			auto const & below (levels.back ());
			std::vector<nano::uint256_union> above;
			for (std::size_t i (0); i < below.size (); i += 2)
			{
				above.push_back (i + 1 < below.size () ? nano::asset_claim_root (below[i], { below[i + 1] }) : below[i]);
			}
			levels.push_back (above);
		}
	}

	/**
	 * A `block_hash` rather than the `uint256_union` the tree is built from,
	 * because a root travels in the block's `link` and that is the only 32-byte
	 * type `nano::link` will take.
	 */
	nano::block_hash root () const
	{
		return nano::block_hash (levels.back ().front ().number ());
	}

	std::vector<nano::uint256_union> proof (std::size_t index_a) const
	{
		std::vector<nano::uint256_union> result;
		for (std::size_t level (0); level + 1 < levels.size (); ++level, index_a /= 2)
		{
			auto const sibling (index_a ^ 1);
			if (sibling < levels[level].size ())
			{
				result.push_back (levels[level][sibling]);
			}
		}
		return result;
	}

private:
	std::vector<std::vector<nano::uint256_union>> levels;
};

/** A claim payload carrying a proof, which is the only op that has one. */
nano::asset_payload claim_payload (std::vector<nano::uint256_union> const & proof_a)
{
	nano::asset_payload payload;
	payload.proof = proof_a;
	return payload;
}

/** A commit payload, whose only field is the recipient count it declares. */
nano::asset_payload commit_payload (uint32_t count_a)
{
	nano::asset_payload payload;
	payload.count = count_a;
	return payload;
}

/** A `swap_offer` payload: what the offerer wants back, and until when (§9.2, §9.3). */
nano::asset_payload offer_payload (nano::uint256_union const & want_asset_a, nano::uint128_t const & want_amount_a, uint64_t expires_at_a = 0)
{
	nano::asset_payload payload;
	payload.want_asset = want_asset_a;
	payload.want_amount = nano::amount (want_amount_a);
	payload.expires_at = expires_at_a;
	return payload;
}

/** A mint the recipient has already collected, so they hold the asset outright. */
struct funded final
{
	std::shared_ptr<nano::asset_block> mint;
	std::shared_ptr<nano::asset_block> collect;
};

/**
 * Mint `amount_a` of `asset_id_a` to `recipient_a` and have them collect it,
 * off the team allocation's chain at `issuer_previous_a`. Every swap test needs
 * at least one player who already holds something to offer.
 */
funded fund_player (nano::ledger & ledger_a, nano::write_transaction const & transaction_a, nano::work_pool & pool_a, nano::uint256_union const & asset_id_a, nano::block_hash const & issuer_previous_a, nano::keypair const & recipient_a, nano::uint128_t const & amount_a)
{
	funded result;
	result.mint = signed_asset (pool_a, nano::dev::team_key, issuer_previous_a, nano::amount (after_issuing (1)), nano::asset_op::mint, asset_id_a, amount_a, recipient_a.pub);
	EXPECT_EQ (nano::process_result::progress, ledger_a.process (transaction_a, *result.mint).code);
	result.collect = signed_asset (pool_a, recipient_a, 0, 0, nano::asset_op::asset_receive, 0, 0, result.mint->hash ());
	EXPECT_EQ (nano::process_result::progress, ledger_a.process (transaction_a, *result.collect).code);
	return result;
}

/**
 * Add `count_a` rows to an account's `holdings` for assets no block mentions.
 *
 * Written straight to the store rather than driven through blocks because the
 * §7 cap counts rows in that one table and nothing else: `holdings_count` is a
 * prefix scan over `(account, *)`, so a row put here is indistinguishable from
 * one a mint and a collect would have left. Driving 1,023 of them properly
 * would need 1,023 issuances, and issuance burns an escalating amount of Kei
 * (§5.6.5) — the team allocation cannot pay for that many, and the test would
 * be about issuance pricing rather than about the cap.
 *
 * The ids are small integers. They collide with nothing: a real id is
 * H(issuer ‖ symbol), and these are never looked up in `assets` — only counted.
 */
void fill_holdings (nano::store & store_a, nano::write_transaction const & transaction_a, nano::account const & account_a, std::size_t count_a)
{
	for (std::size_t i (0); i < count_a; ++i)
	{
		store_a.asset.balance_put (transaction_a, account_a, nano::uint256_union (static_cast<uint64_t> (i + 1)), nano::amount (1));
	}
}

/** The JSON shape used by RPC must reproduce an accepted signed block exactly. */
void assert_rpc_json_round_trip (nano::asset_block const & block_a)
{
	std::string json;
	block_a.serialize_json (json);
	boost::property_tree::ptree tree;
	std::stringstream input (json);
	boost::property_tree::read_json (input, tree);
	bool error (false);
	nano::asset_block decoded (error, tree);
	ASSERT_FALSE (error);
	ASSERT_EQ (block_a.hash (), decoded.hash ());
	ASSERT_EQ (block_a.signature, decoded.signature);
	ASSERT_FALSE (nano::validate_message (decoded.hashables.account, decoded.hash (), decoded.signature));
}
}

// Every op has to survive the record the block store writes — a type byte, the
// block, then its sideband — because that record is the only way a block ever
// comes back out of the store, and a block that cannot be read cannot be rolled
// back, bootstrapped, or served over RPC.
//
// This is where the empty payload was found. `burn` and `asset_receive` are the
// only two ops whose canonical payload is zero bytes, every other op writes at
// least a length prefix, and an empty payload could not be parsed at all: an
// empty vector's `data ()` is null and boost's direct stream throws over a null
// buffer instead of reporting end-of-stream. Both serialised fine and then
// failed to come back, which nothing noticed until a rollback read one.

// The block processor's entry gate is deliberately only a cheap floor. Asset
// operations have three different thresholds, so ingress must admit tier C and
// leave the exact A/B/C decision to ledger_processor::asset_block.
TEST (asset_ledger, work_tiers_clear_ingress_before_ledger_enforces_the_exact_tier)
{
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair key;
	auto const asset_id (nano::derive_asset_id (key.pub, "GEM"));

	for (auto const op : { nano::asset_op::issue, nano::asset_op::transfer, nano::asset_op::asset_receive })
	{
		auto block (signed_asset (pool, key, 1, 1000, op, asset_id, 5, 9,
			op == nano::asset_op::issue ? issuance ("GEM", nano::transfer_policy::open, 1000) : nano::asset_payload{}));
		SCOPED_TRACE (nano::asset_op_to_string (op));
		ASSERT_FALSE (nano::dev::network_params.work.validate_entry (*block));
	}

	// Work below even tier C is still rejected at ingress.
	auto below_entry (signed_asset (pool, key, 1, 1000, nano::asset_op::asset_receive, asset_id, 5, 9));
	uint64_t insufficient{ 0 };
	while (nano::dev::network_params.work.difficulty (below_entry->work_version (), below_entry->root (), insufficient) >= nano::dev::network_params.work.entry)
	{
		++insufficient;
	}
	below_entry->block_work_set (insufficient);
	ASSERT_TRUE (nano::dev::network_params.work.validate_entry (*below_entry));

	// Clearing ingress is not enough: tier-C work on an issue must still fail
	// the ledger's tier-A rule.
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	auto const payload (issuance ("GEM", nano::transfer_policy::open, 1000));
	auto const team_asset_id (nano::derive_asset_id (nano::dev::team_key.pub, payload.symbol));
	auto issue (signed_asset (pool, nano::dev::team_key, nano::dev::constants.genesis_allocations.back ().open->hash (), nano::amount (after_issuing (1)), nano::asset_op::issue, team_asset_id, 0, 0, payload));

	uint64_t tier_c_only{ 0 };
	auto const tier_c (nano::dev::network_params.work.tier_c ());
	auto const tier_a (nano::dev::network_params.work.tier_a);
	while (true)
	{
		auto const difficulty (nano::dev::network_params.work.difficulty (issue->work_version (), issue->root (), tier_c_only));
		if (difficulty >= tier_c && difficulty < tier_a)
		{
			break;
		}
		++tier_c_only;
	}
	issue->block_work_set (tier_c_only);
	ASSERT_FALSE (nano::dev::network_params.work.validate_entry (*issue));
	ASSERT_EQ (nano::process_result::insufficient_work, ledger.process (store.tx_begin_write (), *issue).code);
}

TEST (asset_ledger, every_op_survives_the_record_the_store_writes)
{
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair key;

	for (auto const op : { nano::asset_op::issue, nano::asset_op::mint, nano::asset_op::burn, nano::asset_op::transfer, nano::asset_op::asset_receive, nano::asset_op::commit, nano::asset_op::commit_close, nano::asset_op::claim, nano::asset_op::swap_offer, nano::asset_op::swap_accept, nano::asset_op::swap_cancel })
	{
		SCOPED_TRACE (nano::asset_op_to_string (op));
		nano::asset_payload payload;
		if (op == nano::asset_op::issue)
		{
			payload = issuance ("GEM", nano::transfer_policy::open, 1000);
		}
		else if (op == nano::asset_op::mint || op == nano::asset_op::transfer)
		{
			payload.memo = "quest reward";
		}
		else if (op == nano::asset_op::commit)
		{
			payload.count = 4096;
		}
		else if (op == nano::asset_op::swap_offer)
		{
			payload = offer_payload (nano::uint256_union (7), 300, 1234567890);
		}
		else if (op == nano::asset_op::claim)
		{
			// A proof deep enough to catch a length prefix that only works for
			// one sibling, and shallow enough to stay a unit test.
			for (uint8_t depth (0); depth < 12; ++depth)
			{
				payload.proof.push_back (nano::uint256_union (depth + 1));
			}
		}
		// An asset block can open an account, so one of these has no predecessor
		// — the shape a player's first collect takes (§10), and the shape a
		// player's first claim takes too (§5.5).
		nano::block_hash const previous (op == nano::asset_op::asset_receive ? 0 : 7);
		auto block (signed_asset (pool, key, previous, 1000, op, nano::derive_asset_id (key.pub, "GEM"), 5, 9, payload));
		block->sideband_set (nano::block_sideband (key.pub, 0, 0, 1, nano::seconds_since_epoch (), nano::block_details (nano::epoch::epoch_0, false, false, false), nano::epoch::epoch_0));

		std::vector<uint8_t> record;
		{
			nano::vectorstream stream (record);
			nano::serialize_block (stream, *block);
			block->sideband ().serialize (stream, block->type ());
		}

		nano::bufferstream stream (record.data (), record.size ());
		nano::block_type type{ nano::block_type::invalid };
		ASSERT_FALSE (nano::try_read (stream, type));
		ASSERT_EQ (nano::block_type::asset, type);
		auto parsed (nano::deserialize_block (stream, type));
		ASSERT_NE (nullptr, parsed);
		ASSERT_TRUE (*block == *parsed);
		nano::block_sideband sideband;
		ASSERT_FALSE (sideband.deserialize (stream, type));
		ASSERT_TRUE (nano::at_end (stream));
	}
}

// SPEC §5.6.5: the nth asset an account issues burns n Kei, and the burn is the
// balance decrease itself — no receivable, because the Kei is destroyed rather
// than moved (§12).
TEST (asset_ledger, issue_burns_one_kei_and_records_the_asset)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));

	auto transaction (store.tx_begin_read ());
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::dev::team_key.pub, asset.issuer);
	ASSERT_EQ ("GEM", asset.symbol);
	ASSERT_EQ (nano::transfer_policy::open, asset.transfer);
	ASSERT_TRUE (asset.circulating.is_zero ());
	ASSERT_EQ (1, store.asset.issued_count (transaction, nano::dev::team_key.pub));

	// One Kei gone, and gone from the supply rather than to anyone.
	ASSERT_EQ (nano::dev::constants.allocation_team () - nano::BAN_ratio, ledger.account_balance (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (nano::dev::constants.allocation_team () - nano::BAN_ratio, ledger.weight (nano::dev::team_key.pub));
	auto const info (ledger.account_info (transaction, nano::dev::team_key.pub));
	ASSERT_TRUE (info);
	ASSERT_EQ (issued.block->hash (), info->head);
	ASSERT_EQ (2, info->block_count);
}

// The escalating half of §5.6.5, through the ledger rather than the arithmetic:
// the same account's second asset costs two Kei, not one.
TEST (asset_ledger, the_second_asset_costs_two_kei)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const first (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));

	auto const payload (issuance ("GOLD", nano::transfer_policy::open, 1000));
	auto const id (nano::derive_asset_id (nano::dev::team_key.pub, payload.symbol));
	// One Kei short of what a second asset costs.
	auto underpaid (signed_asset (pool, nano::dev::team_key, first.block->hash (), nano::amount (after_issuing (1) - nano::BAN_ratio), nano::asset_op::issue, id, 0, 0, payload));
	auto second (signed_asset (pool, nano::dev::team_key, first.block->hash (), nano::amount (after_issuing (2)), nano::asset_op::issue, id, 0, 0, payload));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::issuance_burn_mismatch, ledger.process (transaction, *underpaid).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *second).code);
	}

	auto transaction (store.tx_begin_read ());
	ASSERT_EQ (2, store.asset.issued_count (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (nano::dev::constants.allocation_team () - (nano::BAN_ratio * 3), ledger.account_balance (transaction, nano::dev::team_key.pub));
}

// Identity is derived, never assigned (SPEC §5.6.1), so re-issuing a symbol
// from the same account computes the same id and there is nothing to collide.
TEST (asset_ledger, the_same_symbol_cannot_be_issued_twice)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto again (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (2)), nano::asset_op::issue, issued.id, 0, 0, issued.payload));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::asset_exists, ledger.process (transaction, *again).code);
}

// SPEC §5.6.3: a mint writes nothing to the recipient's state. It creates a
// receivable, which the recipient's own signed block collects — which is what
// makes an attacker's dust-spam their storage problem and not the network's.
TEST (asset_ledger, a_mint_arrives_as_receivable_and_the_holder_collects_it)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	}
	{
		auto transaction (store.tx_begin_read ());
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (500), asset.circulating);
		// Nothing on the recipient's side yet: no holding, and no account.
		ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
		ASSERT_EQ (0, store.asset.holdings_count (transaction, player.pub));
		ASSERT_FALSE (ledger.account_info (transaction, player.pub));
		nano::asset_pending_info pending;
		ASSERT_FALSE (store.asset.pending_get (transaction, nano::pending_key (player.pub, mint->hash ()), pending));
		ASSERT_EQ (nano::dev::team_key.pub, pending.source);
		ASSERT_EQ (issued.id, pending.asset_id);
		ASSERT_EQ (nano::amount (500), pending.amount);
	}

	// An asset block can open an account: a player who has never held Kei can
	// still be minted a token (§10).
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	}

	auto transaction (store.tx_begin_read ());
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_EQ (1, store.asset.holdings_count (transaction, player.pub));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));

	// SPEC §7: the same fact indexed both ways. `holders` is what answers
	// balanceOf in a single lookup, and it is written by nothing else.
	auto holding (store.asset.holdings_begin (transaction, nano::holding_key (player.pub, issued.id)));
	ASSERT_NE (store.asset.holdings_end (), holding);
	ASSERT_TRUE (nano::holding_key (player.pub, issued.id) == holding->first);
	ASSERT_EQ (nano::amount (500), holding->second);
	auto holder (store.asset.holders_begin (transaction, nano::holder_key (issued.id, player.pub)));
	ASSERT_NE (store.asset.holders_end (), holder);
	ASSERT_TRUE (nano::holder_key (issued.id, player.pub) == holder->first);
	ASSERT_EQ (nano::amount (500), holder->second);

	// SPEC §5.6.2: holding 500 of an asset buys no consensus weight, ever.
	// If it did, capturing the chain would cost one issuance and one mint.
	ASSERT_EQ (nano::uint128_t (0), ledger.weight (player.pub));
	ASSERT_TRUE (ledger.account_balance (transaction, player.pub).is_zero ());
}

TEST (asset_ledger, asset_receive_rejects_noncanonical_signed_headers_without_mutation)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	auto const block_count (ledger.cache.block_count.load ());
	auto const stored_block_count (store.block.count (transaction));
	auto const issuer_head (ledger.latest (transaction, nano::dev::team_key.pub));
	auto const issuer_frontier (store.frontier.get (transaction, issuer_head));

	std::array<std::shared_ptr<nano::asset_block>, 2> noncanonical{
		signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()),
		signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 999, mint->hash ())
	};
	for (auto const & rejected : noncanonical)
	{
		SCOPED_TRACE (rejected->hash ().to_string ());
		ASSERT_EQ (nano::process_result::bad_asset_payload, ledger.process (transaction, *rejected).code);
		ASSERT_EQ (block_count, ledger.cache.block_count.load ());
		ASSERT_EQ (stored_block_count, store.block.count (transaction));
		ASSERT_FALSE (ledger.account_info (transaction, player.pub));
		ASSERT_TRUE (store.frontier.get (transaction, rejected->hash ()).is_zero ());
		ASSERT_FALSE (store.block.exists (transaction, rejected->hash ()));
		ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
		ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
		ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (player.pub, rejected->hash ())));
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (500), asset.circulating);
		ASSERT_EQ (issuer_head, ledger.latest (transaction, nano::dev::team_key.pub));
		ASSERT_EQ (issuer_frontier, store.frontier.get (transaction, issuer_head));
	}

	auto canonical (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *canonical).code);
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
	assert_rpc_json_round_trip (*canonical);
}

TEST (asset_ledger, burn_rejects_noncanonical_link_without_mutation)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	auto const & issuer (nano::dev::team_key);
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto mint (signed_asset (pool, issuer, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, issuer.pub));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	auto collect (signed_asset (pool, issuer, mint->hash (), nano::amount (after_issuing (1)), nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	auto rejected (signed_asset (pool, issuer, collect->hash (), nano::amount (after_issuing (1)), nano::asset_op::burn, issued.id, 100, nano::block_hash{ 1 }));
	auto const block_count (ledger.cache.block_count.load ());
	auto const stored_block_count (store.block.count (transaction));
	auto const head (ledger.latest (transaction, issuer.pub));
	auto const frontier (store.frontier.get (transaction, head));
	auto const kei_balance (ledger.account_balance (transaction, issuer.pub));

	ASSERT_EQ (nano::process_result::bad_asset_payload, ledger.process (transaction, *rejected).code);
	ASSERT_EQ (block_count, ledger.cache.block_count.load ());
	ASSERT_EQ (stored_block_count, store.block.count (transaction));
	ASSERT_EQ (head, ledger.latest (transaction, issuer.pub));
	ASSERT_EQ (kei_balance, ledger.account_balance (transaction, issuer.pub));
	ASSERT_EQ (frontier, store.frontier.get (transaction, head));
	ASSERT_TRUE (store.frontier.get (transaction, rejected->hash ()).is_zero ());
	ASSERT_FALSE (store.block.exists (transaction, rejected->hash ()));
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, issuer.pub, issued.id));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (issuer.pub, rejected->hash ())));
	ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (issuer.pub, rejected->hash ())));
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (500), asset.circulating);

	auto canonical (signed_asset (pool, issuer, collect->hash (), nano::amount (after_issuing (1)), nano::asset_op::burn, issued.id, 100, 0));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *canonical).code);
	ASSERT_EQ (nano::amount (400), store.asset.balance (transaction, issuer.pub, issued.id));
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (400), asset.circulating);
	assert_rpc_json_round_trip (*canonical);
}

// The cap is on circulating supply, and it is the node that enforces it.
TEST (asset_ledger, mint_cannot_exceed_max_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 600, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	}
	auto over (signed_asset (pool, nano::dev::team_key, mint->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto exact (signed_asset (pool, nano::dev::team_key, mint->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 400, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *over).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *exact).code);
	}

	auto transaction (store.tx_begin_read ());
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (asset.max_supply, asset.circulating);
}

// The cap is checked with uint128 arithmetic, and uint128 wraps. Comparing
// `circulating + amount` against the cap lets a large enough amount carry past
// 2^128, compare small, and be credited as the wrapped remainder — a mint of
// nearly 2^128 units against a cap of 1000, leaving the supply at zero. The
// checks subtract instead, which cannot wrap.
TEST (asset_ledger, minting_cannot_wrap_a_capped_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const ceiling (std::numeric_limits<nano::uint128_t>::max ());
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto const balance (nano::amount (after_issuing (1)));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), balance, nano::asset_op::mint, issued.id, 600, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	}

	// 600 + (2^128 - 599) is 2^128, which is zero in uint128.
	auto wrapping (signed_asset (pool, nano::dev::team_key, mint->hash (), balance, nano::asset_op::mint, issued.id, nano::amount (ceiling - 599), player.pub));
	auto exact (signed_asset (pool, nano::dev::team_key, mint->hash (), balance, nano::asset_op::mint, issued.id, 400, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *wrapping).code);

		// Rejected means nothing moved: the supply is what it was, the block was
		// never stored, and there is no receivable for anyone to collect.
		nano::asset_info rejected;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, rejected));
		ASSERT_EQ (nano::amount (600), rejected.circulating);
		ASSERT_FALSE (store.block.exists (transaction, wrapping->hash ()));
		ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, wrapping->hash ())));
		auto const issuer (ledger.account_info (transaction, nano::dev::team_key.pub));
		ASSERT_TRUE (issuer);
		ASSERT_EQ (mint->hash (), issuer->head);

		// And the headroom that is actually there is still spendable.
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *exact).code);
	}

	auto transaction (store.tx_begin_read ());
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (1000), asset.circulating);
}

// An uncapped asset has no cap to compare against, which is exactly why the old
// check skipped it: `!uncapped ()` short-circuited and the addition ran
// unguarded. There is no cap but there is still a ceiling, and a mint that
// crosses it must fail rather than roll the supply over to a small number.
TEST (asset_ledger, minting_cannot_wrap_an_uncapped_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const ceiling (std::numeric_limits<nano::uint128_t>::max ());
	// A zero maxSupply is uncapped (SPEC §5.6.6).
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 0));
	auto const balance (nano::amount (after_issuing (1)));
	auto nearly_all (signed_asset (pool, nano::dev::team_key, issued.block->hash (), balance, nano::asset_op::mint, issued.id, nano::amount (ceiling - 10), player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *nearly_all).code);
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_TRUE (asset.uncapped ());
		ASSERT_EQ (nano::amount (ceiling - 10), asset.circulating);
	}

	auto over (signed_asset (pool, nano::dev::team_key, nearly_all->hash (), balance, nano::asset_op::mint, issued.id, 11, player.pub));
	auto exact (signed_asset (pool, nano::dev::team_key, nearly_all->hash (), balance, nano::asset_op::mint, issued.id, 10, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *over).code);

		nano::asset_info rejected;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, rejected));
		ASSERT_EQ (nano::amount (ceiling - 10), rejected.circulating);
		ASSERT_FALSE (store.block.exists (transaction, over->hash ()));
		ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, over->hash ())));

		// The last ten units that fit still fit: the guard is off by nothing.
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *exact).code);
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (ceiling), asset.circulating);

		// With the supply at the ceiling, one more unit has nowhere to go.
		auto beyond (signed_asset (pool, nano::dev::team_key, exact->hash (), balance, nano::asset_op::mint, issued.id, 1, player.pub));
		ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *beyond).code);
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (ceiling), asset.circulating);
	}
}

// SPEC §5.6.6: maxSupply caps what exists at once, so burning frees headroom to
// mint again. The consequence, documented rather than discovered: a burned item
// can be re-minted by its issuer.
TEST (asset_ledger, burning_frees_headroom_to_mint_again)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	auto const & issuer (nano::dev::team_key);
	auto const balance (nano::amount (after_issuing (1)));

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	// The issuer mints to itself, which is still a receivable it has to collect.
	auto mint (signed_asset (pool, issuer, issued.block->hash (), balance, nano::asset_op::mint, issued.id, 1000, issuer.pub));
	auto collect (signed_asset (pool, issuer, mint->hash (), balance, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	auto burn (signed_asset (pool, issuer, collect->hash (), balance, nano::asset_op::burn, issued.id, 400, 0));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *burn).code);
	}
	{
		auto transaction (store.tx_begin_read ());
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (600), asset.circulating);
		ASSERT_EQ (nano::amount (600), store.asset.balance (transaction, issuer.pub, issued.id));
	}

	auto again (signed_asset (pool, issuer, burn->hash (), balance, nano::asset_op::mint, issued.id, 400, issuer.pub));
	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *again).code);
}

// SPEC §5.4: `none` is soulbound. Units cannot move at all once minted; they
// can only be burned. A policy the SDK merely asks developers to respect is not
// a policy, so the node is where this lives.
TEST (asset_ledger, a_soulbound_asset_cannot_be_transferred)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player, other;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::none, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	auto transfer (signed_asset (pool, player, collect->hash (), 0, nano::asset_op::transfer, issued.id, 100, other.pub));
	auto burn (signed_asset (pool, player, collect->hash (), 0, nano::asset_op::burn, issued.id, 100, 0));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	ASSERT_EQ (nano::process_result::transfer_not_permitted, ledger.process (transaction, *transfer).code);
	// The escape hatch a soulbound asset does have.
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *burn).code);
}

// SPEC §5.4: `issuer-only` means units move only to or from the issuer. This is
// how a genuinely internal currency is possible — players earn, spend, and
// return units, but cannot trade with each other.
TEST (asset_ledger, issuer_only_permits_only_the_issuer_side)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player, other;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::issuer_only, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	auto to_player = signed_asset (pool, player, collect->hash (), 0, nano::asset_op::transfer, issued.id, 100, other.pub);
	auto to_issuer = signed_asset (pool, player, collect->hash (), 0, nano::asset_op::transfer, issued.id, 100, nano::dev::team_key.pub);

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	ASSERT_EQ (nano::process_result::transfer_not_permitted, ledger.process (transaction, *to_player).code);
	// Spending it back at the shop is the whole point of the policy.
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *to_issuer).code);
}

// SPEC §5.6.1's concession to §5.6.8: every asset block carries the account's
// Kei balance unchanged, so a Banano-derived explorer that ignores the asset
// payload still tracks Kei correctly instead of reporting a broken balance.
TEST (asset_ledger, an_asset_block_may_not_move_kei)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1) - nano::BAN_ratio), nano::asset_op::mint, issued.id, 500, player.pub));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::asset_balance_mismatch, ledger.process (transaction, *mint).code);
}

// SPEC §7: zero-balance entries are deleted from both tables, never kept at
// zero, so a player's state footprint shrinks when they spend.
TEST (asset_ledger, transferring_everything_deletes_both_entries)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player, other;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	auto transfer (signed_asset (pool, player, collect->hash (), 0, nano::asset_op::transfer, issued.id, 500, other.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *transfer).code);
	}

	auto transaction (store.tx_begin_read ());
	ASSERT_EQ (0, store.asset.holdings_count (transaction, player.pub));
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
	auto holder (store.asset.holders_begin (transaction, nano::holder_key (issued.id, player.pub)));
	ASSERT_TRUE (holder == store.asset.holders_end () || !(nano::holder_key (issued.id, player.pub) == holder->first));
	// It moved rather than vanishing: the recipient has it receivable.
	ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (other.pub, transfer->hash ())));
}

// Rollback has to give back the Kei the issuance destroyed, and take the
// issuance count back down with it — otherwise the account's next asset is
// priced as though this one still existed (§12).
TEST (asset_ledger, rolling_back_an_issue_gives_the_kei_back)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_FALSE (ledger.rollback (transaction, issued.block->hash ()));
	}

	auto transaction (store.tx_begin_read ());
	ASSERT_FALSE (store.asset.exists (transaction, issued.id));
	ASSERT_EQ (0, store.asset.issued_count (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (nano::dev::constants.allocation_team (), ledger.account_balance (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (nano::dev::constants.allocation_team (), ledger.weight (nano::dev::team_key.pub));
	ASSERT_FALSE (store.block.exists (transaction, issued.block->hash ()));
	auto const info (ledger.account_info (transaction, nano::dev::team_key.pub));
	ASSERT_TRUE (info);
	ASSERT_EQ (nano::dev::constants.genesis_allocations.back ().open->hash (), info->head);
	ASSERT_EQ (1, info->block_count);
}

// A mint nobody has collected: the receivable goes, and so does the circulating
// supply it created.
TEST (asset_ledger, rolling_back_a_mint_takes_the_receivable_back)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
		ASSERT_FALSE (ledger.rollback (transaction, mint->hash ()));
	}

	auto transaction (store.tx_begin_read ());
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_TRUE (asset.circulating.is_zero ());
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
	// The asset itself survives: only the mint was rolled back.
	ASSERT_TRUE (store.asset.exists (transaction, issued.id));
	ASSERT_EQ (issued.block->hash (), ledger.latest (transaction, nano::dev::team_key.pub));
}

// The case that is easy to get wrong, and whose bug is invisible until a fork:
// the recipient has already collected the mint, so their `asset_receive` has to
// come off their chain first. The two accounts are separate chains with no
// ordering between them, so nothing about that can be assumed.
TEST (asset_ledger, rolling_back_a_mint_the_holder_already_collected)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
		ASSERT_FALSE (ledger.rollback (transaction, mint->hash ()));
	}

	auto transaction (store.tx_begin_read ());
	// The player's collect was rolled back to reach the mint, so the account the
	// collect opened is gone with it.
	ASSERT_FALSE (store.block.exists (transaction, collect->hash ()));
	ASSERT_FALSE (ledger.account_info (transaction, player.pub));
	ASSERT_EQ (0, store.asset.holdings_count (transaction, player.pub));
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
	// And the receivable is not left behind either.
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_TRUE (asset.circulating.is_zero ());
}

// Rolling back a collect on its own puts the receivable back exactly as it was,
// which means recovering the source account and the memo from the source block.
TEST (asset_ledger, rolling_back_a_collect_makes_it_receivable_again)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	nano::asset_payload payload;
	payload.memo = "quest reward";
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub, payload));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
		ASSERT_FALSE (ledger.rollback (transaction, collect->hash ()));
	}

	auto transaction (store.tx_begin_read ());
	ASSERT_EQ (0, store.asset.holdings_count (transaction, player.pub));
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
	nano::asset_pending_info pending;
	ASSERT_FALSE (store.asset.pending_get (transaction, nano::pending_key (player.pub, mint->hash ()), pending));
	ASSERT_EQ (nano::dev::team_key.pub, pending.source);
	ASSERT_EQ (issued.id, pending.asset_id);
	ASSERT_EQ (nano::amount (500), pending.amount);
	ASSERT_EQ ("quest reward", pending.memo);
	// The mint is untouched, so the supply it created is still there.
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (500), asset.circulating);
}

// A pruned source block should make rollback fail safely rather than crash.
TEST (asset_ledger, rolling_back_a_collect_with_pruned_source_rejected_safely)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction = store.tx_begin_write ();
	nano::asset_payload payload;
	payload.memo = "quest reward";
	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub, payload));
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));

	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);

	ledger.pruning = true;
	ASSERT_GT (ledger.pruning_action (transaction, mint->hash (), 1), 0);
	ASSERT_FALSE (store.block.exists (transaction, mint->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, mint->hash ()));

	// The source is what says who sent the units, which asset they were and
	// what memo rode along, so without it the receivable cannot be rebuilt and
	// the rollback is refused.
	ASSERT_TRUE (ledger.rollback (transaction, collect->hash ()));
	// Refused has to mean nothing moved. The three facts below are the ones a
	// half-applied rollback would put out of step with each other: the collect
	// stays on the chain, the player keeps what it collected, and the
	// receivable it consumed stays consumed. Restoring the receivable while
	// leaving the balance would credit the player twice.
	ASSERT_TRUE (store.block.exists (transaction, collect->hash ()));
	ASSERT_EQ (collect->hash (), ledger.latest (transaction, player.pub));
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
}

// If the accepter's tip is pruned, offer rollback cannot walk back to it.
TEST (asset_ledger, rolling_back_an_accepted_offer_with_pruned_settler_rejected_safely)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction = store.tx_begin_write ();
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto const offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto const accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	ledger.pruning = true;
	ASSERT_GT (ledger.pruning_action (transaction, accept->hash (), 1), 0);
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, accept->hash ()));

	ASSERT_TRUE (ledger.rollback (transaction, offer->hash ()));
	ASSERT_TRUE (store.block.exists (transaction, offer->hash ()));
	ASSERT_EQ (offer->hash (), ledger.latest (transaction, player.pub));
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, accept->hash ()));
	// Both arrivals the accept created are still uncollected and still where it
	// put them: the asset went to the accepter, and the Kei the offer asked for
	// went to the offerer — and Kei rides in the inherited `pending` table
	// rather than `asset_pending`, told apart by the zero asset id.
	ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	ASSERT_TRUE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));
	ASSERT_EQ (after_issuing (1) - 50, ledger.account_balance (transaction, nano::dev::team_key.pub));
}

// If the swap lock record disappears, accept rollback must fail cleanly.
TEST (asset_ledger, rolling_back_a_swap_accept_with_missing_lock_rejected_safely)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction = store.tx_begin_write ();
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto const offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto const accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	store.asset.lock_del (transaction, offer->hash ());

	// The asset leg arrived for the accepter and the Kei leg for the offerer.
	ASSERT_TRUE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	ASSERT_TRUE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));
	// The lock is the only record of what the two legs were worth, so with it
	// gone the accept cannot be inverted and the rollback is refused.
	ASSERT_TRUE (ledger.rollback (transaction, accept->hash ()));
	// And refusing leaves every one of those three facts untouched.
	ASSERT_TRUE (store.block.exists (transaction, accept->hash ()));
	ASSERT_EQ (accept->hash (), ledger.latest (transaction, nano::dev::team_key.pub));
	ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	ASSERT_TRUE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));
	ASSERT_EQ (after_issuing (1) - 50, ledger.account_balance (transaction, nano::dev::team_key.pub));
}


// The claim model (SPEC §5.5). Everything below is about the one property that
// makes it worth having: the issuer writes one block, and the claims that block
// underwrites are written by other accounts, in parallel, on their own chains.

// The headline case. Three players, one issuer block, three claims — and the
// issuer's chain is one block longer at the end of it, not three.
TEST (asset_ledger, one_issuer_block_underwrites_claims_from_many_accounts)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	std::array<nano::keypair, 3> players;
	std::array<nano::uint128_t, 3> const amounts{ 100, 250, 550 };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	std::vector<nano::uint256_union> leaves;
	for (std::size_t i (0); i < players.size (); ++i)
	{
		leaves.push_back (nano::asset_claim_leaf (players[i].pub, issued.id, nano::amount (amounts[i])));
	}
	drop const tree (leaves);

	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 900, tree.root (), commit_payload (3)));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	}
	{
		auto transaction (store.tx_begin_read ());
		nano::asset_commit_info published;
		ASSERT_FALSE (store.asset.commit_get (transaction, tree.root (), published));
		ASSERT_EQ (nano::dev::team_key.pub, published.issuer);
		ASSERT_EQ (issued.id, published.asset_id);
		ASSERT_EQ (3, published.count);
		ASSERT_EQ (nano::amount (900), published.total);
		ASSERT_EQ (commit->hash (), published.block);
		ASSERT_FALSE (published.closed);
		// Committing to a drop creates nothing. The units exist only once someone
		// claims them, which is what makes an unclaimed entitlement free.
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_TRUE (asset.circulating.is_zero ());
	}

	// Each claim opens its player's account: a player who has never held Kei can
	// claim, and pays no Kei to do it.
	for (std::size_t i (0); i < players.size (); ++i)
	{
		SCOPED_TRACE (i);
		auto claim (signed_asset (pool, players[i], 0, 0, nano::asset_op::claim, issued.id, nano::amount (amounts[i]), tree.root (), claim_payload (tree.proof (i))));
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claim).code);
		ASSERT_EQ (claim->hash (), ledger.latest (transaction, players[i].pub));
	}

	auto transaction (store.tx_begin_read ());
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (900), asset.circulating);
	for (std::size_t i (0); i < players.size (); ++i)
	{
		SCOPED_TRACE (i);
		ASSERT_EQ (nano::amount (amounts[i]), store.asset.balance (transaction, players[i].pub, issued.id));
		ASSERT_TRUE (store.asset.claim_exists (transaction, players[i].pub, tree.root ()));
		// SPEC §5.6.2 again, and it matters more here than anywhere: a drop that
		// bought weight would let an issuer mint themselves a majority.
		ASSERT_EQ (nano::uint128_t (0), ledger.weight (players[i].pub));
	}

	// The point of the whole mechanism: the issuer signed one block for three
	// recipients, and would have signed one for three million.
	auto const issuer_info (ledger.account_info (transaction, nano::dev::team_key.pub));
	ASSERT_TRUE (issuer_info);
	ASSERT_EQ (commit->hash (), issuer_info->head);
}

// One leaf per account per root, claimed once (SPEC §5.5). Without this the same
// proof is a licence to print the asset.
TEST (asset_ledger, a_claim_cannot_be_made_twice)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (player.pub, issued.id, nano::amount (500)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	auto claim (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claim).code);

	// A second claim from the same account against the same root, with a
	// perfectly valid proof, and it is still refused.
	auto again (signed_asset (pool, player, claim->hash (), 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::already_claimed, ledger.process (transaction, *again).code);
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
}

// A proof proves one account is owed one amount of one asset. Change any of the
// three and it proves nothing.
TEST (asset_ledger, a_proof_only_proves_the_leaf_it_was_cut_from)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;
	nano::keypair thief;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 10000));
	drop const tree ({ nano::asset_claim_leaf (player.pub, issued.id, nano::amount (500)),
	nano::asset_claim_leaf (thief.pub, issued.id, nano::amount (1)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 501, tree.root (), commit_payload (2)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);

	// The thief's own proof, for the amount the other player was owed.
	auto inflated (signed_asset (pool, thief, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::bad_claim_proof, ledger.process (transaction, *inflated).code);

	// Someone else's proof, presented by the thief.
	auto stolen (signed_asset (pool, thief, 0, 0, nano::asset_op::claim, issued.id, 1, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::bad_claim_proof, ledger.process (transaction, *stolen).code);

	// No proof at all, which is the shape that works if a root is ever allowed
	// to equal a leaf it did not commit to.
	auto bare (signed_asset (pool, thief, 0, 0, nano::asset_op::claim, issued.id, 1, tree.root (), claim_payload ({})));
	ASSERT_EQ (nano::process_result::bad_claim_proof, ledger.process (transaction, *bare).code);

	// The honest claim still works, so the rejections above are about the proofs
	// and not about the root.
	auto honest (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *honest).code);
}

// A leaf hash must never be readable as an interior node. Sorted pairs are what
// let a proof be siblings alone, and this is the attack that trick invites: fold
// two leaves by hand, present the pair as a leaf, and claim the parent.
TEST (asset_ledger, a_leaf_cannot_be_passed_off_as_an_interior_node)
{
	nano::keypair player;
	auto const asset_id (nano::derive_asset_id (player.pub, "GEM"));
	auto const leaf (nano::asset_claim_leaf (player.pub, asset_id, nano::amount (500)));
	auto const sibling (nano::asset_claim_leaf (player.pub, asset_id, nano::amount (501)));

	// Folding is order-independent, which is the property that removes direction
	// bits from the proof.
	ASSERT_EQ (nano::asset_claim_root (leaf, { sibling }), nano::asset_claim_root (sibling, { leaf }));

	// And the two domains are genuinely separate: the interior node over a pair
	// is not the leaf of anything, so no leaf can be constructed to equal it.
	auto const parent (nano::asset_claim_root (leaf, { sibling }));
	ASSERT_NE (parent, leaf);
	ASSERT_NE (parent, sibling);
	ASSERT_EQ (parent, nano::asset_claim_root (parent, {}));
}

namespace
{
/**
 * Cross-language claim vectors, generated by running kei-transaction's own
 * `packages/core/src/merkle.ts` (`leafHash` / `combineHashes`) — the frozen
 * contract `@keicoin/claims` proofs are built against — and independently
 * cross-checked against a second, unrelated BLAKE2b implementation
 * (`@noble/hashes`) before being pinned here. This node computed leaves and
 * roots that disagreed with every one of these, silently, because a domain
 * separator was hashed instead of sent as the SDK's literal `0x00`/`0x01`
 * tag byte (docs/decisions-m4.md §3). These tests exist so that regressing
 * the tag, the field order, or the amount width fails loudly instead of as
 * `bad_claim_proof` on a real network.
 */
nano::uint256_union hash32 (std::string const & hex_a)
{
	nano::uint256_union result;
	release_assert (!result.decode_hex (hex_a));
	return result;
}

nano::account account32 (std::string const & hex_a)
{
	nano::account result;
	release_assert (!result.decode_hex (hex_a));
	return result;
}

nano::amount amount_dec (std::string const & dec_a)
{
	nano::amount result;
	release_assert (!result.decode_dec (dec_a));
	return result;
}

nano::uint256_union const sdk_asset (hash32 ("FEEDFACEFEEDFACEFEEDFACEFEEDFACEFEEDFACEFEEDFACEFEEDFACEFEEDFACE"));
nano::uint256_union const sdk_asset2 (hash32 ("C0FFEE00C0FFEE00C0FFEE00C0FFEE00C0FFEE00C0FFEE00C0FFEE00C0FFEE00"));
}

TEST (asset_ledger, claim_leaf_matches_the_frozen_sdk_vectors)
{
	// A single leaf, and its sibling from the two-leaf vector below.
	ASSERT_EQ (hash32 ("1402721BF99B3726B51D10011B1F8567C3F475E7C96229B5B17B913E4F852205"),
	nano::asset_claim_leaf (account32 ("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), sdk_asset, nano::amount (500)));
	ASSERT_EQ (hash32 ("747B2A828015083E0D690A6D682F83C911681F0D704FD63EE111DDDD62D9D6AE"),
	nano::asset_claim_leaf (account32 ("B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2"), sdk_asset, nano::amount (1)));

	// Boundary amounts: zero, and the largest value a 128-bit amount can hold.
	// The amount is 16 bytes on both sides of the wire (SPEC §5.4); this is
	// the check that neither end silently truncates or sign-extends it.
	ASSERT_EQ (hash32 ("CC51FF1614DEAE1F581B9FD1B71C199756630A9DA179D57249D3708BA1B8E63E"),
	nano::asset_claim_leaf (account32 ("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), sdk_asset, nano::amount (0)));
	ASSERT_EQ (hash32 ("20DAF936F09E33045B0E02ED26E4EBA5F1AD31D42555B64D118643AAD60DCB77"),
	nano::asset_claim_leaf (account32 ("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), sdk_asset, amount_dec ("340282366920938463463374607431768211455")));

	// Same account and amount, different asset: a different leaf (§3's
	// "the leaf binds the asset id, not just the amount").
	ASSERT_NE (nano::asset_claim_leaf (account32 ("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), sdk_asset, nano::amount (500)),
	nano::asset_claim_leaf (account32 ("A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1"), sdk_asset2, nano::amount (500)));
}

// A one-leaf drop: the proof is empty and the root equals the leaf itself —
// the only shape that produces the JSON `[]` decisions-m4.md §8.3 describes.
TEST (asset_ledger, claim_root_of_an_empty_proof_is_the_leaf_itself)
{
	auto const leaf (hash32 ("721B73BEC584F7433AD9A1130D90A0F6D05342BD32B51139C921B3AAE93E072B"));
	ASSERT_EQ (leaf, nano::asset_claim_root (leaf, {}));
}

// A two-leaf drop: the proof is exactly one sibling, and folds to the same
// root regardless of which side of the pair supplies it.
TEST (asset_ledger, claim_root_of_a_two_leaf_drop_matches_the_sdk)
{
	auto const leaf_a (hash32 ("1402721BF99B3726B51D10011B1F8567C3F475E7C96229B5B17B913E4F852205"));
	auto const leaf_b (hash32 ("747B2A828015083E0D690A6D682F83C911681F0D704FD63EE111DDDD62D9D6AE"));
	auto const root (hash32 ("4D92EF46B7C8DD62A4A36EB099322AFFA64B54D990279244D4C424604B14B574"));
	ASSERT_EQ (root, nano::asset_claim_root (leaf_a, { leaf_b }));
	ASSERT_EQ (root, nano::asset_claim_root (leaf_b, { leaf_a }));
}

// A five-leaf drop, pairwise left to right with the odd node promoted at the
// top (the SDK's own tree shape — this node never chooses one, §3), checked
// with every leaf's own multi-level proof rather than just the first.
TEST (asset_ledger, claim_root_of_a_five_leaf_drop_matches_the_sdk)
{
	auto const leaf0 (hash32 ("A2894133746AB705360EDADC89D9B6758B8E4B725F4E083314B06637E5842D1C"));
	auto const leaf1 (hash32 ("8876712DF6EE23B2ADA3EE69E08BA7D551614E511DEC0729091A5B5980FA57A4"));
	auto const leaf2 (hash32 ("088F6D869E5B1E6721A5D21B982D46A8B3A1119D934732F45E07EFFD212B09A5"));
	auto const leaf3 (hash32 ("B558332403A2D64B56E7418D59835AC235BAC53F2AB17A10775CDEF71C9A8DCF"));
	auto const leaf4 (hash32 ("B1BD7E140EF0A09533D996AA089F029427E3AC8D235257C40B84259A9FDFAE5C"));
	auto const level1_01 (hash32 ("9D9735E537D629E9223EB4A22E29265D965CB4257A6EC4E0D898856785AA93F8"));
	auto const level1_23 (hash32 ("805ECA323809743F128B5258EB3EFF61E00E5A018F1745BCECA048D43AF89826"));
	auto const level2_0123 (hash32 ("E4B60B003BCF33F8D2D6F019B9AA5D3E98D9072ED728E9DA4361B35F2C106A1A"));
	auto const root (hash32 ("D52BC3EBFF47B0D5E7DF0739250E53E71C82D210D06169F20338B3A397E7D53C"));

	ASSERT_EQ (root, nano::asset_claim_root (leaf0, { leaf1, level1_23, leaf4 }));
	ASSERT_EQ (root, nano::asset_claim_root (leaf1, { leaf0, level1_23, leaf4 }));
	ASSERT_EQ (root, nano::asset_claim_root (leaf2, { leaf3, level1_01, leaf4 }));
	ASSERT_EQ (root, nano::asset_claim_root (leaf3, { leaf2, level1_01, leaf4 }));
	ASSERT_EQ (root, nano::asset_claim_root (leaf4, { level2_0123 }));
}

// End to end: a claim built entirely from values the frozen SDK produced —
// the leaf, the root, and the (empty, single-leaf) proof — going through the
// real validation path (`ledger::process`), not just the hash functions
// above. This is the shape that was actually broken: every proof
// `@keicoin/claims` produces failed here with `bad_claim_proof` until the
// domain separator matched the SDK's exactly.
TEST (asset_ledger, an_sdk_produced_claim_proof_is_accepted_end_to_end)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	// `nano::dev::team_key` is the same fixed issuer `issue_one` uses
	// elsewhere in this file, so the asset id it derives here is exactly what
	// the SDK computed offline from the same key and symbol.
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	ASSERT_EQ (hash32 ("C103556DE3F72E4C23FA8D338A202C2BE5F3FD596ABB636323A04B9C41752017"), issued.id);

	nano::keypair const player ("AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA00");
	ASSERT_EQ (account32 ("DECD65070C4EF8AE1AC04E6581A95F0FEFA0602F1761D2E70DA848285988857E"), player.pub);

	auto const leaf (hash32 ("721B73BEC584F7433AD9A1130D90A0F6D05342BD32B51139C921B3AAE93E072B"));
	ASSERT_EQ (leaf, nano::asset_claim_leaf (player.pub, issued.id, nano::amount (500)));
	nano::block_hash const root (leaf.number ());

	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, root, commit_payload (1)));
	auto claim (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, root, claim_payload ({})));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claim).code);
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
}

// Roots are closed by the issuer, not by a clock (SPEC §5.5). A block-lattice
// has no clock to close them with.
TEST (asset_ledger, a_closed_root_accepts_no_further_claims)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair early;
	nano::keypair late;
	nano::keypair stranger;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (early.pub, issued.id, nano::amount (100)),
	nano::asset_claim_leaf (late.pub, issued.id, nano::amount (200)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 300, tree.root (), commit_payload (2)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	auto claimed (signed_asset (pool, early, 0, 0, nano::asset_op::claim, issued.id, 100, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claimed).code);

	// Only the issuer may close, and closing is a statement about the root alone.
	auto trespass (signed_asset (pool, stranger, 0, 0, nano::asset_op::commit_close, 0, 0, tree.root ()));
	ASSERT_EQ (nano::process_result::not_issuer, ledger.process (transaction, *trespass).code);

	auto close (signed_asset (pool, nano::dev::team_key, commit->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit_close, 0, 0, tree.root ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *close).code);
	nano::asset_commit_info published;
	ASSERT_FALSE (store.asset.commit_get (transaction, tree.root (), published));
	ASSERT_TRUE (published.closed);

	// The unclaimed entitlement is now unclaimable, which is the cost the issuer
	// accepted in exchange for the root becoming prunable.
	auto too_late (signed_asset (pool, late, 0, 0, nano::asset_op::claim, issued.id, 200, tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::commit_closed, ledger.process (transaction, *too_late).code);

	// Closing twice would be a block that changes nothing.
	auto close_again (signed_asset (pool, nano::dev::team_key, close->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit_close, 0, 0, tree.root ()));
	ASSERT_EQ (nano::process_result::commit_closed, ledger.process (transaction, *close_again).code);

	// What was claimed before the close stays claimed.
	ASSERT_EQ (nano::amount (100), store.asset.balance (transaction, early.pub, issued.id));
}

// Only the asset's issuer can underwrite a drop of it — which is also what makes
// racing an issuer to publish their root impossible rather than merely hard.
TEST (asset_ledger, only_the_issuer_can_commit_a_root)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair impostor;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (impostor.pub, issued.id, nano::amount (500)) });

	auto transaction (store.tx_begin_write ());
	auto forged (signed_asset (pool, impostor, 0, 0, nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	ASSERT_EQ (nano::process_result::not_issuer, ledger.process (transaction, *forged).code);

	// And a root nobody published underwrites nothing.
	auto orphan (signed_asset (pool, impostor, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::no_such_commit, ledger.process (transaction, *orphan).code);

	// Republishing a root the issuer already published would reopen it.
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	auto twice (signed_asset (pool, nano::dev::team_key, commit->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	ASSERT_EQ (nano::process_result::commit_exists, ledger.process (transaction, *twice).code);
}

// The cap binds where units come into existence, and for a drop that is the
// claim rather than the commit — the node cannot know what a root adds up to.
TEST (asset_ledger, claims_cannot_exceed_max_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair lucky;
	nano::keypair unlucky;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 100));
	drop const tree ({ nano::asset_claim_leaf (lucky.pub, issued.id, nano::amount (60)),
	nano::asset_claim_leaf (unlucky.pub, issued.id, nano::amount (60)) });
	// The issuer over-committed. Nothing here is checkable at commit time.
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 120, tree.root (), commit_payload (2)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);

	auto first (signed_asset (pool, lucky, 0, 0, nano::asset_op::claim, issued.id, 60, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *first).code);

	// Whoever claims second finds the cap already spent. This is the issuer's
	// mistake surfacing at the only place the node can see it.
	auto second (signed_asset (pool, unlucky, 0, 0, nano::asset_op::claim, issued.id, 60, tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *second).code);
}

// The claim path carries the same uint128 sum as the mint, and a leaf's amount
// is whatever the issuer put in the tree — nothing bounds it below 2^128. A
// wrapping claim would leave its claimant holding nearly the whole numeric range
// of an asset capped at 100, and the recorded supply at zero.
TEST (asset_ledger, claiming_cannot_wrap_a_capped_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair lucky;
	nano::keypair greedy;
	nano::keypair last;

	auto const ceiling (std::numeric_limits<nano::uint128_t>::max ());
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 100));
	// 60 + (2^128 - 59) is 2^128, which is zero in uint128.
	drop const tree ({ nano::asset_claim_leaf (lucky.pub, issued.id, nano::amount (60)),
	nano::asset_claim_leaf (greedy.pub, issued.id, nano::amount (ceiling - 59)),
	nano::asset_claim_leaf (last.pub, issued.id, nano::amount (40)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, nano::amount (ceiling), tree.root (), commit_payload (3)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);

	auto first (signed_asset (pool, lucky, 0, 0, nano::asset_op::claim, issued.id, 60, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *first).code);

	auto wrapping (signed_asset (pool, greedy, 0, 0, nano::asset_op::claim, issued.id, nano::amount (ceiling - 59), tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *wrapping).code);

	// Nothing was credited and nothing was recorded, so the supply still says
	// what the first claim left it saying.
	nano::asset_info rejected;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, rejected));
	ASSERT_EQ (nano::amount (60), rejected.circulating);
	ASSERT_TRUE (store.asset.balance (transaction, greedy.pub, issued.id).is_zero ());
	ASSERT_EQ (0, store.asset.holdings_count (transaction, greedy.pub));
	ASSERT_FALSE (store.asset.claim_exists (transaction, greedy.pub, tree.root ()));
	ASSERT_FALSE (store.block.exists (transaction, wrapping->hash ()));
	ASSERT_FALSE (ledger.account_info (transaction, greedy.pub));

	// The honest leaf behind it still claims the headroom that is really there.
	auto third (signed_asset (pool, last, 0, 0, nano::asset_op::claim, issued.id, 40, tree.root (), claim_payload (tree.proof (2))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *third).code);
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (100), asset.circulating);
	ASSERT_EQ (nano::amount (40), store.asset.balance (transaction, last.pub, issued.id));
}

// And uncapped on the claim side too, where the old check was skipped outright.
TEST (asset_ledger, claiming_cannot_wrap_an_uncapped_supply)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair whale;
	nano::keypair greedy;
	nano::keypair last;

	auto const ceiling (std::numeric_limits<nano::uint128_t>::max ());
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 0));
	drop const tree ({ nano::asset_claim_leaf (whale.pub, issued.id, nano::amount (ceiling - 10)),
	nano::asset_claim_leaf (greedy.pub, issued.id, nano::amount (11)),
	nano::asset_claim_leaf (last.pub, issued.id, nano::amount (10)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, nano::amount (ceiling), tree.root (), commit_payload (3)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);

	auto first (signed_asset (pool, whale, 0, 0, nano::asset_op::claim, issued.id, nano::amount (ceiling - 10), tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *first).code);

	auto over (signed_asset (pool, greedy, 0, 0, nano::asset_op::claim, issued.id, 11, tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::over_max_supply, ledger.process (transaction, *over).code);

	nano::asset_info rejected;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, rejected));
	ASSERT_EQ (nano::amount (ceiling - 10), rejected.circulating);
	ASSERT_TRUE (store.asset.balance (transaction, greedy.pub, issued.id).is_zero ());
	ASSERT_FALSE (store.asset.claim_exists (transaction, greedy.pub, tree.root ()));
	ASSERT_FALSE (store.block.exists (transaction, over->hash ()));

	// Ten fits where eleven did not.
	auto exact (signed_asset (pool, last, 0, 0, nano::asset_op::claim, issued.id, 10, tree.root (), claim_payload (tree.proof (2))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *exact).code);
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_EQ (nano::amount (ceiling), asset.circulating);
}

TEST (asset_ledger, rolling_back_a_claim_returns_the_units_and_the_entitlement)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (player.pub, issued.id, nano::amount (500)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	auto claim (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claim).code);
	ASSERT_FALSE (ledger.rollback (transaction, claim->hash ()));

	// The units are gone, the supply they added is gone, and the account is back
	// to never having existed.
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
	ASSERT_EQ (0, store.asset.holdings_count (transaction, player.pub));
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_TRUE (asset.circulating.is_zero ());
	ASSERT_FALSE (ledger.account_info (transaction, player.pub));

	// And the entitlement is claimable again, because a rolled-back claim never
	// happened. A double-claim record that outlived its block would strand it.
	ASSERT_FALSE (store.asset.claim_exists (transaction, player.pub, tree.root ()));
	auto retry (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *retry).code);
}

// The one rollback with no precedent in M2: the commit block loses a fork, and
// the claims it underwrote are on chains this ledger cannot enumerate from the
// commit itself. They come off first, oldest-account-last, or the ledger keeps
// balances that nothing backs.
TEST (asset_ledger, rolling_back_a_commit_takes_its_claims_with_it)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	std::array<nano::keypair, 2> players;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (players[0].pub, issued.id, nano::amount (100)),
	nano::asset_claim_leaf (players[1].pub, issued.id, nano::amount (200)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 300, tree.root (), commit_payload (2)));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	auto first (signed_asset (pool, players[0], 0, 0, nano::asset_op::claim, issued.id, 100, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *first).code);
	auto second (signed_asset (pool, players[1], 0, 0, nano::asset_op::claim, issued.id, 200, tree.root (), claim_payload (tree.proof (1))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *second).code);

	// One player has since spent part of what they claimed, so their claim is no
	// longer their frontier — the rollback has to walk their chain back to it.
	auto spent (signed_asset (pool, players[1], second->hash (), 0, nano::asset_op::burn, issued.id, 50, 0));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *spent).code);

	ASSERT_FALSE (ledger.rollback (transaction, commit->hash ()));

	nano::asset_commit_info published;
	ASSERT_TRUE (store.asset.commit_get (transaction, tree.root (), published));
	for (auto const & player : players)
	{
		ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
		ASSERT_FALSE (store.asset.claim_exists (transaction, player.pub, tree.root ()));
		ASSERT_FALSE (ledger.account_info (transaction, player.pub));
	}
	nano::asset_info asset;
	ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
	ASSERT_TRUE (asset.circulating.is_zero ());
	// The issuance underneath is untouched: only the drop was rolled back.
	ASSERT_EQ (issued.block->hash (), ledger.latest (transaction, nano::dev::team_key.pub));
}

TEST (asset_ledger, rolling_back_a_close_reopens_the_root)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	drop const tree ({ nano::asset_claim_leaf (player.pub, issued.id, nano::amount (500)) });
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 500, tree.root (), commit_payload (1)));
	auto close (signed_asset (pool, nano::dev::team_key, commit->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit_close, 0, 0, tree.root ()));

	auto transaction (store.tx_begin_write ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *close).code);
	ASSERT_FALSE (ledger.rollback (transaction, close->hash ()));

	nano::asset_commit_info published;
	ASSERT_FALSE (store.asset.commit_get (transaction, tree.root (), published));
	ASSERT_FALSE (published.closed);
	auto claim (signed_asset (pool, player, 0, 0, nano::asset_op::claim, issued.id, 500, tree.root (), claim_payload (tree.proof (0))));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claim).code);
}

// SPEC §5.6.4: all three swap legs sit at tier B, alongside `send` and
// `transfer` — an offer nobody accepts still costs the offerer their own
// asset until they cancel, so the spam it buys is self-funded.
TEST (asset_ledger, swap_ops_are_priced_at_tier_b)
{
	for (auto const op : { nano::asset_op::swap_offer, nano::asset_op::swap_accept, nano::asset_op::swap_cancel })
	{
		SCOPED_TRACE (nano::asset_op_to_string (op));
		ASSERT_EQ (nano::dev::network_params.work.tier_b (), nano::dev::network_params.work.threshold_asset (op));
	}
}

// SPEC §9.2: the offer debits the offerer's own asset out of their spendable
// balance and into a locked entry keyed by the offer's own hash, and lists it
// for the market (SPEC §9.3) — nothing moves to anybody yet.
TEST (asset_ledger, swap_offer_locks_the_asset_and_lists_it)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	// Open to anyone (no `to`), asking 50 raw Kei for 200 GEM.
	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	ASSERT_EQ (nano::amount (300), store.asset.balance (transaction, player.pub, issued.id));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_TRUE (lock.open ());
	ASSERT_EQ (player.pub, lock.offerer);
	ASSERT_EQ (issued.id, lock.asset_id);
	ASSERT_EQ (nano::amount (200), lock.amount);
	ASSERT_TRUE (lock.want_asset.is_zero ());
	ASSERT_EQ (nano::amount (50), lock.want_amount);
	ASSERT_TRUE (lock.counterparty.is_zero ());

	auto found (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_NE (store.asset.offers_end (), found);
	ASSERT_TRUE (nano::offer_key (issued.id, offer->hash ()) == found->first);
	ASSERT_EQ (player.pub, found->second);
}

// Locking an asset moves none of the offerer's Kei, so the fixed header's
// own balance field must restate the previous one exactly — the same
// invariant `swap_accept`/`swap_cancel` already enforce on their own asset
// branches. Without it, any account holding any asset could write whatever
// it likes into this field and mint itself Kei.
TEST (asset_ledger, swap_offer_locking_an_asset_cannot_forge_the_balance_field)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	// The player's chain has never received Kei — its balance is 0 — so any
	// nonzero value here is a forgery, not a restatement.
	auto forged (signed_asset (pool, player, held.collect->hash (), nano::amount (1'000'000), nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::asset_balance_mismatch, ledger.process (transaction, *forged).code);
}

// The first time an asset-typed block moves Kei itself: locking Kei is the
// fixed header's own balance field, exactly like a send (SPEC §9.2).
TEST (asset_ledger, an_offer_may_lock_kei_itself)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());

	auto offer (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1) - 5), nano::asset_op::swap_offer, 0, 5, 0, offer_payload (issued.id, 100)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	ASSERT_EQ (after_issuing (1) - 5, ledger.account_balance (transaction, nano::dev::team_key.pub));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_TRUE (lock.asset_id.is_zero ());
	ASSERT_EQ (nano::amount (5), lock.amount);
	ASSERT_EQ (issued.id, lock.want_asset);
}

// SPEC §9.2: `swap_accept` debits the accepter and creates a receivable of
// each side to the other, in the one block that settles both legs.
TEST (asset_ledger, swap_accept_settles_both_legs_and_each_side_collects)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	// The team allocation pays 50 raw Kei for the 200 GEM the offer locked.
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_FALSE (lock.open ());
	ASSERT_EQ (accept->hash (), lock.settled_by);
	auto gone (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_TRUE (gone == store.asset.offers_end () || !(nano::offer_key (issued.id, offer->hash ()) == gone->first));

	// The player collects Kei exactly like any other receivable — a state
	// block, because a swap leg denominated in Kei is Kei (SPEC §9.2).
	nano::pending_info kei_pending;
	ASSERT_FALSE (store.pending.get (transaction, nano::pending_key (player.pub, accept->hash ()), kei_pending));
	ASSERT_EQ (nano::dev::team_key.pub, kei_pending.source);
	ASSERT_EQ (nano::amount (50), kei_pending.amount);
	// The player's chain is not new — `held.collect` already opened it, and
	// `offer` is its current head — so this receive continues it rather than
	// opening a fresh account.
	nano::block_builder builder;
	auto open (builder.state ()
			   .make_block ()
			   .account (player.pub)
			   .previous (offer->hash ())
			   .representative (player.pub)
			   .balance (50)
			   .link (accept->hash ())
			   .sign (player.prv, player.pub)
			   // Root is `previous` once the chain is non-empty, not the
			   // account — work generated for the wrong root reads as
			   // insufficient no matter how much of it there is.
			   .work (*pool.generate (offer->hash ()))
			   .build_shared ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *open).code);
	ASSERT_EQ (nano::uint128_t (50), ledger.account_balance (transaction, player.pub));

	// The team allocation collects the 200 GEM the same way any mint is
	// collected — the asset side of the same block's other leg.
	nano::asset_pending_info gem_pending;
	ASSERT_FALSE (store.asset.pending_get (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ()), gem_pending));
	ASSERT_EQ (player.pub, gem_pending.source);
	ASSERT_EQ (issued.id, gem_pending.asset_id);
	ASSERT_EQ (nano::amount (200), gem_pending.amount);
	auto collect (signed_asset (pool, nano::dev::team_key, accept->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::asset_receive, 0, 0, accept->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	ASSERT_EQ (nano::amount (200), store.asset.balance (transaction, nano::dev::team_key.pub, issued.id));
}

// SPEC §9.2: "the same sword cannot be promised into ten swaps because after
// the first offer the sword is not in A's spendable balance to offer again."
TEST (asset_ledger, self_locking_prevents_offering_units_already_locked)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 500, 0, offer_payload (0, 10)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	auto second (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_offer, issued.id, 100, 0, offer_payload (0, 1)));
	ASSERT_EQ (nano::process_result::insufficient_asset_balance, ledger.process (transaction, *second).code);
}

// The offerer recovers their own asset whenever they like — the lock's own
// garbage collector (SPEC §9.3).
TEST (asset_ledger, swap_cancel_rejects_noncanonical_signed_headers_without_mutation)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto const block_count (ledger.cache.block_count.load ());
	auto const stored_block_count (store.block.count (transaction));
	auto const frontier (store.frontier.get (transaction, offer->hash ()));

	std::array<std::shared_ptr<nano::asset_block>, 2> noncanonical{
		signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, issued.id, 0, offer->hash ()),
		signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 1, offer->hash ())
	};
	for (auto const & rejected : noncanonical)
	{
		SCOPED_TRACE (rejected->hash ().to_string ());
		ASSERT_EQ (nano::process_result::bad_asset_payload, ledger.process (transaction, *rejected).code);
		ASSERT_EQ (block_count, ledger.cache.block_count.load ());
		ASSERT_EQ (stored_block_count, store.block.count (transaction));
		ASSERT_EQ (offer->hash (), ledger.latest (transaction, player.pub));
		ASSERT_TRUE (ledger.account_balance (transaction, player.pub).is_zero ());
		ASSERT_EQ (frontier, store.frontier.get (transaction, offer->hash ()));
		ASSERT_TRUE (store.frontier.get (transaction, rejected->hash ()).is_zero ());
		ASSERT_FALSE (store.block.exists (transaction, rejected->hash ()));
		ASSERT_EQ (nano::amount (300), store.asset.balance (transaction, player.pub, issued.id));
		ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, rejected->hash ())));
		ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (player.pub, rejected->hash ())));
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (500), asset.circulating);
		nano::asset_lock_info lock;
		ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
		ASSERT_TRUE (lock.open ());
		auto listing (store.asset.offers_begin (transaction, nano::offer_key (issued.id, offer->hash ())));
		ASSERT_NE (store.asset.offers_end (), listing);
		ASSERT_TRUE (nano::offer_key (issued.id, offer->hash ()) == listing->first);
	}

	auto canonical (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *canonical).code);
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
	assert_rpc_json_round_trip (*canonical);
}

TEST (asset_ledger, swap_cancel_returns_the_locked_asset)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);

	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
	auto gone (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_TRUE (gone == store.asset.offers_end () || !(nano::offer_key (issued.id, offer->hash ()) == gone->first));
}

TEST (asset_ledger, swap_cancel_returns_locked_kei)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());

	auto offer (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1) - 5), nano::asset_op::swap_offer, 0, 5, 0, offer_payload (issued.id, 100)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, nano::dev::team_key, offer->hash (), nano::amount (after_issuing (1)), nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);

	ASSERT_EQ (after_issuing (1), ledger.account_balance (transaction, nano::dev::team_key.pub));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
}

// A Kei-denominated lock credits the account's Kei balance, not a row in
// `holdings`, so the §7 cap has nothing to say about it. Worth pinning: the
// cancel above and the cancel below take the same branch on everything except
// whether `lock.asset_id` is zero.
TEST (asset_ledger, a_kei_cancel_is_unaffected_by_the_holdings_cap)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	fill_holdings (store, transaction, nano::dev::team_key.pub, nano::max_assets_per_account);

	auto offer (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1) - 5), nano::asset_op::swap_offer, 0, 5, 0, offer_payload (issued.id, 100)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, nano::dev::team_key, offer->hash (), nano::amount (after_issuing (1)), nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
	ASSERT_EQ (after_issuing (1), ledger.account_balance (transaction, nano::dev::team_key.pub));
}

// SPEC §7's cap, which until now was enforced by code no test executed. A mint
// is fine at the cap — it only leaves a receivable — and the refusal lands on
// the collect, which is the block that would add the row.
TEST (asset_ledger, a_collect_at_the_holdings_cap_is_refused)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	fill_holdings (store, transaction, player.pub, nano::max_assets_per_account);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));

	auto mint (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 500, player.pub));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *mint).code);
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, 0, 0, mint->hash ()));
	ASSERT_EQ (nano::process_result::too_many_assets, ledger.process (transaction, *collect).code);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));
	// The units are not destroyed by the refusal, and the account was never
	// opened: the receivable is still there to collect once a row is free.
	ASSERT_TRUE (store.asset.pending_exists (transaction, nano::pending_key (player.pub, mint->hash ())));
	ASSERT_FALSE (ledger.account_info (transaction, player.pub));
}

// A collect into a row the account already has is not a new row, so the cap
// does not bind on it. Getting this wrong would strand every receivable an
// account at the cap has for an asset it already holds.
TEST (asset_ledger, a_collect_at_the_holdings_cap_for_an_asset_already_held_is_accepted)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
	fill_holdings (store, transaction, player.pub, nano::max_assets_per_account - 1);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));

	auto again (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1)), nano::asset_op::mint, issued.id, 300, player.pub));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *again).code);
	auto collect (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::asset_receive, 0, 0, again->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *collect).code);
	ASSERT_EQ (nano::amount (800), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));
}

// #46. A `swap_offer` for an account's *whole* balance of an asset deletes the
// row rather than shrinking it, so a slot under the §7 cap comes free while the
// units sit in the lock. `swap_cancel` is the only op that puts units straight
// back into `holdings` — every other credit arrives as a receivable and is
// caught by the collect — and it had no cap check, so the freed slot could be
// spent on something else and the cancel would then make the 1,025th row. One
// per lock-receive-cancel cycle, with no ceiling to converge on.
TEST (asset_ledger, a_cancel_that_would_exceed_the_holdings_cap_is_refused)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
	fill_holdings (store, transaction, player.pub, nano::max_assets_per_account - 1);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 500, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
	ASSERT_EQ (nano::max_assets_per_account - 1, store.asset.holdings_count (transaction, player.pub));

	// Spend the freed slot on a second asset. This must be accepted — 1,023
	// rows really is under the cap, and refusing it would be the same bug
	// pointing the other way.
	auto const second_payload (issuance ("GEM2", nano::transfer_policy::open, 1000));
	auto const second_id (nano::derive_asset_id (nano::dev::team_key.pub, second_payload.symbol));
	auto second_issue (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (2)), nano::asset_op::issue, second_id, 0, 0, second_payload));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *second_issue).code);
	auto second_mint (signed_asset (pool, nano::dev::team_key, second_issue->hash (), nano::amount (after_issuing (2)), nano::asset_op::mint, second_id, 1, player.pub));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *second_mint).code);
	auto second_collect (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::asset_receive, 0, 0, second_mint->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *second_collect).code);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));

	auto cancel (signed_asset (pool, player, second_collect->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::too_many_assets, ledger.process (transaction, *cancel).code);
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));
	ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());

	// The refusal consumes nothing, so the locked asset is not lost. Nothing
	// expires a lock — `expires_at` is recorded on it and the ledger never
	// reads it — so this exact cancel stays available indefinitely.
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_TRUE (lock.open ());
	ASSERT_EQ (nano::amount (500), lock.amount);
	ASSERT_EQ (issued.id, lock.asset_id);

	// SPEC §7 says the error should name the fix — "burn or transfer
	// something" — so the fix has to work. `burn` is gated by no policy at
	// all, which is what makes the way out unconditional.
	auto burn (signed_asset (pool, player, second_collect->hash (), 0, nano::asset_op::burn, second_id, 1, 0));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *burn).code);
	ASSERT_EQ (nano::max_assets_per_account - 1, store.asset.holdings_count (transaction, player.pub));
	auto retried (signed_asset (pool, player, burn->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *retried).code);
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_EQ (nano::max_assets_per_account, store.asset.holdings_count (transaction, player.pub));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
}

// SPEC §9.2 point 4: the accept-vs-cancel race. Whichever the ledger applies
// first wins, and the second is a retryable "nothing moved" rather than a
// fault — this is the ledger's half of ORV resolving that conflict.
TEST (asset_ledger, an_accept_after_a_cancel_is_rejected_as_offer_consumed)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);

	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::offer_consumed, ledger.process (transaction, *accept).code);
}

TEST (asset_ledger, a_cancel_after_an_accept_is_rejected_as_offer_consumed)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	// The rightful offerer, trying to cancel a lock that already settled.
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::offer_consumed, ledger.process (transaction, *cancel).code);
}

TEST (asset_ledger, swap_consumer_with_pruned_or_missing_offer_is_safe)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued = issue_one (ledger, store, pool, nano::transfer_policy::open, 1000);
	auto transaction = store.tx_begin_write ();
	auto const held = fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500);

	auto const offer = signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto const accept = signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	// A closed lock pointing at a pruned block should not crash on lookup.
	nano::asset_lock_info synthetic_lock;
	synthetic_lock.offerer = player.pub;
	synthetic_lock.amount = nano::amount (200);
	synthetic_lock.settled_by = accept->hash ();
	store.asset.lock_put (transaction, nano::block_hash{ 99 }, synthetic_lock);

	ledger.pruning = true;
	ASSERT_GT (ledger.pruning_action (transaction, accept->hash (), 1), 0);
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, accept->hash ()));

	ASSERT_FALSE (ledger.swap_consumer (transaction, nano::block_hash{ 99 }));
	auto missing_offer_consumer = ledger.swap_consumer (transaction, nano::block_hash{ 100 });
	ASSERT_FALSE (missing_offer_consumer);
}

TEST (asset_ledger, rollback_swap_conflict_with_pruned_settler_is_rejected_safely)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued = issue_one (ledger, store, pool, nano::transfer_policy::open, 1000);
	auto transaction = store.tx_begin_write ();
	auto const held = fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500);

	auto const offer = signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto const accept = signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	// A competing consumer, so that reconciling towards it actually has to
	// unwind the accept. Naming the accept itself would be a no-op: it is
	// already the settler, and the function would return before it ever looked
	// at the pruned chain.
	auto const cancel = signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ());
	ASSERT_EQ (nano::process_result::offer_consumed, ledger.process (transaction, *cancel).code);

	ledger.pruning = true;
	ASSERT_GT (ledger.pruning_action (transaction, accept->hash (), 1), 0);
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, accept->hash ()));

	// Unwinding the accept means walking the accepter's chain down, and the
	// pruned tip is where that walk runs out of blocks to walk.
	std::vector<std::shared_ptr<nano::block>> rolled_back;
	ASSERT_TRUE (ledger.rollback_swap_conflict (transaction, offer->hash (), cancel->hash (), rolled_back));
	ASSERT_TRUE (rolled_back.empty ());
	ASSERT_TRUE (store.block.exists (transaction, offer->hash ()));
	// The assertion with teeth: the pruned accept is still pruned. A rollback
	// that gave up part-way through unwinding it could have put it back.
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_TRUE (store.pruned.exists (transaction, accept->hash ()));
	// The cancel never entered the store — `process` refused it as
	// `offer_consumed` above — so on its own this line is close to a tautology.
	// Kept alongside the accept check rather than in place of it.
	ASSERT_FALSE (store.block.exists (transaction, cancel->hash ()));
	// The lock is still settled by the accept, exactly as it was found.
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_EQ (accept->hash (), lock.settled_by);
}

TEST (asset_ledger, an_elected_accept_replaces_an_applied_cancel)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::offer_consumed, ledger.process (transaction, *accept).code);

	std::vector<std::shared_ptr<nano::block>> rolled_back;
	ASSERT_FALSE (ledger.rollback_swap_conflict (transaction, offer->hash (), accept->hash (), rolled_back));
	ASSERT_EQ (1, rolled_back.size ());
	ASSERT_EQ (cancel->hash (), rolled_back.front ()->hash ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);
	ASSERT_TRUE (store.block.exists (transaction, accept->hash ()));
	ASSERT_FALSE (store.block.exists (transaction, cancel->hash ()));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_EQ (accept->hash (), lock.settled_by);
}

TEST (asset_ledger, an_elected_cancel_replaces_an_applied_accept)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::offer_consumed, ledger.process (transaction, *cancel).code);

	std::vector<std::shared_ptr<nano::block>> rolled_back;
	ASSERT_FALSE (ledger.rollback_swap_conflict (transaction, offer->hash (), cancel->hash (), rolled_back));
	ASSERT_EQ (1, rolled_back.size ());
	ASSERT_EQ (accept->hash (), rolled_back.front ()->hash ());
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
	ASSERT_TRUE (store.block.exists (transaction, cancel->hash ()));
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
	ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
}

TEST (asset_ledger, cross_account_swap_consumers_join_one_active_election)
{
	nano::test::system system;
	auto & node = *system.add_node ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair offerer;
	nano::keypair accepter;
	nano::block_hash const offer_hash{ 42 };

	auto cancel = signed_asset (pool, offerer, offer_hash, 1000, nano::asset_op::swap_cancel, 0, 0, offer_hash);
	auto accept = signed_asset (pool, accepter, nano::block_hash{ 7 }, 950, nano::asset_op::swap_accept, 0, 50, offer_hash);
	cancel->sideband_set ({});
	accept->sideband_set ({});

	auto inserted = node.active.insert (cancel);
	ASSERT_TRUE (inserted.inserted);
	ASSERT_NE (nullptr, inserted.election);
	ASSERT_FALSE (node.active.publish (accept));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (2, inserted.election->blocks ().size ());
	ASSERT_EQ (inserted.election, node.active.election (accept->election_qualified_root ()));
}

TEST (asset_ledger, final_vote_store_allows_only_one_swap_consumer_per_offer)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair offerer;
	nano::keypair accepter;
	nano::block_hash const offer_hash{ 42 };
	auto cancel = signed_asset (pool, offerer, offer_hash, 1000, nano::asset_op::swap_cancel, 0, 0, offer_hash);
	auto accept = signed_asset (pool, accepter, nano::block_hash{ 7 }, 950, nano::asset_op::swap_accept, 0, 50, offer_hash);
	auto transaction = store.tx_begin_write ({ nano::tables::final_votes });

	ASSERT_EQ (cancel->election_qualified_root (), accept->election_qualified_root ());
	ASSERT_TRUE (store.final_vote.put (transaction, cancel->election_qualified_root (), cancel->hash ()));
	ASSERT_FALSE (store.final_vote.put (transaction, accept->election_qualified_root (), accept->hash ()));
	auto const persisted = store.final_vote.get (transaction, cancel->election_root ());
	ASSERT_EQ (1, persisted.size ());
	ASSERT_EQ (cancel->hash (), persisted.front ());
}

TEST (asset_ledger, block_processor_seeds_and_reconciles_a_cross_chain_swap_election)
{
	nano::test::system system;
	auto & node = *system.add_node ();
	auto & ledger = node.ledger;
	auto & store = node.store;
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;
	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	std::shared_ptr<nano::asset_block> offer;
	std::shared_ptr<nano::asset_block> cancel;
	std::shared_ptr<nano::asset_block> accept;
	{
		auto transaction = store.tx_begin_write ();
		auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));
		offer = signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50));
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
		cancel = signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
		accept = signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ());
	}

	// There was no active election when the cancel was applied directly. The
	// rejected accept must make block_publisher recover that applied consumer
	// and seed the shared resource election before publishing the contender.
	node.process_active (accept);
	node.block_processor.flush ();
	auto election = node.active.election (accept->election_qualified_root ());
	ASSERT_NE (nullptr, election);
	ASSERT_EQ (2, election->blocks ().size ());

	// Simulate the election switching to the accept. Forced processing must
	// roll back the cancel on the other account chain without erasing this
	// election, and then apply the accept.
	node.block_processor.force (accept);
	node.block_processor.flush ();
	auto transaction = store.tx_begin_read ();
	ASSERT_TRUE (store.block.exists (transaction, accept->hash ()));
	ASSERT_FALSE (store.block.exists (transaction, cancel->hash ()));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_EQ (accept->hash (), lock.settled_by);
	ASSERT_EQ (election, node.active.election (accept->election_qualified_root ()));
}

TEST (asset_ledger, only_the_named_counterparty_may_accept)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player, named;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, named.pub, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	// The team allocation is not who this offer named.
	auto wrong (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::swap_not_counterparty, ledger.process (transaction, *wrong).code);
}

TEST (asset_ledger, an_accept_must_restate_the_offers_own_terms)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	// 40, not the 50 the offer asked for.
	auto cheap (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 40), nano::asset_op::swap_accept, 0, 40, offer->hash ()));
	ASSERT_EQ (nano::process_result::swap_terms_mismatch, ledger.process (transaction, *cheap).code);
}

TEST (asset_ledger, an_offerer_cannot_accept_their_own_offer)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (issued.id, 1)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);

	auto self_accept (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_accept, issued.id, 1, offer->hash ()));
	ASSERT_EQ (nano::process_result::swap_not_counterparty, ledger.process (transaction, *self_accept).code);
}

// SPEC §5.4: a swap leg answers to the same immutable policy a `transfer`
// does — the SDK does not get to trade its way around it.
TEST (asset_ledger, an_issuer_only_asset_can_only_be_offered_to_or_by_the_issuer)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::issuer_only, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	// Listed for anyone: not the issuer on either side, so nobody could
	// legally accept it, and the offer is refused rather than left open forever.
	auto open_listing (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::transfer_not_permitted, ledger.process (transaction, *open_listing).code);

	// Sold directly back to the issuer: the escape hatch the policy exists for.
	auto to_issuer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, nano::dev::team_key.pub, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *to_issuer).code);
}

TEST (asset_ledger, a_soulbound_asset_cannot_be_offered)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::none, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::transfer_not_permitted, ledger.process (transaction, *offer).code);
}

TEST (asset_ledger, rolling_back_an_offer_returns_the_lock_and_the_balance)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	ASSERT_FALSE (ledger.rollback (transaction, offer->hash ()));

	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
	auto gone (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_TRUE (gone == store.asset.offers_end () || !(nano::offer_key (issued.id, offer->hash ()) == gone->first));
}

TEST (asset_ledger, rolling_back_a_cancel_relocks_the_asset)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
	ASSERT_FALSE (ledger.rollback (transaction, cancel->hash ()));

	ASSERT_EQ (nano::amount (300), store.asset.balance (transaction, player.pub, issued.id));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_TRUE (lock.open ());
	auto found (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_NE (store.asset.offers_end (), found);
	ASSERT_TRUE (nano::offer_key (issued.id, offer->hash ()) == found->first);
}

TEST (asset_ledger, rolling_back_an_accept_takes_back_both_arrivals_and_reopens_the_lock)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);
	ASSERT_FALSE (ledger.rollback (transaction, accept->hash ()));

	ASSERT_EQ (after_issuing (1), ledger.account_balance (transaction, nano::dev::team_key.pub));
	ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	nano::asset_lock_info lock;
	ASSERT_FALSE (store.asset.lock_get (transaction, offer->hash (), lock));
	ASSERT_TRUE (lock.open ());
	auto found (store.asset.offers_begin (transaction, nano::offer_key (issued.id, 0)));
	ASSERT_NE (store.asset.offers_end (), found);
	ASSERT_TRUE (nano::offer_key (issued.id, offer->hash ()) == found->first);

	// The offerer can still cancel it after the reopen — the lock is not just
	// present but genuinely usable again.
	auto cancel (signed_asset (pool, player, offer->hash (), 0, nano::asset_op::swap_cancel, 0, 0, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *cancel).code);
}

// The sharp edge of §9.2 point 4: undoing an offer that was already accepted
// has to undo the accept first, even though it sits on a different account's
// chain that ordinary tip-first rollback never walks on its own.
TEST (asset_ledger, rolling_back_an_offer_that_was_accepted_undoes_the_accept_first)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	ASSERT_FALSE (ledger.rollback (transaction, offer->hash ()));

	// The accept is gone from the team allocation's own chain, taking it back
	// to exactly where it stood after the mint.
	ASSERT_EQ (held.mint->hash (), ledger.latest (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (after_issuing (1), ledger.account_balance (transaction, nano::dev::team_key.pub));
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	// `held.collect` opened the offerer's account before the offer ever
	// existed, so rolling the offer back — a single non-open block — leaves
	// the account in place, at the block before it, with its asset restored.
	ASSERT_EQ (held.collect->hash (), ledger.latest (transaction, player.pub));
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
}

// An offer cannot name its own author: `swap_accept` already refuses a
// self-accept independently (`an_offerer_cannot_accept_their_own_offer`
// above), but a *named* self-offer could never be accepted by anyone, so it
// is refused here instead of locking the asset forever for nothing.
TEST (asset_ledger, an_offer_naming_its_own_author_as_counterparty_is_refused)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, player.pub, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::self_swap, ledger.process (transaction, *offer).code);

	// Refused before anything is written: the asset never left spendable
	// balance and no lock or listing exists for a block that never applied.
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
}

// The sharp edge of §9.2 point 4, one step sharper than
// `rolling_back_an_offer_that_was_accepted_undoes_the_accept_first`: the
// accepter's chain has grown *since* the accept, so undoing the accept is
// not "roll back the accepter's current tip once" but "roll back the
// accepter's chain until the specific accept block is gone." A single,
// unlooped rollback call here would delete the offer's lock while the
// accept — and the receivables it created — silently survived underneath
// the accepter's newer block, which is a real double-spend: the offerer's
// own balance would be restored *and* the accepted receivable would remain
// collectible.
TEST (asset_ledger, rolling_back_an_offer_whose_accepter_has_since_grown_their_chain_still_undoes_the_accept_in_full)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::keypair player;

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 1000));
	auto transaction (store.tx_begin_write ());
	auto const held (fund_player (ledger, transaction, pool, issued.id, issued.block->hash (), player, 500));

	auto offer (signed_asset (pool, player, held.collect->hash (), 0, nano::asset_op::swap_offer, issued.id, 200, 0, offer_payload (0, 50)));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *offer).code);
	auto accept (signed_asset (pool, nano::dev::team_key, held.mint->hash (), nano::amount (after_issuing (1) - 50), nano::asset_op::swap_accept, 0, 50, offer->hash ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *accept).code);

	// The accepter does something else entirely afterward — a second,
	// unrelated issuance — so their chain's tip is no longer the accept.
	auto const second (issuance ("GEM2", nano::transfer_policy::open, 1000));
	auto const second_id (nano::derive_asset_id (nano::dev::team_key.pub, second.symbol));
	auto extra (signed_asset (pool, nano::dev::team_key, accept->hash (), nano::amount (after_issuing (2) - 50), nano::asset_op::issue, second_id, 0, 0, second));
	ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *extra).code);

	ASSERT_FALSE (ledger.rollback (transaction, offer->hash ()));

	// Both blocks the accepter wrote after the offer are gone, not just
	// pushed out of the way — the extra issuance never happened either.
	ASSERT_FALSE (store.block.exists (transaction, extra->hash ()));
	ASSERT_FALSE (store.block.exists (transaction, accept->hash ()));
	ASSERT_EQ (held.mint->hash (), ledger.latest (transaction, nano::dev::team_key.pub));
	ASSERT_EQ (after_issuing (1), ledger.account_balance (transaction, nano::dev::team_key.pub));
	nano::asset_info second_asset;
	ASSERT_TRUE (store.asset.get (transaction, second_id, second_asset));

	// The accept's own effects are fully undone, not left dangling under
	// the block that grew on top of them.
	ASSERT_FALSE (store.asset.pending_exists (transaction, nano::pending_key (nano::dev::team_key.pub, accept->hash ())));
	ASSERT_FALSE (store.pending.exists (transaction, nano::pending_key (player.pub, accept->hash ())));

	// The offerer's own side unwound exactly once — not doubled by a lock
	// that was deleted while the accept it belonged to was still standing.
	// `held.collect` opened this account before the offer, so it survives
	// rollback at the block before the offer, with its asset restored.
	ASSERT_EQ (held.collect->hash (), ledger.latest (transaction, player.pub));
	ASSERT_EQ (nano::amount (500), store.asset.balance (transaction, player.pub, issued.id));
	ASSERT_FALSE (store.asset.lock_exists (transaction, offer->hash ()));
}
