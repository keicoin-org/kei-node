#include <nano/lib/blocks.hpp>
#include <nano/node/node.hpp>
#include <nano/node/ipc/ipc_server.hpp>
#include <nano/node/node_rpc_config.hpp>
#include <nano/rpc/rpc_request_processor.hpp>
#include <nano/rpc_test/common.hpp>
#include <nano/secure/store.hpp>
#include <nano/crypto/blake2/blake2.h>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace nano::test;

namespace
{
struct fake_market_chain
{
	nano::keypair key;
	nano::uint256_union asset{ 7 };
	std::vector<nano::block_hash> hashes;
	std::vector<nano::block_hash> offers;
};

constexpr char market_account_swaps_snapshot_salt[]{ "kmp1-account-swaps-snapshot-v1" };
constexpr char market_account_swaps_snapshot_empty_state{ '*' };

bool market_account_swaps_snapshot_hash (nano::node & node_a, nano::transaction const & transaction_a, nano::account const & account_a, nano::block_hash const & anchor_a, nano::block_hash const & position_a, nano::uint256_union & snapshot_a)
{
	blake2b_state state;
	if (blake2b_init (&state, sizeof (nano::uint256_union)) != 0)
	{
		return false;
	}
	blake2b_update (&state, reinterpret_cast<uint8_t const *> (&account_a), sizeof (account_a));
	blake2b_update (&state, reinterpret_cast<uint8_t const *> (market_account_swaps_snapshot_salt), sizeof (market_account_swaps_snapshot_salt) - 1);

	auto current (anchor_a);
	while (!current.is_zero () && current != position_a)
	{
		auto const block (node_a.store.block.get (transaction_a, current));
		if (block == nullptr)
		{
			return false;
		}
		auto const offer (dynamic_cast<nano::asset_block const *> (block.get ()));
		if (offer != nullptr && offer->hashables.op == nano::asset_op::swap_offer)
		{
			char state_char (market_account_swaps_snapshot_empty_state);
			nano::block_hash settled_by_hash{ 0 };
			nano::asset_lock_info lock;
			if (!node_a.store.asset.lock_get (transaction_a, current, lock))
			{
				if (lock.open ())
				{
					state_char = 'o';
				}
				else
				{
					state_char = 'a';
					settled_by_hash = lock.settled_by;
				}
			}
			else
			{
				auto cursor (current);
				while (true)
				{
					auto const next (node_a.store.block.successor (transaction_a, cursor));
					if (next.is_zero ())
					{
						break;
					}
					auto const next_block (node_a.store.block.get (transaction_a, next));
					if (next_block == nullptr)
					{
						return false;
					}
					auto const next_offer (dynamic_cast<nano::asset_block const *> (next_block.get ()));
					if (next_offer != nullptr && next_offer->hashables.op == nano::asset_op::swap_cancel && next_offer->hashables.link.as_block_hash () == current)
					{
						state_char = 'c';
						settled_by_hash = next;
						break;
					}
					cursor = next;
				}
			}
			blake2b_update (&state, current.bytes.data (), current.bytes.size ());
			blake2b_update (&state, reinterpret_cast<uint8_t const *> (&state_char), sizeof (state_char));
			blake2b_update (&state, settled_by_hash.bytes.data (), settled_by_hash.bytes.size ());
		}
		else
		{
			blake2b_update (&state, current.bytes.data (), current.bytes.size ());
			blake2b_update (&state, reinterpret_cast<uint8_t const *> (&market_account_swaps_snapshot_empty_state), sizeof (market_account_swaps_snapshot_empty_state));
			nano::block_hash const zero { 0 };
			blake2b_update (&state, zero.bytes.data (), zero.bytes.size ());
		}
		current = block->previous ();
	}

	blake2b_final (&state, snapshot_a.bytes.data (), snapshot_a.bytes.size ());
	return true;
}

nano::block_hash append_market_block (nano::node & node_a, fake_market_chain & chain_a, bool offer_a)
{
	auto const height (chain_a.hashes.size () + 1);
	auto const previous (chain_a.hashes.empty () ? nano::block_hash{ 0 } : chain_a.hashes.back ());
	nano::asset_payload payload;
	nano::asset_op const op (offer_a ? nano::asset_op::swap_offer : nano::asset_op::burn);
	if (offer_a)
	{
		payload.want_asset = 0;
		payload.want_amount = nano::amount (height * 10);
	}
	auto block (std::make_shared<nano::asset_block> (
	chain_a.key.pub,
	previous,
	chain_a.key.pub,
	nano::amount (100000 - height),
	op,
	chain_a.asset,
	nano::amount (height),
	0,
	payload,
	chain_a.key.prv,
	chain_a.key.pub,
	0));
	block->sideband_set (nano::block_sideband{
	chain_a.key.pub,
	0,
	nano::amount (100000 - height),
	static_cast<uint64_t> (height),
	static_cast<nano::seconds_t> (1000 + height),
	nano::block_details{ nano::epoch::epoch_0, false, false, false },
	nano::epoch::epoch_0 });

	auto transaction (node_a.store.tx_begin_write ());
	if (!previous.is_zero ())
	{
		auto previous_block (node_a.store.block.get (transaction, previous));
		auto sideband (previous_block->sideband ());
		sideband.successor = block->hash ();
		previous_block->sideband_set (sideband);
		node_a.store.block.put (transaction, previous, *previous_block);
	}
	node_a.store.block.put (transaction, block->hash (), *block);
	chain_a.hashes.push_back (block->hash ());
	if (offer_a)
	{
		nano::asset_lock_info lock;
		lock.offerer = chain_a.key.pub;
		lock.asset_id = chain_a.asset;
		lock.amount = nano::amount (height);
		lock.want_amount = nano::amount (height * 10);
		node_a.store.asset.lock_put (transaction, block->hash (), lock);
		node_a.store.asset.offer_put (transaction, chain_a.asset, block->hash (), chain_a.key.pub);
		chain_a.offers.push_back (block->hash ());
	}
	node_a.store.account.put (transaction, chain_a.key.pub, nano::account_info{ block->hash (), chain_a.key.pub, chain_a.hashes.front (), nano::amount (100000 - height), static_cast<nano::seconds_t> (1000 + height), static_cast<uint64_t> (height), nano::epoch::epoch_0 });
	node_a.store.confirmation_height.put (transaction, chain_a.key.pub, nano::confirmation_height_info{ static_cast<uint64_t> (height), block->hash () });
	return block->hash ();
}

boost::property_tree::ptree swaps_request (nano::account const & account_a, boost::optional<uint64_t> count_a = boost::none, boost::optional<uint64_t> scan_count_a = boost::none, std::string const & before_a = "")
{
	boost::property_tree::ptree request;
	request.put ("action", "account_swaps");
	request.put ("account", account_a.to_account ());
	if (count_a.is_initialized ())
	{
		request.put ("count", *count_a);
	}
	if (scan_count_a.is_initialized ())
	{
		request.put ("scan_count", *scan_count_a);
	}
	if (!before_a.empty ())
	{
		request.put ("before", before_a);
	}
	return request;
}

boost::property_tree::ptree wait_swaps (nano::test::system & system_a, nano::test::rpc_context const & rpc_ctx_a, nano::account const & account_a, boost::optional<uint64_t> count_a = boost::none, boost::optional<uint64_t> scan_count_a = boost::none, std::string const & before_a = "")
{
	auto request (swaps_request (account_a, count_a, scan_count_a, before_a));
	return wait_response (system_a, rpc_ctx_a, request, 10s);
}

std::vector<std::string> offer_hashes (boost::property_tree::ptree const & response_a)
{
	std::vector<std::string> hashes;
	for (auto const & child : response_a.get_child ("offers"))
	{
		hashes.push_back (child.second.get<std::string> ("hash"));
		EXPECT_TRUE (child.second.get<bool> ("confirmed"));
	}
	return hashes;
}
}

TEST (rpc, account_swaps_pages_without_duplicate_or_skip_and_freezes_head)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, false);
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, false);
	auto const frozen_head (append_market_block (*node, chain, true));

	nano::uint256_union expected_snapshot;
	auto transaction (node->store.tx_begin_read ());
	auto page1 (wait_swaps (system, rpc_ctx, chain.key.pub, 2, 2));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers[2].to_string () }), offer_hashes (page1));
	ASSERT_EQ (2, page1.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page1.get<bool> ("exhausted"));
	ASSERT_EQ ("scan_limit", page1.get<std::string> ("stopped"));
	ASSERT_TRUE (market_account_swaps_snapshot_hash (*node, transaction, chain.key.pub, frozen_head, chain.hashes[2], expected_snapshot));
	ASSERT_EQ (expected_snapshot.to_string (), page1.get<std::string> ("snapshot"));
	auto const cursor1 (page1.get<std::string> ("next"));

	// A new offer after page one must not appear in the frozen backward walk.
	append_market_block (*node, chain, true);
	auto page2 (wait_swaps (system, rpc_ctx, chain.key.pub, 2, 2, cursor1));
	ASSERT_TRUE (market_account_swaps_snapshot_hash (*node, transaction, chain.key.pub, frozen_head, chain.hashes[0], expected_snapshot));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers[1].to_string () }), offer_hashes (page2));
	ASSERT_EQ (2, page2.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page2.get<bool> ("exhausted"));
	ASSERT_EQ (expected_snapshot.to_string (), page2.get<std::string> ("snapshot"));

	auto page3 (wait_swaps (system, rpc_ctx, chain.key.pub, 1, 1, page2.get<std::string> ("next")));
	nano::uint256_union expected_final_snapshot;
	ASSERT_TRUE (market_account_swaps_snapshot_hash (*node, transaction, chain.key.pub, frozen_head, nano::block_hash{ 0 }, expected_final_snapshot));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers[0].to_string () }), offer_hashes (page3));
	ASSERT_EQ (1, page3.get<uint64_t> ("scanned"));
	ASSERT_TRUE (page3.get<bool> ("exhausted"));
	ASSERT_EQ ("exhausted", page3.get<std::string> ("stopped"));
	ASSERT_EQ ("null", page3.get<std::string> ("next"));
	ASSERT_EQ (expected_final_snapshot.to_string (), page3.get<std::string> ("snapshot"));

	std::set<std::string> unique;
	for (auto const & hash : { chain.offers[2], chain.offers[1], chain.offers[0] })
	{
		ASSERT_TRUE (unique.insert (hash.to_string ()).second);
	}
}

TEST (rpc, account_swaps_default_count_and_scan_count)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	for (auto i (0); i < 1025; ++i)
	{
		append_market_block (*node, chain, true);
	}

	auto page1 (wait_swaps (system, rpc_ctx, chain.key.pub));
	std::vector<std::string> expected_page1;
	for (auto i (chain.offers.size () - 1); i > 0; --i)
	{
		expected_page1.push_back (chain.offers[i].to_string ());
	}
	ASSERT_EQ (expected_page1, offer_hashes (page1));
	ASSERT_EQ (1024, page1.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page1.get<bool> ("exhausted"));
	ASSERT_EQ ("result_limit", page1.get<std::string> ("stopped"));
	ASSERT_NE ("null", page1.get<std::string> ("next"));

	nano::uint256_union expected_snapshot_page1;
	auto transaction (node->store.tx_begin_read ());
	ASSERT_TRUE (market_account_swaps_snapshot_hash (*node, transaction, chain.key.pub, chain.offers.back (), chain.offers.front (), expected_snapshot_page1));
	ASSERT_EQ (expected_snapshot_page1.to_string (), page1.get<std::string> ("snapshot"));

	auto page2 (wait_swaps (system, rpc_ctx, chain.key.pub, boost::none, boost::none, page1.get<std::string> ("next")));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers.front ().to_string () }), offer_hashes (page2));
	ASSERT_EQ (1, page2.get<uint64_t> ("scanned"));
	ASSERT_TRUE (page2.get<bool> ("exhausted"));
	ASSERT_EQ ("exhausted", page2.get<std::string> ("stopped"));
	ASSERT_EQ ("null", page2.get<std::string> ("next"));

	nano::uint256_union expected_snapshot_page2;
	ASSERT_TRUE (market_account_swaps_snapshot_hash (*node, transaction, chain.key.pub, chain.offers.back (), nano::block_hash{ 0 }, expected_snapshot_page2));
	ASSERT_EQ (expected_snapshot_page2.to_string (), page2.get<std::string> ("snapshot"));
}

TEST (rpc, account_swaps_rejects_malformed_tampered_cross_scope_and_stale_cursors)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, false);
	append_market_block (*node, chain, true);
	auto page (wait_swaps (system, rpc_ctx, chain.key.pub, 1, 1));
	auto const cursor (page.get<std::string> ("next"));

	auto malformed (swaps_request (chain.key.pub, 1, 1, "not-a-cursor"));
	ASSERT_EQ ("Invalid or tampered market cursor", wait_response (system, rpc_ctx, malformed, 10s).get<std::string> ("error"));

	auto tampered_cursor (cursor);
	tampered_cursor[20] = tampered_cursor[20] == 'A' ? 'B' : 'A';
	auto tampered (swaps_request (chain.key.pub, 1, 1, tampered_cursor));
	ASSERT_EQ ("Invalid or tampered market cursor", wait_response (system, rpc_ctx, tampered, 10s).get<std::string> ("error"));

	nano::keypair other;
	auto wrong_account (swaps_request (other.pub, 1, 1, cursor));
	ASSERT_EQ ("Market cursor does not belong to this account, asset, or filter", wait_response (system, rpc_ctx, wrong_account, 10s).get<std::string> ("error"));

	auto wrong_filter (swaps_request (chain.key.pub, 1, 1, cursor));
	wrong_filter.put ("state", "open");
	ASSERT_EQ ("Market cursor does not belong to this account, asset, or filter", wait_response (system, rpc_ctx, wrong_filter, 10s).get<std::string> ("error"));

	// Page one scanned the newest offer, so its cursor points at the middle block.
	{
		auto transaction (node->store.tx_begin_write ());
		node->store.block.del (transaction, chain.hashes[1]);
	}
	auto stale (swaps_request (chain.key.pub, 1, 1, cursor));
	ASSERT_EQ ("Market cursor is stale because its ledger position is no longer available", wait_response (system, rpc_ctx, stale, 10s).get<std::string> ("error"));
}

TEST (rpc, account_swaps_scan_count_is_independent_and_legacy_request_still_works)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, false);
	append_market_block (*node, chain, false);

	auto bounded (wait_swaps (system, rpc_ctx, chain.key.pub, 10, 1));
	ASSERT_TRUE (offer_hashes (bounded).empty ());
	ASSERT_EQ (1, bounded.get<uint64_t> ("scanned"));
	ASSERT_FALSE (bounded.get<bool> ("exhausted"));
	ASSERT_EQ ("scan_limit", bounded.get<std::string> ("stopped"));

	boost::property_tree::ptree legacy;
	legacy.put ("action", "account_swaps");
	legacy.put ("account", chain.key.pub.to_account ());
	legacy.put ("count", 10);
	auto legacy_response (wait_response (system, rpc_ctx, legacy, 10s));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers[0].to_string () }), offer_hashes (legacy_response));
	ASSERT_EQ (3, legacy_response.get<uint64_t> ("scanned"));
	ASSERT_TRUE (legacy_response.get<bool> ("exhausted"));

	for (auto const & invalid : { "0", "-1", "1x", "18446744073709551616", "1025" })
	{
		auto request (swaps_request (chain.key.pub, 1, 1));
		request.put ("scan_count", invalid);
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("Invalid scan_count", response.get<std::string> ("error"));
		ASSERT_EQ (invalid, response.get<std::string> ("requested"));
		ASSERT_EQ ("scan_count", response.get<std::string> ("field"));
		ASSERT_EQ ("1024", response.get<std::string> ("max"));
		ASSERT_EQ ("1024", response.get<std::string> ("maxAllowed"));
		ASSERT_EQ ("Retry with a lower value and the same request.", response.get<std::string> ("suggestion"));
	}

	for (auto const & invalid : { "0", "1025" })
	{
		auto request (swaps_request (chain.key.pub, static_cast<uint64_t> (1), static_cast<uint64_t> (1)));
		request.put ("count", invalid);
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("Invalid count", response.get<std::string> ("error"));
		ASSERT_EQ (invalid, response.get<std::string> ("requested"));
		ASSERT_EQ ("count", response.get<std::string> ("field"));
		ASSERT_EQ ("1024", response.get<std::string> ("max"));
		ASSERT_EQ ("1024", response.get<std::string> ("maxAllowed"));
		ASSERT_EQ ("Retry with a lower value and the same request.", response.get<std::string> ("suggestion"));
	}
}

TEST (rpc, account_swaps_scan_count_bounds_cancelled_state_walk)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	auto const offer (append_market_block (*node, chain, true));
	append_market_block (*node, chain, false);
	append_market_block (*node, chain, false);

	{
		auto transaction (node->store.tx_begin_write ());
		node->store.asset.lock_del (transaction, offer);
	}

	auto bounded (wait_swaps (system, rpc_ctx, chain.key.pub, 10, 3));
	ASSERT_TRUE (offer_hashes (bounded).empty ());
	ASSERT_EQ (3, bounded.get<uint64_t> ("scanned"));
	ASSERT_FALSE (bounded.get<bool> ("exhausted"));
	ASSERT_EQ ("scan_limit", bounded.get<std::string> ("stopped"));
}

TEST (rpc, account_swaps_stale_cursor_on_state_filter_when_head_advances)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, true);

	auto page1 (swaps_request (chain.key.pub, 1));
	page1.put ("state", "open");
	auto response1 (wait_response (system, rpc_ctx, page1, 10s));
	ASSERT_NE ("null", response1.get<std::string> ("next"));

	append_market_block (*node, chain, true);
	auto page2 (swaps_request (chain.key.pub, 1, boost::none, response1.get<std::string> ("next")));
	page2.put ("state", "open");
	ASSERT_EQ ("Market cursor is stale because its ledger position is no longer available", wait_response (system, rpc_ctx, page2, 10s).get<std::string> ("error"));
}

TEST (rpc, account_swaps_first_page_works_when_history_is_pruned)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, true);
	append_market_block (*node, chain, true);

	{
		auto transaction (node->store.tx_begin_write ());
		node->store.block.del (transaction, chain.hashes[1]);
	}

	auto page (wait_swaps (system, rpc_ctx, chain.key.pub));
	ASSERT_EQ ((std::vector<std::string>{ chain.offers[2].to_string () }), offer_hashes (page));
	ASSERT_EQ (1, page.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page.get<bool> ("exhausted"));
	ASSERT_EQ ("pruned", page.get<std::string> ("stopped"));
	ASSERT_EQ ("null", page.get<std::string> ("next"));
}

namespace
{
// `asset_offers` and `asset_holders` read two store tables and never look at a
// block, so the rows are written directly. Building 1,030 real `swap_offer`
// chains would cost the test several seconds to prove something
// `account_swaps` above already proves.
std::set<std::string> seed_offers (nano::node & node_a, nano::uint256_union const & asset_a, nano::account const & offerer_a, std::size_t count_a)
{
	std::set<std::string> hashes;
	auto transaction (node_a.store.tx_begin_write ());
	for (std::size_t i (0); i < count_a; ++i)
	{
		nano::block_hash const hash (i + 1);
		node_a.store.asset.offer_put (transaction, asset_a, hash, offerer_a);
		hashes.insert (hash.to_string ());
	}
	return hashes;
}

std::set<std::string> seed_holders (nano::node & node_a, nano::uint256_union const & asset_a, std::size_t count_a)
{
	std::set<std::string> accounts;
	auto transaction (node_a.store.tx_begin_write ());
	for (std::size_t i (0); i < count_a; ++i)
	{
		nano::account const holder (i + 1);
		node_a.store.asset.balance_put (transaction, holder, asset_a, nano::amount (5));
		accounts.insert (holder.to_account ());
	}
	return accounts;
}

// Collected rather than compared position by position: what the issue asks for
// is that paging returns every row exactly once, and asserting that directly
// does not also assert an ordering the store is free to choose.
std::vector<std::string> page_values (boost::property_tree::ptree const & response_a, char const * array_a, char const * field_a)
{
	std::vector<std::string> values;
	for (auto const & child : response_a.get_child (array_a))
	{
		values.push_back (child.second.get<std::string> (field_a));
	}
	return values;
}

// `append_market_block` only makes offers and burns. A cancelled offer needs a
// `swap_cancel` whose link names the offer, and the lock deleted — deleting it
// is what a real cancel does (decisions-m5.md §7) and is what forces
// `swap_offer_to_json` down the chain walk instead of the cheap lock lookup.
nano::block_hash append_cancel (nano::node & node_a, fake_market_chain & chain_a, nano::block_hash const & offer_a)
{
	auto const height (chain_a.hashes.size () + 1);
	auto const previous (chain_a.hashes.back ());
	nano::asset_payload payload;
	auto block (std::make_shared<nano::asset_block> (
	chain_a.key.pub,
	previous,
	chain_a.key.pub,
	nano::amount (100000 - height),
	nano::asset_op::swap_cancel,
	0,
	nano::amount (0),
	offer_a,
	payload,
	chain_a.key.prv,
	chain_a.key.pub,
	0));
	block->sideband_set (nano::block_sideband{
	chain_a.key.pub,
	0,
	nano::amount (100000 - height),
	static_cast<uint64_t> (height),
	static_cast<nano::seconds_t> (1000 + height),
	nano::block_details{ nano::epoch::epoch_0, false, false, false },
	nano::epoch::epoch_0 });

	auto transaction (node_a.store.tx_begin_write ());
	auto previous_block (node_a.store.block.get (transaction, previous));
	auto sideband (previous_block->sideband ());
	sideband.successor = block->hash ();
	previous_block->sideband_set (sideband);
	node_a.store.block.put (transaction, previous, *previous_block);
	node_a.store.block.put (transaction, block->hash (), *block);
	chain_a.hashes.push_back (block->hash ());
	node_a.store.asset.lock_del (transaction, offer_a);
	node_a.store.asset.offer_del (transaction, chain_a.asset, offer_a);
	node_a.store.account.put (transaction, chain_a.key.pub, nano::account_info{ block->hash (), chain_a.key.pub, chain_a.hashes.front (), nano::amount (100000 - height), static_cast<nano::seconds_t> (1000 + height), static_cast<uint64_t> (height), nano::epoch::epoch_0 });
	node_a.store.confirmation_height.put (transaction, chain_a.key.pub, nano::confirmation_height_info{ static_cast<uint64_t> (height), block->hash () });
	return block->hash ();
}

boost::property_tree::ptree swap_info_request (nano::block_hash const & hash_a, boost::optional<std::string> const & scan_count_a = boost::none)
{
	boost::property_tree::ptree request;
	request.put ("action", "swap_info");
	request.put ("hash", hash_a.to_string ());
	if (scan_count_a.is_initialized ())
	{
		request.put ("scan_count", *scan_count_a);
	}
	return request;
}
}

TEST (rpc, asset_offers_bounds_count_and_pages_each_offer_once)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	nano::uint256_union const asset{ 7 };
	nano::keypair offerer;
	auto const expected (seed_offers (*node, asset, offerer.pub, 1030));

	boost::property_tree::ptree request;
	request.put ("action", "asset_offers");
	request.put ("asset", asset.to_string ());

	// A bare request used to walk every open offer for the asset into one
	// response body. It now stops at the same 1,024 `account_swaps` stops at.
	auto const page1 (wait_response (system, rpc_ctx, request, 10s));
	auto const values1 (page_values (page1, "offers", "offer"));
	ASSERT_EQ (1024, values1.size ());
	ASSERT_EQ (1025, page1.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page1.get<bool> ("exhausted"));
	ASSERT_EQ ("result_limit", page1.get<std::string> ("stopped"));
	auto const next (page1.get<std::string> ("next"));
	ASSERT_NE ("null", next);

	auto request2 (request);
	request2.put ("start", next);
	auto const page2 (wait_response (system, rpc_ctx, request2, 10s));
	auto const values2 (page_values (page2, "offers", "offer"));
	ASSERT_EQ (6, values2.size ());
	ASSERT_TRUE (page2.get<bool> ("exhausted"));
	ASSERT_EQ ("exhausted", page2.get<std::string> ("stopped"));
	ASSERT_EQ ("null", page2.get<std::string> ("next"));
	// `start` is inclusive, the way `account_swaps` resumes at its cursor, so
	// the first row of page two is the one page one stopped before.
	ASSERT_EQ (next, values2.front ());

	std::set<std::string> seen;
	for (auto const & values : { values1, values2 })
	{
		for (auto const & value : values)
		{
			ASSERT_TRUE (seen.insert (value).second) << value << " was returned twice";
		}
	}
	ASSERT_EQ (expected, seen);

	for (auto const & invalid : { "0", "1025" })
	{
		auto bad (request);
		bad.put ("count", invalid);
		auto const response (wait_response (system, rpc_ctx, bad, 10s));
		ASSERT_EQ ("Invalid count", response.get<std::string> ("error"));
		ASSERT_EQ (invalid, response.get<std::string> ("requested"));
		ASSERT_EQ ("count", response.get<std::string> ("field"));
		ASSERT_EQ ("1024", response.get<std::string> ("max"));
		ASSERT_EQ ("1024", response.get<std::string> ("maxAllowed"));
		ASSERT_EQ ("Retry with a lower value and the same request.", response.get<std::string> ("suggestion"));
	}
}

TEST (rpc, asset_holders_bounds_count_and_pages_each_holder_once)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	nano::uint256_union const asset{ 9 };
	auto const expected (seed_holders (*node, asset, 1030));

	boost::property_tree::ptree request;
	request.put ("action", "asset_holders");
	request.put ("asset", asset.to_string ());

	auto const page1 (wait_response (system, rpc_ctx, request, 10s));
	auto const values1 (page_values (page1, "holders", "account"));
	ASSERT_EQ (1024, values1.size ());
	ASSERT_EQ (1025, page1.get<uint64_t> ("scanned"));
	ASSERT_FALSE (page1.get<bool> ("exhausted"));
	ASSERT_EQ ("result_limit", page1.get<std::string> ("stopped"));
	auto const next (page1.get<std::string> ("next"));
	ASSERT_NE ("null", next);

	auto request2 (request);
	request2.put ("start", next);
	auto const page2 (wait_response (system, rpc_ctx, request2, 10s));
	auto const values2 (page_values (page2, "holders", "account"));
	ASSERT_EQ (6, values2.size ());
	ASSERT_TRUE (page2.get<bool> ("exhausted"));
	ASSERT_EQ ("exhausted", page2.get<std::string> ("stopped"));
	ASSERT_EQ ("null", page2.get<std::string> ("next"));
	ASSERT_EQ (next, values2.front ());

	std::set<std::string> seen;
	for (auto const & values : { values1, values2 })
	{
		for (auto const & value : values)
		{
			ASSERT_TRUE (seen.insert (value).second) << value << " was returned twice";
		}
	}
	ASSERT_EQ (expected, seen);

	// A holder count that fits still answers in one page, and says so.
	nano::uint256_union const small{ 11 };
	seed_holders (*node, small, 3);
	auto small_request (request);
	small_request.put ("asset", small.to_string ());
	auto const small_page (wait_response (system, rpc_ctx, small_request, 10s));
	ASSERT_EQ (3, page_values (small_page, "holders", "account").size ());
	ASSERT_TRUE (small_page.get<bool> ("exhausted"));
	ASSERT_EQ ("null", small_page.get<std::string> ("next"));

	auto bad (request);
	bad.put ("count", "1025");
	auto const response (wait_response (system, rpc_ctx, bad, 10s));
	ASSERT_EQ ("Invalid count", response.get<std::string> ("error"));
	ASSERT_EQ ("count", response.get<std::string> ("field"));
	ASSERT_EQ ("1024", response.get<std::string> ("maxAllowed"));
}

TEST (rpc, swap_info_bounds_the_cancelled_offer_walk)
{
	nano::test::system system;
	auto node (add_ipc_enabled_node (system));
	auto const rpc_ctx (add_rpc (system, node));
	fake_market_chain chain;
	auto const offer (append_market_block (*node, chain, true));
	auto const open_offer (append_market_block (*node, chain, true));
	append_market_block (*node, chain, false);
	append_market_block (*node, chain, false);
	auto const cancel (append_cancel (*node, chain, offer));

	// An offer that is still open is one lock lookup and must stay one: the
	// budget must not be spent in front of the cheap path.
	{
		auto request (swap_info_request (open_offer));
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("open", response.get<std::string> ("offer.state"));
		ASSERT_EQ (0, response.get<uint64_t> ("scanned"));
		ASSERT_TRUE (response.get<bool> ("exhausted"));
		ASSERT_EQ ("exhausted", response.get<std::string> ("stopped"));
	}

	// The default budget is wide enough to find a cancel four blocks along.
	{
		auto request (swap_info_request (offer));
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("cancelled", response.get<std::string> ("offer.state"));
		ASSERT_EQ (cancel.to_string (), response.get<std::string> ("offer.settledBy"));
		ASSERT_TRUE (response.get<bool> ("exhausted"));
		ASSERT_EQ ("exhausted", response.get<std::string> ("stopped"));
	}

	// And the budget is actually consulted. Before this change `swap_info`
	// passed `UINT64_MAX` and had no `scan_count` at all, so this request
	// would have walked to the cancel and reported it.
	{
		auto request (swap_info_request (offer, std::string ("1")));
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("cancelled", response.get<std::string> ("offer.state"));
		// Undetermined, not absent: the walk stopped before it knew.
		ASSERT_EQ ("null", response.get<std::string> ("offer.settledBy"));
		ASSERT_EQ ("null", response.get<std::string> ("offer.settledAt"));
		ASSERT_FALSE (response.get<bool> ("offer.confirmed"));
		ASSERT_FALSE (response.get<bool> ("exhausted"));
		ASSERT_EQ ("scan_limit", response.get<std::string> ("stopped"));
		ASSERT_EQ (1, response.get<uint64_t> ("scanned"));
	}

	// Refused above the cap, the same way and with the same fields
	// `account_swaps` refuses it.
	for (auto const & invalid : { "0", "1025" })
	{
		auto request (swap_info_request (offer, std::string (invalid)));
		auto const response (wait_response (system, rpc_ctx, request, 10s));
		ASSERT_EQ ("Invalid scan_count", response.get<std::string> ("error"));
		ASSERT_EQ (invalid, response.get<std::string> ("requested"));
		ASSERT_EQ ("scan_count", response.get<std::string> ("field"));
		ASSERT_EQ ("1024", response.get<std::string> ("maxAllowed"));
	}
}
