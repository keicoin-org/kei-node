#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>
#include <nano/test_common/ledger.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace
{
std::array<nano::keypair const *, 4> const circulating_keys{
	&nano::dev::grants_key,
	&nano::dev::community_key,
	&nano::dev::bounty_key,
	&nano::dev::team_key
};

std::array<nano::uint128_t, 4> const circulating_amounts{
	nano::dev::constants.allocation_grants (),
	nano::dev::constants.allocation_community (),
	nano::dev::constants.allocation_bounty (),
	nano::dev::constants.allocation_team ()
};
}

TEST (genesis_reserve, dev_ceremony_installs_the_set_and_exact_allocation)
{
	nano::test::context::ledger_context context;
	auto & ledger (context.ledger ());
	auto & store (context.store ());
	auto transaction (store.tx_begin_read ());
	auto const reserve (nano::dev::genesis_key.pub);

	ASSERT_EQ (nano::kei_total_supply, nano::dev::constants.genesis_amount);
	ASSERT_EQ (1, nano::dev::constants.reserve_accounts.size ());
	ASSERT_EQ (reserve, nano::dev::constants.reserve_accounts.front ());
	ASSERT_TRUE (nano::dev::constants.is_reserve (reserve));
	ASSERT_TRUE (nano::dev::genesis->representative ().is_zero ());
	ASSERT_FALSE (nano::validate_message (reserve, nano::dev::genesis->hash (), nano::dev::genesis->block_signature ()));
	ASSERT_GE (nano::dev::constants.work.difficulty (*nano::dev::genesis), nano::dev::constants.work.epoch_1);
	ASSERT_EQ (4, nano::dev::constants.genesis_allocations.size ());

	auto const reserve_info (ledger.account_info (transaction, reserve));
	ASSERT_TRUE (reserve_info.has_value ());
	ASSERT_TRUE (reserve_info->representative.is_zero ());
	ASSERT_EQ (nano::dev::constants.allocation_reserve (), reserve_info->balance.number ());
	ASSERT_EQ (5, reserve_info->block_count);
	ASSERT_EQ (nano::dev::constants.genesis_allocations.back ().send->hash (), reserve_info->head);
	nano::confirmation_height_info reserve_confirmation;
	ASSERT_FALSE (store.confirmation_height.get (transaction, reserve, reserve_confirmation));
	ASSERT_EQ (5, reserve_confirmation.height);
	ASSERT_EQ (reserve_info->head, reserve_confirmation.frontier);
	ASSERT_EQ (0, ledger.weight (reserve));
	ASSERT_EQ (0, ledger.weight (nano::account{}));

	nano::uint128_t circulating{ 0 };
	for (std::size_t i = 0; i < circulating_keys.size (); ++i)
	{
		auto const account (circulating_keys[i]->pub);
		ASSERT_FALSE (nano::dev::constants.is_reserve (account));
		auto const info (ledger.account_info (transaction, account));
		ASSERT_TRUE (info.has_value ());
		ASSERT_EQ (circulating_amounts[i], info->balance.number ());
		ASSERT_EQ (account, info->representative);
		ASSERT_EQ (circulating_amounts[i], ledger.weight (account));
		nano::confirmation_height_info confirmation;
		ASSERT_FALSE (store.confirmation_height.get (transaction, account, confirmation));
		ASSERT_EQ (1, confirmation.height);
		ASSERT_EQ (info->head, confirmation.frontier);
		ASSERT_TRUE (store.block.exists (transaction, nano::dev::constants.genesis_allocations[i].send->hash ()));
		ASSERT_TRUE (store.block.exists (transaction, nano::dev::constants.genesis_allocations[i].open->hash ()));
		ASSERT_FALSE (nano::validate_message (reserve, nano::dev::constants.genesis_allocations[i].send->hash (), nano::dev::constants.genesis_allocations[i].send->block_signature ()));
		ASSERT_FALSE (nano::validate_message (account, nano::dev::constants.genesis_allocations[i].open->hash (), nano::dev::constants.genesis_allocations[i].open->block_signature ()));
		ASSERT_GE (nano::dev::constants.work.difficulty (*nano::dev::constants.genesis_allocations[i].send), nano::dev::constants.work.tier_b ());
		ASSERT_GE (nano::dev::constants.work.difficulty (*nano::dev::constants.genesis_allocations[i].open), nano::dev::constants.work.tier_c ());
		ASSERT_EQ (nano::dev::constants.genesis_allocations[i].send->hash (), nano::dev::constants.genesis_allocations[i].open->link ().as_block_hash ());
		circulating += info->balance.number ();
	}

	ASSERT_EQ (nano::uint128_t ("100000000000") * nano::BAN_ratio, circulating);
	ASSERT_EQ (nano::kei_total_supply, reserve_info->balance.number () + circulating);
	ASSERT_EQ (9, ledger.cache.block_count.load ());
	ASSERT_EQ (9, ledger.cache.cemented_count.load ());
	ASSERT_EQ (5, ledger.cache.account_count.load ());
}

TEST (genesis_reserve, ordinary_blocks_cannot_delegate_send_or_issue)
{
	nano::test::context::ledger_context context;
	auto & ledger (context.ledger ());
	auto & store (context.store ());
	auto const reserve (nano::dev::genesis_key.pub);
	std::optional<nano::account_info> reserve_info;
	{
		auto read_transaction (store.tx_begin_read ());
		reserve_info = ledger.account_info (read_transaction, reserve);
	}
	ASSERT_TRUE (reserve_info.has_value ());

	nano::block_builder builder;
	auto bad_representative = builder.state ()
		.make_block ()
		.account (reserve)
		.previous (reserve_info->head)
		.representative (nano::dev::team_key.pub)
		.balance (reserve_info->balance)
		.link (0)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (0)
		.build ();
	ASSERT_EQ (nano::process_result::reserve_representative, ledger.process (store.tx_begin_write (), *bad_representative).code);

	auto send = std::make_shared<nano::state_block> (reserve, reserve_info->head, nano::account{}, nano::amount (reserve_info->balance.number () - 1), nano::dev::team_key.pub, nano::dev::genesis_key.prv, nano::dev::genesis_key.pub, 0);
	ASSERT_EQ (nano::process_result::reserve_locked, ledger.process (store.tx_begin_write (), *send).code);

	nano::asset_payload payload;
	payload.name = "Forbidden";
	payload.symbol = "NOPE";
	payload.decimals = 0;
	payload.max_supply = 1;
	payload.transfer = nano::transfer_policy::open;
	payload.swap = nano::swap_policy::off;
	payload.kind = nano::asset_kind::token;
	auto const asset_id (nano::derive_asset_id (reserve, payload.symbol));
	auto issue = std::make_shared<nano::asset_block> (reserve, reserve_info->head, nano::account{}, nano::amount (reserve_info->balance.number () - nano::issuance_burn (0)), nano::asset_op::issue, asset_id, 0, 0, payload, nano::dev::genesis_key.prv, nano::dev::genesis_key.pub, 0);
	ASSERT_EQ (nano::process_result::reserve_locked, ledger.process (store.tx_begin_write (), *issue).code);
}

TEST (genesis_reserve, rebuilding_the_weight_cache_keeps_reserve_excluded)
{
	nano::test::context::ledger_context context;
	nano::stats rebuilt_stats;
	nano::ledger rebuilt (context.store (), rebuilt_stats, nano::dev::constants);

	ASSERT_EQ (0, rebuilt.weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, rebuilt.weight (nano::account{}));
	for (std::size_t i = 0; i < circulating_keys.size (); ++i)
	{
		ASSERT_EQ (circulating_amounts[i], rebuilt.weight (circulating_keys[i]->pub));
	}
}

TEST (genesis_reserve, rebuilding_preserves_a_circulating_null_representative)
{
	nano::test::context::ledger_context context;
	auto & ledger (context.ledger ());
	auto & store (context.store ());
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const account (nano::dev::team_key.pub);
	auto const balance (nano::dev::constants.allocation_team ());
	auto const initial_head (nano::dev::constants.genesis_allocations.back ().open->hash ());
	auto change_to_null = std::make_shared<nano::state_block> (account, initial_head, nano::account{}, nano::amount (balance), 0, nano::dev::team_key.prv, account, 0);
	change_to_null->block_work_set (*pool.generate (change_to_null->root (), nano::dev::constants.work.tier_b ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (store.tx_begin_write (), *change_to_null).code);
	ASSERT_EQ (0, ledger.weight (account));
	ASSERT_EQ (balance, ledger.weight (nano::account{}));

	nano::stats rebuilt_stats;
	nano::ledger rebuilt (store, rebuilt_stats, nano::dev::constants);
	ASSERT_EQ (0, rebuilt.weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, rebuilt.weight (account));
	ASSERT_EQ (balance, rebuilt.weight (nano::account{}));

	auto change_back = std::make_shared<nano::state_block> (account, change_to_null->hash (), account, nano::amount (balance), 0, nano::dev::team_key.prv, account, 0);
	change_back->block_work_set (*pool.generate (change_back->root (), nano::dev::constants.work.tier_b ()));
	ASSERT_EQ (nano::process_result::progress, rebuilt.process (store.tx_begin_write (), *change_back).code);
	ASSERT_EQ (balance, rebuilt.weight (account));
	ASSERT_EQ (0, rebuilt.weight (nano::account{}));

	ASSERT_FALSE (rebuilt.rollback (store.tx_begin_write (), change_back->hash ()));
	ASSERT_EQ (0, rebuilt.weight (account));
	ASSERT_EQ (balance, rebuilt.weight (nano::account{}));
}

TEST (genesis_reserve, receive_and_rollback_never_give_reserve_weight)
{
	nano::test::context::ledger_context context;
	auto & ledger (context.ledger ());
	auto & store (context.store ());
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	auto const reserve (nano::dev::genesis_key.pub);
	auto const sender (nano::dev::team_key.pub);
	auto const reserve_start (nano::dev::constants.allocation_reserve ());
	auto const sender_start (nano::dev::constants.allocation_team ());
	auto const reserve_head (nano::dev::constants.genesis_allocations.back ().send->hash ());
	auto const sender_head (nano::dev::constants.genesis_allocations.back ().open->hash ());

	auto send = std::make_shared<nano::state_block> (sender, sender_head, sender, nano::amount (sender_start - 1), reserve, nano::dev::team_key.prv, sender, 0);
	send->block_work_set (*pool.generate (send->root (), nano::dev::constants.work.tier_b ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (store.tx_begin_write (), *send).code);

	auto receive = std::make_shared<nano::state_block> (reserve, reserve_head, nano::account{}, nano::amount (reserve_start + 1), send->hash (), nano::dev::genesis_key.prv, reserve, 0);
	receive->block_work_set (*pool.generate (receive->root (), nano::dev::constants.work.tier_c ()));
	ASSERT_EQ (nano::process_result::progress, ledger.process (store.tx_begin_write (), *receive).code);
	ASSERT_EQ (0, ledger.weight (reserve));
	ASSERT_EQ (0, ledger.weight (nano::account{}));
	ASSERT_EQ (sender_start - 1, ledger.weight (sender));

	ASSERT_FALSE (ledger.rollback (store.tx_begin_write (), receive->hash ()));
	ASSERT_EQ (0, ledger.weight (reserve));
	ASSERT_EQ (0, ledger.weight (nano::account{}));
	ASSERT_EQ (sender_start - 1, ledger.weight (sender));
}
