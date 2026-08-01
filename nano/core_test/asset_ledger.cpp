#include <nano/lib/work.hpp>
#include <nano/secure/buffer.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>
#include <nano/test_common/ledger.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <limits>

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

nano::uint256_union claim_leaf_for_test (nano::account const & account_a, nano::uint256_union const & asset_a, nano::amount const & amount_a, uint8_t tag_a = 0x00)
{
	nano::uint256_union result;
	blake2b_state hash;
	blake2b_init (&hash, result.bytes.size ());
	blake2b_update (&hash, &tag_a, 1);
	if (tag_a == 0x00)
	{
		blake2b_update (&hash, account_a.bytes.data (), account_a.bytes.size ());
		blake2b_update (&hash, asset_a.bytes.data (), asset_a.bytes.size ());
		blake2b_update (&hash, amount_a.bytes.data (), amount_a.bytes.size ());
	}
	else
	{
		blake2b_update (&hash, account_a.bytes.data (), account_a.bytes.size ());
	}
	blake2b_final (&hash, result.bytes.data (), result.bytes.size ());
	return result;
}

nano::uint256_union claim_parent_for_test (nano::uint256_union const & a, nano::uint256_union const & b)
{
	auto const ordered = std::lexicographical_compare (b.bytes.begin (), b.bytes.end (), a.bytes.begin (), a.bytes.end ());
	auto const & first = ordered ? b : a;
	auto const & second = ordered ? a : b;
	nano::uint256_union result;
	blake2b_state hash;
	blake2b_init (&hash, result.bytes.size ());
	uint8_t const tag{ 0x01 };
	blake2b_update (&hash, &tag, 1);
	blake2b_update (&hash, first.bytes.data (), first.bytes.size ());
	blake2b_update (&hash, second.bytes.data (), second.bytes.size ());
	blake2b_final (&hash, result.bytes.data (), result.bytes.size ());
	return result;
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

	for (auto const op : { nano::asset_op::issue, nano::asset_op::mint, nano::asset_op::burn, nano::asset_op::transfer, nano::asset_op::asset_receive })
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
		// An asset block can open an account, so one of these has no predecessor
		// — the shape a player's first collect takes (§10).
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, issuer, mint->hash (), balance, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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
	auto collect (signed_asset (pool, player, 0, 0, nano::asset_op::asset_receive, issued.id, 0, mint->hash ()));
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

TEST (asset_ledger, one_commit_underwrites_three_parallel_player_claims)
{
	auto ctx = nano::test::context::ledger_empty ();
	auto & ledger = ctx.ledger ();
	auto & store = ctx.store ();
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	std::array<nano::keypair, 3> players;
	std::array<nano::amount, 3> amounts{ nano::amount (10), nano::amount (20), nano::amount (30) };

	auto const issued (issue_one (ledger, store, pool, nano::transfer_policy::open, 60));
	std::array<nano::uint256_union, 4> leaves{
		claim_leaf_for_test (players[0].pub, issued.id, amounts[0]),
		claim_leaf_for_test (players[1].pub, issued.id, amounts[1]),
		claim_leaf_for_test (players[2].pub, issued.id, amounts[2]),
		claim_leaf_for_test (players[0].pub, nano::uint256_union{}, nano::amount (0), 0x02)
	};
	auto const left (claim_parent_for_test (leaves[0], leaves[1]));
	auto const right (claim_parent_for_test (leaves[2], leaves[3]));
	auto const root (claim_parent_for_test (left, right));

	nano::asset_payload commit_payload;
	commit_payload.count = 3;
	auto commit (signed_asset (pool, nano::dev::team_key, issued.block->hash (), nano::amount (after_issuing (1)), nano::asset_op::commit, issued.id, 60, root, commit_payload));
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *commit).code);
	}

	std::array<std::shared_ptr<nano::asset_block>, 3> claims;
	for (std::size_t i = 0; i < claims.size (); ++i)
	{
		nano::asset_payload payload;
		payload.proof = i == 0 ? std::vector<nano::uint256_union>{ leaves[1], right } :
			i == 1 ? std::vector<nano::uint256_union>{ leaves[0], right } :
			std::vector<nano::uint256_union>{ leaves[3], left };
		claims[i] = signed_asset (pool, players[i], 0, 0, nano::asset_op::claim, issued.id, amounts[i], root, payload);
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, *claims[i]).code);
	}

	{
		auto transaction (store.tx_begin_read ());
		for (std::size_t i = 0; i < claims.size (); ++i)
		{
			ASSERT_EQ (amounts[i], store.asset.balance (transaction, players[i].pub, issued.id));
			nano::block_hash claimed;
			ASSERT_FALSE (store.asset.claim_get (transaction, players[i].pub, root, claimed));
			ASSERT_EQ (claims[i]->hash (), claimed);
		}
		nano::asset_info asset;
		ASSERT_FALSE (store.asset.get (transaction, issued.id, asset));
		ASSERT_EQ (nano::amount (60), asset.circulating);
	}

	// The primary index is (account, root), so submitting the same entitlement
	// from a later block on that account cannot credit it twice.
	{
		nano::asset_payload payload;
		payload.proof = { leaves[1], right };
		auto duplicate (signed_asset (pool, players[0], claims[0]->hash (), 0, nano::asset_op::claim, issued.id, amounts[0], root, payload));
		auto transaction (store.tx_begin_write ());
		ASSERT_NE (nano::process_result::progress, ledger.process (transaction, *duplicate).code);
	}

	// Rolling back the issuer commit crosses account chains and removes every
	// dependent claim before deleting the root.
	{
		auto transaction (store.tx_begin_write ());
		ASSERT_FALSE (ledger.rollback (transaction, commit->hash ())) ;
	}
	{
		auto transaction (store.tx_begin_read ());
		for (auto const & player : players)
		{
			ASSERT_FALSE (ledger.account_info (transaction, player.pub));
			ASSERT_TRUE (store.asset.balance (transaction, player.pub, issued.id).is_zero ());
		}
		nano::asset_commit_info gone;
		ASSERT_TRUE (store.asset.commit_get (transaction, root, gone));
	}
}
