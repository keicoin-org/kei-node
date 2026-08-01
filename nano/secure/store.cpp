#include <nano/lib/threading.hpp>
#include <nano/lib/timer.hpp>
#include <nano/secure/store.hpp>

nano::representative_visitor::representative_visitor (nano::transaction const & transaction_a, nano::store & store_a) :
	transaction (transaction_a),
	store (store_a),
	result (0)
{
}

void nano::representative_visitor::compute (nano::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block (store.block.get (transaction, current));
		debug_assert (block != nullptr);
		block->visit (*this);
	}
}

void nano::representative_visitor::send_block (nano::send_block const & block_a)
{
	current = block_a.previous ();
}

void nano::representative_visitor::receive_block (nano::receive_block const & block_a)
{
	current = block_a.previous ();
}

void nano::representative_visitor::open_block (nano::open_block const & block_a)
{
	result = block_a.hash ();
}

void nano::representative_visitor::change_block (nano::change_block const & block_a)
{
	result = block_a.hash ();
}

void nano::representative_visitor::state_block (nano::state_block const & block_a)
{
	result = block_a.hash ();
}

void nano::representative_visitor::asset_block (nano::asset_block const & block_a)
{
	// Carries a representative field unconditionally, same as state_block
	// (decisions-m2.md §7).
	result = block_a.hash ();
}

nano::read_transaction::read_transaction (std::unique_ptr<nano::read_transaction_impl> read_transaction_impl) :
	impl (std::move (read_transaction_impl))
{
}

void * nano::read_transaction::get_handle () const
{
	return impl->get_handle ();
}

void nano::read_transaction::reset () const
{
	impl->reset ();
}

void nano::read_transaction::renew () const
{
	impl->renew ();
}

void nano::read_transaction::refresh () const
{
	reset ();
	renew ();
}

nano::write_transaction::write_transaction (std::unique_ptr<nano::write_transaction_impl> write_transaction_impl) :
	impl (std::move (write_transaction_impl))
{
	/*
	 * For IO threads, we do not want them to block on creating write transactions.
	 */
	debug_assert (nano::thread_role::get () != nano::thread_role::name::io);
}

void * nano::write_transaction::get_handle () const
{
	return impl->get_handle ();
}

void nano::write_transaction::commit ()
{
	impl->commit ();
}

void nano::write_transaction::renew ()
{
	impl->renew ();
}

void nano::write_transaction::refresh ()
{
	impl->commit ();
	impl->renew ();
}

bool nano::write_transaction::contains (nano::tables table_a) const
{
	return impl->contains (table_a);
}

// clang-format off
nano::store::store (
	nano::block_store & block_store_a,
	nano::frontier_store & frontier_store_a,
	nano::account_store & account_store_a,
	nano::pending_store & pending_store_a,
	nano::online_weight_store & online_weight_store_a,
	nano::pruned_store & pruned_store_a,
	nano::peer_store & peer_store_a,
	nano::confirmation_height_store & confirmation_height_store_a,
	nano::final_vote_store & final_vote_store_a,
	nano::version_store & version_store_a,
	nano::asset_store & asset_store_a
) :
	block (block_store_a),
	frontier (frontier_store_a),
	account (account_store_a),
	pending (pending_store_a),
	online_weight (online_weight_store_a),
	pruned (pruned_store_a),
	peer (peer_store_a),
	confirmation_height (confirmation_height_store_a),
	final_vote (final_vote_store_a),
	version (version_store_a),
	asset (asset_store_a)
{
}
// clang-format on

/**
 * If using a different store version than the latest then you may need
 * to modify some of the objects in the store to be appropriate for the version before an upgrade.
 */
void nano::store::initialize (nano::write_transaction const & transaction_a, nano::ledger_cache & ledger_cache_a, nano::ledger_constants & constants)
{
	debug_assert (constants.genesis->has_sideband ());
	debug_assert (account.begin (transaction_a) == account.end ());
	auto const genesis_hash (constants.genesis->hash ());
	auto const timestamp (nano::seconds_since_epoch ());
	block.put (transaction_a, genesis_hash, *constants.genesis);

	// Beta/live have no ceremony data until their offline genesis ceremony has
	// happened (and ledger_constants refuses their placeholder before reaching
	// here). The dev network has four fixed sends and matching opens. Installing
	// those blocks as the immutable initial history is the same special operation
	// as installing the genesis open itself: ordinary ledger processing begins
	// only after the reserve has paid the four allocations and become locked.
	uint64_t reserve_height{ 1 };
	nano::block_hash reserve_head (genesis_hash);
	nano::amount reserve_balance (constants.genesis_amount);
	for (auto const & allocation : constants.genesis_allocations)
	{
		++reserve_height;
		reserve_head = allocation.send->hash ();
		reserve_balance = allocation.send->balance ();
		allocation.send->sideband_set (nano::block_sideband (constants.genesis->account (), 0, 0, reserve_height, timestamp, nano::block_details (nano::epoch::epoch_2, true, false, false), nano::epoch::epoch_0));
		block.put (transaction_a, reserve_head, *allocation.send);

		auto const recipient (allocation.open->account ());
		auto const open_hash (allocation.open->hash ());
		allocation.open->sideband_set (nano::block_sideband (recipient, 0, 0, 1, timestamp, nano::block_details (nano::epoch::epoch_2, false, true, false), nano::epoch::epoch_2));
		block.put (transaction_a, open_hash, *allocation.open);
		account.put (transaction_a, recipient, { open_hash, recipient, open_hash, allocation.amount, timestamp, 1, nano::epoch::epoch_2 });
		confirmation_height.put (transaction_a, recipient, nano::confirmation_height_info{ 1, open_hash });
		ledger_cache_a.rep_weights.representation_put (recipient, allocation.amount);
	}

	ledger_cache_a.block_count += 1 + (constants.genesis_allocations.size () * 2);
	ledger_cache_a.cemented_count += 1 + (constants.genesis_allocations.size () * 2);
	ledger_cache_a.account_count += 1 + constants.genesis_allocations.size ();
	confirmation_height.put (transaction_a, constants.genesis->account (), nano::confirmation_height_info{ reserve_height, reserve_head });
	ledger_cache_a.final_votes_confirmation_canary = (constants.final_votes_canary_account == constants.genesis->account () && reserve_height >= constants.final_votes_canary_height);
	// The epoch the genesis account records has to be the one its own sideband
	// carries, which for Kei is epoch 2 (nano/secure/common.cpp). These are two
	// separate writes of the same fact and only this one is read when the next
	// block on the chain is validated, so leaving it at epoch 0 kept the whole
	// ledger at epoch 0 no matter what the sideband said — the tiers in
	// decisions-m2.md §11 stayed unreachable and every opening block a client
	// signed was refused for insufficient work.
	account.put (transaction_a, constants.genesis->account (), { reserve_head, constants.genesis->representative (), genesis_hash, reserve_balance, timestamp, reserve_height, constants.genesis->sideband ().details.epoch });
	// No representation entry for reserve: the null representative is not a
	// bucket holding 90% of supply; reserve Kei contributes zero weight of any
	// kind. Circulating accounts were added above under their own representatives.
	if (!constants.is_reserve (constants.genesis->account ()) && !constants.genesis->representative ().is_zero ())
	{
		ledger_cache_a.rep_weights.representation_put (constants.genesis->representative (), reserve_balance);
	}
	// Every account frontier is a state block after the ceremony. The inherited
	// frontier table indexes only legacy blocks, so none is inserted here.
	if (constants.genesis_allocations.empty ())
	{
		frontier.put (transaction_a, genesis_hash, constants.genesis->account ());
	}
}

std::optional<nano::account_info> nano::account_store::get (const nano::transaction & transaction, const nano::account & account)
{
	nano::account_info info;
	bool error = get (transaction, account, info);
	if (!error)
	{
		return info;
	}
	else
	{
		return std::nullopt;
	}
}

std::optional<nano::confirmation_height_info> nano::confirmation_height_store::get (const nano::transaction & transaction, const nano::account & account)
{
	nano::confirmation_height_info info;
	bool error = get (transaction, account, info);
	if (!error)
	{
		return info;
	}
	else
	{
		return std::nullopt;
	}
}
