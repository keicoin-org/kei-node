#include <nano/lib/rep_weights.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/store.hpp>

#include <boost/optional.hpp>

#include <cryptopp/words.h>

#include <limits>

namespace
{
/** The three ops whose Kei balance rule depends on which asset the offer locked. */
bool swap_leg (nano::asset_op op_a)
{
	return op_a == nano::asset_op::swap_offer || op_a == nano::asset_op::swap_accept || op_a == nano::asset_op::swap_cancel;
}

/**
 * Whether one leg of a swap may move `asset_a` from `from_a` to `to_a`.
 *
 * A swap leg is a transfer with a second leg attached, so it answers to the
 * same immutable policy (SPEC §5.4) — the SDK does not get to trade its way
 * around a soulbound item. Either party may be the zero account, which is what
 * an offer with no named counterparty looks like before anyone accepts it:
 * `issuer_only` then passes only when the *known* side is the issuer, so an
 * offer nobody could legally accept is refused at the offer rather than left
 * open forever.
 */
nano::process_result swap_leg_permitted (nano::asset_info const & asset_a, nano::account const & from_a, nano::account const & to_a)
{
	switch (asset_a.transfer)
	{
		case nano::transfer_policy::open:
			return nano::process_result::progress;
		case nano::transfer_policy::issuer_only:
			return (from_a == asset_a.issuer || to_a == asset_a.issuer) ? nano::process_result::progress : nano::process_result::transfer_not_permitted;
		case nano::transfer_policy::none:
			// Soulbound: it can only be burned, so it cannot be sold either.
			return nano::process_result::transfer_not_permitted;
	}
	return nano::process_result::transfer_not_permitted;
}

/**
 * Rebuild a lock record from the `swap_offer` block that created it.
 *
 * Every field of the lock is a field of the offer, so the record is derived
 * rather than journalled — which is what lets a `swap_cancel` delete it outright
 * and a rollback put back exactly what was there.
 */
nano::asset_lock_info lock_from_offer (nano::asset_block const & offer_a)
{
	nano::asset_lock_info lock;
	lock.offerer = offer_a.hashables.account;
	lock.asset_id = offer_a.hashables.asset_id;
	lock.amount = offer_a.hashables.amount;
	lock.want_asset = offer_a.hashables.payload.want_asset;
	lock.want_amount = offer_a.hashables.payload.want_amount;
	lock.counterparty = offer_a.hashables.link.as_account ();
	lock.expires_at = offer_a.hashables.payload.expires_at;
	return lock;
}

/**
 * One receivable a block creates, before it is known whether the block is valid.
 *
 * A `swap_accept` creates two of these — one per leg, on two different chains —
 * which is why this is a list where every op before M5 needed a single optional
 * (SPEC §9.2). Kei arrivals go to the inherited `pending` table and asset
 * arrivals to `asset_pending`, told apart by the zero asset id rather than by a
 * flag on a shared record (decisions-m5.md §3).
 */
struct staged_arrival final
{
	nano::account to;
	nano::uint256_union asset_id;
	nano::amount amount;
	nano::account source;
	std::string memo;
};

/**
 * Roll back the visited block
 */
class rollback_visitor : public nano::block_visitor
{
public:
	rollback_visitor (nano::write_transaction const & transaction_a, nano::ledger & ledger_a, std::vector<std::shared_ptr<nano::block>> & list_a) :
		transaction (transaction_a),
		ledger (ledger_a),
		list (list_a)
	{
	}
	virtual ~rollback_visitor () = default;
	void send_block (nano::send_block const & block_a) override
	{
		auto hash (block_a.hash ());
		nano::pending_info pending;
		nano::pending_key key (block_a.hashables.destination, hash);
		while (!error && ledger.store.pending.get (transaction, key, pending))
		{
			error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination), list);
		}
		if (!error)
		{
			auto info = ledger.account_info (transaction, pending.source);
			debug_assert (info);
			ledger.store.pending.del (transaction, key);
			ledger.cache.rep_weights.representation_add (info->representative, pending.amount.number ());
			nano::account_info new_info (block_a.hashables.previous, info->representative, info->open_block, ledger.balance (transaction, block_a.hashables.previous), nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
			ledger.update_account (transaction, pending.source, *info, new_info);
			ledger.store.block.del (transaction, hash);
			ledger.store.frontier.del (transaction, hash);
			ledger.store.frontier.put (transaction, block_a.hashables.previous, pending.source);
			ledger.store.block.successor_clear (transaction, block_a.hashables.previous);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
		}
	}
	void receive_block (nano::receive_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, hash));
		auto destination_account (ledger.account (transaction, hash));
		// Pending account entry can be incorrect if source block was pruned. But it's not affecting correct ledger processing
		[[maybe_unused]] bool is_pruned (false);
		auto source_account (ledger.account_safe (transaction, block_a.hashables.source, is_pruned));
		auto info = ledger.account_info (transaction, destination_account);
		debug_assert (info);
		ledger.cache.rep_weights.representation_add (info->representative, 0 - amount);
		nano::account_info new_info (block_a.hashables.previous, info->representative, info->open_block, ledger.balance (transaction, block_a.hashables.previous), nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
		ledger.update_account (transaction, destination_account, *info, new_info);
		ledger.store.block.del (transaction, hash);
		ledger.store.pending.put (transaction, nano::pending_key (destination_account, block_a.hashables.source), { source_account, amount, nano::epoch::epoch_0 });
		ledger.store.frontier.del (transaction, hash);
		ledger.store.frontier.put (transaction, block_a.hashables.previous, destination_account);
		ledger.store.block.successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
	}
	void open_block (nano::open_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, hash));
		auto destination_account (ledger.account (transaction, hash));
		// Pending account entry can be incorrect if source block was pruned. But it's not affecting correct ledger processing
		[[maybe_unused]] bool is_pruned (false);
		auto source_account (ledger.account_safe (transaction, block_a.hashables.source, is_pruned));
		ledger.cache.rep_weights.representation_add (block_a.representative (), 0 - amount);
		nano::account_info new_info;
		ledger.update_account (transaction, destination_account, new_info, new_info);
		ledger.store.block.del (transaction, hash);
		ledger.store.pending.put (transaction, nano::pending_key (destination_account, block_a.hashables.source), { source_account, amount, nano::epoch::epoch_0 });
		ledger.store.frontier.del (transaction, hash);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
	}
	void change_block (nano::change_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto rep_block (ledger.representative (transaction, block_a.hashables.previous));
		auto account (ledger.account (transaction, block_a.hashables.previous));
		auto info = ledger.account_info (transaction, account);
		debug_assert (info);
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto block = ledger.store.block.get (transaction, rep_block);
		release_assert (block != nullptr);
		auto representative = block->representative ();
		ledger.cache.rep_weights.representation_add_dual (block_a.representative (), 0 - balance, representative, balance);
		ledger.store.block.del (transaction, hash);
		nano::account_info new_info (block_a.hashables.previous, representative, info->open_block, info->balance, nano::seconds_since_epoch (), info->block_count - 1, nano::epoch::epoch_0);
		ledger.update_account (transaction, account, *info, new_info);
		ledger.store.frontier.del (transaction, hash);
		ledger.store.frontier.put (transaction, block_a.hashables.previous, account);
		ledger.store.block.successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::change);
	}
	void state_block (nano::state_block const & block_a) override
	{
		auto hash (block_a.hash ());
		nano::block_hash rep_block_hash (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			rep_block_hash = ledger.representative (transaction, block_a.hashables.previous);
		}
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto is_send (block_a.hashables.balance < balance);
		nano::account representative{};
		if (!rep_block_hash.is_zero ())
		{
			auto block (ledger.store.block.get (transaction, rep_block_hash));
			debug_assert (block != nullptr);
			representative = block->representative ();
			if (!ledger.constants.is_reserve (block_a.hashables.account))
			{
				// Move existing representation & add in amount delta.
				ledger.cache.rep_weights.representation_add_dual (representative, balance, block_a.representative (), 0 - block_a.hashables.balance.number ());
			}
		}
		else if (!ledger.constants.is_reserve (block_a.hashables.account))
		{
			// Add in amount delta only. Reserve accounts never enter this cache,
			// including when an ordinary receive is rolled back.
			ledger.cache.rep_weights.representation_add (block_a.representative (), 0 - block_a.hashables.balance.number ());
		}

		auto info = ledger.account_info (transaction, block_a.hashables.account);
		debug_assert (info);

		if (is_send)
		{
			nano::pending_key key (block_a.hashables.link.as_account (), hash);
			while (!error && !ledger.store.pending.exists (transaction, key))
			{
				error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.link.as_account ()), list);
			}
			ledger.store.pending.del (transaction, key);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
		}
		else if (!block_a.hashables.link.is_zero () && !ledger.is_epoch_link (block_a.hashables.link))
		{
			// Pending account entry can be incorrect if source block was pruned. But it's not affecting correct ledger processing
			[[maybe_unused]] bool is_pruned (false);
			auto source_account (ledger.account_safe (transaction, block_a.hashables.link.as_block_hash (), is_pruned));
			nano::pending_info pending_info (source_account, block_a.hashables.balance.number () - balance, block_a.sideband ().source_epoch);
			ledger.store.pending.put (transaction, nano::pending_key (block_a.hashables.account, block_a.hashables.link.as_block_hash ()), pending_info);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
		}

		debug_assert (!error);
		auto previous_version (ledger.store.block.version (transaction, block_a.hashables.previous));
		nano::account_info new_info (block_a.hashables.previous, representative, info->open_block, balance, nano::seconds_since_epoch (), info->block_count - 1, previous_version);
		ledger.update_account (transaction, block_a.hashables.account, *info, new_info);

		auto previous (ledger.store.block.get (transaction, block_a.hashables.previous));
		if (previous != nullptr)
		{
			ledger.store.block.successor_clear (transaction, block_a.hashables.previous);
			if (previous->type () < nano::block_type::state)
			{
				ledger.store.frontier.put (transaction, block_a.hashables.previous, block_a.hashables.account);
			}
		}
		else
		{
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
		}
		ledger.store.block.del (transaction, hash);
	}
	/**
	 * Undo one asset block, exactly reversing what ledger_processor did.
	 *
	 * Every op is invertible from the block alone, which is why none of them
	 * needs a journal: a mint's amount is in the block, a burn's amount is in
	 * the block, and `asset_receive` recovers what it collected by looking the
	 * source block back up.
	 */
	void asset_block (nano::asset_block const & block_a) override
	{
		auto const hash (block_a.hash ());
		nano::account_info info;
		// Not named `error`: that is the visitor's own member, which
		// take_back_receivable below both reads and sets.
		[[maybe_unused]] auto const account_error (ledger.store.account.get (transaction, block_a.hashables.account, info));
		debug_assert (!account_error);

		auto asset_id (block_a.hashables.asset_id);
		auto const amount (block_a.hashables.amount.number ());
		nano::asset_info asset;

		switch (block_a.hashables.op)
		{
			case nano::asset_op::issue:
			{
				// The record was created by this block and nothing can have
				// been minted against it, because a mint must build on a later
				// block in the same chain and rollback works backwards.
				ledger.store.asset.del (transaction, asset_id);
				// The issuance count has to come back down with it, or the
				// account's next asset would be priced as though this one still
				// existed. Rollback is backwards along one chain, so this block
				// is the account's most recent issuance and the count is its
				// ordinal.
				auto const issued (ledger.store.asset.issued_count (transaction, block_a.hashables.account));
				debug_assert (issued > 0);
				ledger.store.asset.issued_put (transaction, block_a.hashables.account, issued - 1);
				break;
			}
			case nano::asset_op::mint:
				take_back_receivable (block_a, hash);
				if (!ledger.store.asset.get (transaction, asset_id, asset))
				{
					asset.circulating = asset.circulating.number () - amount;
					ledger.store.asset.put (transaction, asset_id, asset);
				}
				break;
			case nano::asset_op::burn:
				if (!ledger.store.asset.get (transaction, asset_id, asset))
				{
					asset.circulating = asset.circulating.number () + amount;
					ledger.store.asset.put (transaction, asset_id, asset);
				}
				restore (block_a.hashables.account, asset_id, amount);
				break;
			case nano::asset_op::transfer:
				take_back_receivable (block_a, hash);
				restore (block_a.hashables.account, asset_id, amount);
				break;
			case nano::asset_op::asset_receive:
			{
				// Put the receivable back exactly as it was, which means
				// reading the source block for the source account and the memo.
				auto const source_hash (block_a.hashables.link.as_block_hash ());
				[[maybe_unused]] bool source_is_pruned (false);
				[[maybe_unused]] auto const source_account (ledger.account_safe (transaction, source_hash, source_is_pruned));
				if (source_is_pruned)
				{
					error = true;
					break;
				}
				auto const source (ledger.store.block.get (transaction, source_hash));
				if (source == nullptr)
				{
					error = true;
					break;
				}
				auto const source_asset (dynamic_cast<nano::asset_block const *> (source.get ()));
				if (source_asset == nullptr)
				{
					error = true;
					break;
				}
				asset_id = source_asset->hashables.asset_id;
				auto const collected (source_asset->hashables.amount.number ());
				auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
				debug_assert (held >= collected);
				if (held == collected)
				{
					ledger.store.asset.balance_del (transaction, block_a.hashables.account, asset_id);
				}
				else
				{
					ledger.store.asset.balance_put (transaction, block_a.hashables.account, asset_id, nano::amount (held - collected));
				}
				ledger.store.asset.pending_put (transaction, nano::pending_key (block_a.hashables.account, source_hash), nano::asset_pending_info (source_account, asset_id, collected, source_asset->hashables.payload.memo));
				break;
			}
			case nano::asset_op::commit:
			{
				auto const root (block_a.hashables.link.as_block_hash ());
				// Claims against this root live on other accounts' chains, and
				// nothing orders those against this one. They have to come off
				// first, exactly as a collected mint's receive does — and unlike
				// a receivable, a claim leaves no per-recipient key to find them
				// by, which is what the root-keyed index exists for.
				undo_claims (root);
				if (!error)
				{
					ledger.store.asset.commit_del (transaction, root);
				}
				break;
			}
			case nano::asset_op::commit_close:
			{
				auto const root (block_a.hashables.link.as_block_hash ());
				nano::asset_commit_info commit;
				auto const commit_error (ledger.store.asset.commit_get (transaction, root, commit));
				release_assert (!commit_error);
				commit.closed = false;
				ledger.store.asset.commit_put (transaction, root, commit);
				break;
			}
			case nano::asset_op::claim:
			{
				auto const root (block_a.hashables.link.as_block_hash ());
				if (!ledger.store.asset.get (transaction, asset_id, asset))
				{
					asset.circulating = asset.circulating.number () - amount;
					ledger.store.asset.put (transaction, asset_id, asset);
				}
				auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
				debug_assert (held >= amount);
				if (held == amount)
				{
					ledger.store.asset.balance_del (transaction, block_a.hashables.account, asset_id);
				}
				else
				{
					ledger.store.asset.balance_put (transaction, block_a.hashables.account, asset_id, nano::amount (held - amount));
				}
				ledger.store.asset.claim_del (transaction, block_a.hashables.account, root);
				break;
			}
			case nano::asset_op::swap_offer:
			{
				nano::asset_lock_info lock;
				auto exists (!ledger.store.asset.lock_get (transaction, hash, lock));
				// Settled by an accept on the counterparty's chain, which nothing
				// orders against this one — every block that chain grew since the
				// accept has to come off first, the same reason
				// `take_back_receivable` loops instead of rolling back one block.
				// The lock is re-read each pass because the accept's own rollback
				// (below) is what flips `open ()` back to true; nothing else
				// touches this record from over there.
				while (!error && exists && !lock.open ())
				{
					[[maybe_unused]] bool settler_is_pruned (false);
					auto const settler (ledger.account_safe (transaction, lock.settled_by, settler_is_pruned));
					if (settler_is_pruned)
					{
						error = true;
						break;
					}
					error = ledger.rollback (transaction, ledger.latest (transaction, settler), list);
					exists = !error && !ledger.store.asset.lock_get (transaction, hash, lock);
				}
				if (!error && exists)
				{
					ledger.store.asset.lock_del (transaction, hash);
				}
				// If the lock is already gone, a `swap_cancel` deleted it — and
				// that block sits later on this same chain, so ordinary
				// tip-first rollback has already undone it and put the lock
				// back before reaching this block.
				if (!error)
				{
					ledger.store.asset.offer_del (transaction, asset_id, hash);
					if (!asset_id.is_zero ())
					{
						restore (block_a.hashables.account, asset_id, amount);
					}
				}
				break;
			}
			case nano::asset_op::swap_accept:
			{
				auto const offer_hash (block_a.hashables.link.as_block_hash ());
				nano::asset_lock_info lock;
				auto const missing (ledger.store.asset.lock_get (transaction, offer_hash, lock));
				if (missing)
				{
					error = true;
					break;
				}
				// Both arrivals this block created might already be collected,
				// on either side — each a different chain than this one.
				take_back_arrival (lock.offerer, lock.want_asset, hash);
				if (!error)
				{
					take_back_arrival (block_a.hashables.account, lock.asset_id, hash);
				}
				if (!error)
				{
					if (!asset_id.is_zero ())
					{
						restore (block_a.hashables.account, asset_id, amount);
					}
					lock.settled_by.clear ();
					ledger.store.asset.lock_put (transaction, offer_hash, lock);
					ledger.store.asset.offer_put (transaction, lock.asset_id, offer_hash, lock.offerer);
				}
				break;
			}
			case nano::asset_op::swap_cancel:
			{
				auto const offer_hash (block_a.hashables.link.as_block_hash ());
				nano::asset_lock_info lock;
				if (ledger.store.asset.lock_get (transaction, offer_hash, lock))
				{
					auto const offer_block (ledger.store.block.get (transaction, offer_hash));
					if (offer_block == nullptr)
					{
						error = true;
						break;
					}
					auto const offer_asset (dynamic_cast<nano::asset_block const *> (offer_block.get ()));
					if (offer_asset == nullptr)
					{
						error = true;
						break;
					}
					lock = lock_from_offer (*offer_asset);
				}
				ledger.store.asset.lock_put (transaction, offer_hash, lock);
				ledger.store.asset.offer_put (transaction, lock.asset_id, offer_hash, lock.offerer);
				if (!lock.asset_id.is_zero ())
				{
					auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, lock.asset_id).number ());
					auto const locked (lock.amount.number ());
					debug_assert (held >= locked);
					if (held == locked)
					{
						ledger.store.asset.balance_del (transaction, block_a.hashables.account, lock.asset_id);
					}
					else
					{
						ledger.store.asset.balance_put (transaction, block_a.hashables.account, lock.asset_id, nano::amount (held - locked));
					}
				}
				// Kei restores itself: the generic balance rewrite below puts
				// `block_a.hashables.previous`'s balance back, and that is where
				// a cancelled Kei-denominated lock's amount already was.
				break;
			}
		}
		if (error)
		{
			// A rejected rollback has to leave *this* block where it found it.
			// Falling through would delete it and wind the account's head back
			// to its predecessor while `ledger::rollback ()` reports failure and
			// therefore leaves `cache.block_count` alone — a chain one block
			// shorter than the ledger believes it is, which no later block
			// repairs. The caller does not save us: `blockprocessor` logs
			// `rollback_failed` and carries on with the same write transaction,
			// so whatever a failed rollback wrote is committed.
			//
			// This is not a full undo and is not claimed as one. Any case whose
			// helper loops over nested `ledger.rollback ()` has already written
			// by the time it fails, because the earlier passes of that loop
			// succeeded and really did roll blocks off another account's chain.
			// At least these five, all reachable — `ledger::rollback ()` returns
			// true whenever the block sits at or below the confirmation height:
			//
			//   `mint`      -> take_back_receivable
			//   `transfer`  -> take_back_receivable
			//   `commit`    -> undo_claims
			//   `swap_offer` -> its settler loop
			//   `swap_accept` -> take_back_arrival
			//
			// `swap_accept` is worse than the rest: it calls take_back_arrival
			// twice, so the offerer's leg can be deleted outright and the
			// accepter's then fail, leaving this block claiming two arrivals of
			// which one is gone.
			//
			// Returning here stops that from compounding — the failing block is
			// no longer destroyed on top of it — and nothing more. The residual
			// is #52.
			return;
		}
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::asset_block);

		// ledger.representative () answers with the representative *block's*
		// hash, so the account comes from loading that block — the same two
		// steps the state rollback above takes.
		nano::block_hash rep_block_hash (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			rep_block_hash = ledger.representative (transaction, block_a.hashables.previous);
		}
		auto const previous_balance (ledger.balance (transaction, block_a.hashables.previous));
		nano::account representative{};
		if (!rep_block_hash.is_zero ())
		{
			auto const rep_block (ledger.store.block.get (transaction, rep_block_hash));
			debug_assert (rep_block != nullptr);
			representative = rep_block->representative ();
			ledger.cache.rep_weights.representation_add_dual (representative, previous_balance, block_a.representative (), 0 - block_a.hashables.balance.number ());
		}
		else
		{
			// The asset block opened this account, so there is no predecessor
			// weight to give back.
			ledger.cache.rep_weights.representation_add (block_a.representative (), 0 - block_a.hashables.balance.number ());
		}

		nano::account_info new_info (block_a.hashables.previous, representative, info.open_block, previous_balance, nano::seconds_since_epoch (), info.block_count - 1, info.epoch ());
		ledger.update_account (transaction, block_a.hashables.account, info, new_info);

		auto const previous (ledger.store.block.get (transaction, block_a.hashables.previous));
		if (previous != nullptr)
		{
			ledger.store.block.successor_clear (transaction, block_a.hashables.previous);
			if (previous->type () < nano::block_type::state)
			{
				ledger.store.frontier.put (transaction, block_a.hashables.previous, block_a.hashables.account);
			}
		}
		ledger.store.block.del (transaction, hash);
	}

	/**
	 * Withdraw the receivable a mint or a transfer created.
	 *
	 * The recipient may already have collected it, in which case their
	 * `asset_receive` has to come off their chain first — and that is not
	 * something this rollback can assume happened, because the two accounts are
	 * separate chains with no ordering between them. This is the same loop the
	 * state rollback above runs for an uncollected send, and without it a
	 * rollback of a collected mint deletes a key that is not there and takes the
	 * node down on the release assert.
	 */
	void take_back_receivable (nano::asset_block const & block_a, nano::block_hash const & hash_a)
	{
		auto const recipient (block_a.hashables.link.as_account ());
		nano::pending_key const key (recipient, hash_a);
		while (!error && !ledger.store.asset.pending_exists (transaction, key))
		{
			error = ledger.rollback (transaction, ledger.latest (transaction, recipient), list);
		}
		if (!error)
		{
			ledger.store.asset.pending_del (transaction, key);
		}
	}

	/**
	 * Withdraw one leg of a `swap_accept`'s pair of arrivals.
	 *
	 * The recipient may already have collected it, same as
	 * `take_back_receivable` — and unlike a mint or a transfer, a swap leg can
	 * be Kei itself (SPEC §9.2), which lives in a different table than an
	 * asset receivable, told apart the same way the apply side tells them
	 * apart: by the zero asset id.
	 */
	void take_back_arrival (nano::account const & to_a, nano::uint256_union const & asset_id_a, nano::block_hash const & hash_a)
	{
		nano::pending_key const key (to_a, hash_a);
		if (asset_id_a.is_zero ())
		{
			while (!error && !ledger.store.pending.exists (transaction, key))
			{
				error = ledger.rollback (transaction, ledger.latest (transaction, to_a), list);
			}
			if (!error)
			{
				ledger.store.pending.del (transaction, key);
			}
		}
		else
		{
			while (!error && !ledger.store.asset.pending_exists (transaction, key))
			{
				error = ledger.rollback (transaction, ledger.latest (transaction, to_a), list);
			}
			if (!error)
			{
				ledger.store.asset.pending_del (transaction, key);
			}
		}
	}

	/**
	 * Roll back every claim made against a root, so the commit under them can go.
	 *
	 * Each claim is another account's frontier problem: rolling one back means
	 * rolling back everything that account did afterwards, which is what
	 * `ledger.rollback` already does. The loop re-reads the index each time
	 * because those recursive rollbacks delete from it.
	 */
	void undo_claims (nano::uint256_union const & root_a)
	{
		while (!error)
		{
			auto iterator (ledger.store.asset.claim_roots_begin (transaction, nano::claim_root_key (root_a, 0)));
			if (iterator == ledger.store.asset.claim_roots_end () || iterator->first.first != root_a)
			{
				break;
			}
			nano::account const claimant (iterator->first.second.number ());
			error = ledger.rollback (transaction, ledger.latest (transaction, claimant), list);
		}
	}

	/** Give an account back what a burn or a transfer took from it. */
	void restore (nano::account const & account_a, nano::uint256_union const & asset_id_a, nano::uint128_t const & amount_a)
	{
		auto const held (ledger.store.asset.balance (transaction, account_a, asset_id_a).number ());
		ledger.store.asset.balance_put (transaction, account_a, asset_id_a, nano::amount (held + amount_a));
	}
	nano::write_transaction const & transaction;
	nano::ledger & ledger;
	std::vector<std::shared_ptr<nano::block>> & list;
	bool error{ false };
};

class ledger_processor : public nano::mutable_block_visitor
{
public:
	ledger_processor (nano::ledger &, nano::write_transaction const &);
	virtual ~ledger_processor () = default;
	void send_block (nano::send_block &) override;
	void receive_block (nano::receive_block &) override;
	void open_block (nano::open_block &) override;
	void change_block (nano::change_block &) override;
	void state_block (nano::state_block &) override;
	void state_block_impl (nano::state_block &);
	void epoch_block_impl (nano::state_block &);
	void asset_block (nano::asset_block &) override;
	nano::ledger & ledger;
	nano::write_transaction const & transaction;
	nano::process_return result;

private:
	bool validate_epoch_block (nano::state_block const & block_a);
};

// Returns true if this block which has an epoch link is correctly formed.
bool ledger_processor::validate_epoch_block (nano::state_block const & block_a)
{
	debug_assert (ledger.is_epoch_link (block_a.hashables.link));
	nano::amount prev_balance (0);
	if (!block_a.hashables.previous.is_zero ())
	{
		result.code = ledger.store.block.exists (transaction, block_a.hashables.previous) ? nano::process_result::progress : nano::process_result::gap_previous;
		if (result.code == nano::process_result::progress)
		{
			prev_balance = ledger.balance (transaction, block_a.hashables.previous);
		}
		else
		{
			// Check for possible regular state blocks with epoch link (send subtype)
			if (validate_message (block_a.hashables.account, block_a.hash (), block_a.signature))
			{
				// Is epoch block signed correctly
				if (validate_message (ledger.epoch_signer (block_a.link ()), block_a.hash (), block_a.signature))
				{
					result.code = nano::process_result::bad_signature;
				}
			}
		}
	}
	return (block_a.hashables.balance == prev_balance);
}

void ledger_processor::state_block (nano::state_block & block_a)
{
	result.code = nano::process_result::progress;
	auto is_epoch_block = false;
	if (ledger.is_epoch_link (block_a.hashables.link))
	{
		// This function also modifies the result variable if epoch is mal-formed
		is_epoch_block = validate_epoch_block (block_a);
	}

	if (result.code == nano::process_result::progress)
	{
		if (is_epoch_block)
		{
			epoch_block_impl (block_a);
		}
		else
		{
			state_block_impl (block_a);
		}
	}
}

void ledger_processor::state_block_impl (nano::state_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == nano::process_result::progress)
	{
		result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Unambiguous)
		if (result.code == nano::process_result::progress)
		{
			debug_assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.code = block_a.hashables.account.is_zero () ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == nano::process_result::progress)
			{
				nano::epoch epoch (nano::epoch::epoch_0);
				nano::epoch source_epoch (nano::epoch::epoch_0);
				nano::account_info info;
				nano::amount amount (block_a.hashables.balance);
				auto is_send (false);
				auto is_receive (false);
				auto account_error (ledger.store.account.get (transaction, block_a.hashables.account, info));
				if (!account_error)
				{
					// Account already exists
					epoch = info.epoch ();
					result.code = block_a.hashables.previous.is_zero () ? nano::process_result::fork : nano::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == nano::process_result::progress)
					{
						result.code = ledger.store.block.exists (transaction, block_a.hashables.previous) ? nano::process_result::progress : nano::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
						if (result.code == nano::process_result::progress)
						{
							is_send = block_a.hashables.balance < info.balance;
							is_receive = !is_send && !block_a.hashables.link.is_zero ();
							amount = is_send ? (info.balance.number () - amount.number ()) : (amount.number () - info.balance.number ());
							result.code = block_a.hashables.previous == info.head ? nano::process_result::progress : nano::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						}
					}
				}
				else
				{
					// Account does not yet exists
					result.code = block_a.previous ().is_zero () ? nano::process_result::progress : nano::process_result::gap_previous; // Does the first block in an account yield 0 for previous() ? (Unambigious)
					if (result.code == nano::process_result::progress)
					{
						is_receive = true;
						result.code = !block_a.hashables.link.is_zero () ? nano::process_result::progress : nano::process_result::gap_source; // Is the first block receiving from a send ? (Unambigious)
					}
				}
				if (result.code == nano::process_result::progress)
				{
					if (!is_send)
					{
						if (!block_a.hashables.link.is_zero ())
						{
							result.code = ledger.block_or_pruned_exists (transaction, block_a.hashables.link.as_block_hash ()) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block already? (Harmless)
							if (result.code == nano::process_result::progress)
							{
								nano::pending_key key (block_a.hashables.account, block_a.hashables.link.as_block_hash ());
								nano::pending_info pending;
								result.code = ledger.store.pending.get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == nano::process_result::progress)
								{
									result.code = amount == pending.amount ? nano::process_result::progress : nano::process_result::balance_mismatch;
									source_epoch = pending.epoch;
									epoch = std::max (epoch, source_epoch);
								}
							}
						}
						else
						{
							// If there's no link, the balance must remain the same, only the representative can change
							result.code = amount.is_zero () ? nano::process_result::progress : nano::process_result::balance_mismatch;
						}
					}
				}
				if (result.code == nano::process_result::progress && ledger.constants.is_reserve (block_a.hashables.account))
				{
					// Reserve Kei carries no weight of any kind and moves only
					// through a passed on-chain vote (SPEC 5.7,
					// decisions-m2.md 6). Excluding the reserve from governance
					// is not enough on its own: representative weight governs
					// transaction consensus, so a reserve delegation alone
					// hands over an absolute supermajority with no vote.
					if (!block_a.hashables.representative.is_zero ())
					{
						result.code = nano::process_result::reserve_representative;
					}
					else if (is_send)
					{
						result.code = nano::process_result::reserve_locked;
					}
				}
				if (result.code == nano::process_result::progress)
				{
					nano::block_details block_details (epoch, is_send, is_receive, false);
					result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
					if (result.code == nano::process_result::progress)
					{
						ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::state_block);
						block_a.sideband_set (nano::block_sideband (block_a.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, nano::seconds_since_epoch (), block_details, source_epoch));
						ledger.store.block.put (transaction, hash, block_a);

						if (!ledger.constants.is_reserve (block_a.hashables.account) && !info.head.is_zero ())
						{
							// Move existing representation & add in amount delta
							ledger.cache.rep_weights.representation_add_dual (info.representative, 0 - info.balance.number (), block_a.representative (), block_a.hashables.balance.number ());
						}
						else if (!ledger.constants.is_reserve (block_a.hashables.account))
						{
							// Add in amount delta only. A reserve receive is valid, but
							// its balance still contributes zero representative weight.
							ledger.cache.rep_weights.representation_add (block_a.representative (), block_a.hashables.balance.number ());
						}

						if (is_send)
						{
							nano::pending_key key (block_a.hashables.link.as_account (), hash);
							nano::pending_info info (block_a.hashables.account, amount.number (), epoch);
							ledger.store.pending.put (transaction, key, info);
						}
						else if (!block_a.hashables.link.is_zero ())
						{
							ledger.store.pending.del (transaction, nano::pending_key (block_a.hashables.account, block_a.hashables.link.as_block_hash ()));
						}

						nano::account_info new_info (hash, block_a.representative (), info.open_block.is_zero () ? hash : info.open_block, block_a.hashables.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);
						ledger.update_account (transaction, block_a.hashables.account, info, new_info);
						if (!ledger.store.frontier.get (transaction, info.head).is_zero ())
						{
							ledger.store.frontier.del (transaction, info.head);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::epoch_block_impl (nano::state_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == nano::process_result::progress)
	{
		result.code = validate_message (ledger.epoch_signer (block_a.hashables.link), hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Unambiguous)
		if (result.code == nano::process_result::progress)
		{
			debug_assert (!validate_message (ledger.epoch_signer (block_a.hashables.link), hash, block_a.signature));
			result.code = block_a.hashables.account.is_zero () ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == nano::process_result::progress)
			{
				nano::account_info info;
				auto account_error (ledger.store.account.get (transaction, block_a.hashables.account, info));
				if (!account_error)
				{
					// Account already exists
					result.code = block_a.hashables.previous.is_zero () ? nano::process_result::fork : nano::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.previous == info.head ? nano::process_result::progress : nano::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						if (result.code == nano::process_result::progress)
						{
							result.code = block_a.hashables.representative == info.representative ? nano::process_result::progress : nano::process_result::representative_mismatch;
						}
					}
				}
				else
				{
					result.code = block_a.hashables.representative.is_zero () ? nano::process_result::progress : nano::process_result::representative_mismatch;
					// Non-exisitng account should have pending entries
					if (result.code == nano::process_result::progress)
					{
						bool pending_exists = ledger.store.pending.any (transaction, block_a.hashables.account);
						result.code = pending_exists ? nano::process_result::progress : nano::process_result::gap_epoch_open_pending;
					}
				}
				if (result.code == nano::process_result::progress)
				{
					auto epoch = ledger.constants.epochs.epoch (block_a.hashables.link);
					// Must be an epoch for an unopened account or the epoch upgrade must be sequential
					auto is_valid_epoch_upgrade = account_error ? static_cast<std::underlying_type_t<nano::epoch>> (epoch) > 0 : nano::epochs::is_sequential (info.epoch (), epoch);
					result.code = is_valid_epoch_upgrade ? nano::process_result::progress : nano::process_result::block_position;
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.balance == info.balance ? nano::process_result::progress : nano::process_result::balance_mismatch;
						if (result.code == nano::process_result::progress)
						{
							nano::block_details block_details (epoch, false, false, true);
							result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
							if (result.code == nano::process_result::progress)
							{
								ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::epoch_block);
								block_a.sideband_set (nano::block_sideband (block_a.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
								ledger.store.block.put (transaction, hash, block_a);
								nano::account_info new_info (hash, block_a.representative (), info.open_block.is_zero () ? hash : info.open_block, info.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);
								ledger.update_account (transaction, block_a.hashables.account, info, new_info);
								if (!ledger.store.frontier.get (transaction, info.head).is_zero ())
								{
									ledger.store.frontier.del (transaction, info.head);
								}
							}
						}
					}
				}
			}
		}
	}
}

/**
 * Validate and apply one asset block (decisions-m2.md §7, SPEC §5.6).
 *
 * The order below is the mock ledger's, deliberately: `MockLedger` in
 * `@keicoin/core` is the reference implementation for these rules and has a
 * test suite pinning them, so where this could differ it does not.
 *
 * Nothing is written until every check has passed.
 */
void ledger_processor::asset_block (nano::asset_block & block_a)
{
	auto const hash (block_a.hash ());
	result.code = ledger.block_or_pruned_exists (transaction, hash) ? nano::process_result::old : nano::process_result::progress;
	if (result.code != nano::process_result::progress)
	{
		return;
	}
	result.code = block_a.hashables.account.is_zero () ? nano::process_result::opened_burn_account : nano::process_result::progress;
	if (result.code != nano::process_result::progress)
	{
		return;
	}
	result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress;
	if (result.code != nano::process_result::progress)
	{
		return;
	}

	nano::account_info info;
	auto const account_error (ledger.store.account.get (transaction, block_a.hashables.account, info));
	if (!account_error)
	{
		result.code = block_a.hashables.previous.is_zero () ? nano::process_result::fork : nano::process_result::progress;
		if (result.code == nano::process_result::progress)
		{
			result.code = ledger.store.block.exists (transaction, block_a.hashables.previous) ? nano::process_result::progress : nano::process_result::gap_previous;
		}
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.hashables.previous == info.head ? nano::process_result::progress : nano::process_result::fork;
		}
	}
	else
	{
		// An asset block can open an account: a player who has never held Kei
		// can still be minted a token, and `asset_receive` is how they collect
		// it (decisions-m2.md §10). Their Kei balance stays zero, which the
		// balance rule below already requires.
		result.code = block_a.hashables.previous.is_zero () ? nano::process_result::progress : nano::process_result::gap_previous;
	}
	if (result.code != nano::process_result::progress)
	{
		return;
	}

	// Reserve Kei carries no weight of any kind (SPEC §5.7, decisions-m2.md §6).
	// Excluding the reserve from governance is not enough on its own —
	// representative weight governs transaction consensus, so a reserve
	// delegation alone hands over an absolute supermajority with no vote.
	if (ledger.constants.is_reserve (block_a.hashables.account))
	{
		if (!block_a.hashables.representative.is_zero ())
		{
			result.code = nano::process_result::reserve_representative;
			return;
		}
		switch (block_a.hashables.op)
		{
			case nano::asset_op::issue:
			// Issuance destroys Kei (§12). From a reserve account that is
			// a supply change with no vote behind it, which SPEC §5.7 does not
			// permit — so the reserve cannot issue.
			//
			// This is a deliberate divergence from MockLedger, which checks
			// reserve-locked only on a `send` and would let a reserve account
			// burn its way through the reserve one issuance at a time. It costs
			// nothing today because the reserve set is empty until the genesis
			// ceremony (§16), and the mock should adopt it.
			case nano::asset_op::swap_offer:
			case nano::asset_op::swap_accept:
			case nano::asset_op::swap_cancel:
				// And the reserve does not trade. An offer of reserve Kei is a
				// send with an extra step, and an accept pays for something —
				// both move reserve Kei without the vote SPEC §5.7 requires.
				// `swap_cancel` only ever undoes an offer that could not have
				// been made, so refusing all three keeps the rule one line
				// instead of a case analysis that has to stay true.
				result.code = nano::process_result::reserve_locked;
				return;
			default:
				break;
		}
	}

	result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold_asset (block_a.hashables.op) ? nano::process_result::progress : nano::process_result::insufficient_work;
	if (result.code != nano::process_result::progress)
	{
		return;
	}

	// These fields are part of the signed fixed header, but the operation's
	// canonical JSON does not carry them. Reject any second representation
	// before consulting the state which supplies the operation's real values;
	// otherwise a stored block cannot round-trip through RPC with the same hash.
	switch (block_a.hashables.op)
	{
		case nano::asset_op::burn:
			if (!block_a.hashables.link.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			break;
		case nano::asset_op::asset_receive:
		case nano::asset_op::swap_cancel:
			if (!block_a.hashables.asset_id.is_zero () || !block_a.hashables.amount.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			break;
		default:
			break;
	}

	auto const previous_balance (info.balance.number ());
	auto const new_balance (block_a.hashables.balance.number ());
	// How many this account has issued already, which is what prices the next
	// one (§12). Read here because the burn check needs it and the apply below
	// writes it back incremented.
	uint64_t issued_already (0);
	if (block_a.hashables.op == nano::asset_op::issue)
	{
		issued_already = ledger.store.asset.issued_count (transaction, block_a.hashables.account);
		auto const burn (nano::issuance_burn (issued_already));
		// The burn is expressed as the balance decrease itself, with no
		// corresponding receivable — the Kei is destroyed, not moved (§12).
		if (previous_balance < burn || new_balance != previous_balance - burn)
		{
			result.code = nano::process_result::issuance_burn_mismatch;
			return;
		}
	}
	else if (!swap_leg (block_a.hashables.op) && new_balance != previous_balance)
	{
		// §5.6.1's concession to §5.6.8: a Banano-derived explorer that ignores
		// the asset payload still tracks Kei correctly instead of reporting a
		// broken balance.
		//
		// The swap legs are excluded because their answer depends on which
		// asset the offer locked, and that is only known once the lock has been
		// read. Each of them checks the balance itself, and none of them
		// reaches an apply without having done so (decisions-m5.md §3).
		result.code = nano::process_result::asset_balance_mismatch;
		return;
	}

	// Everything below stages its writes and applies them only once the whole
	// block is known to be valid.
	nano::asset_info asset;
	nano::uint256_union asset_id (block_a.hashables.asset_id);
	auto const amount (block_a.hashables.amount.number ());
	// The rooted ops put the root where every other op puts a destination or a
	// source, which is what lets them reuse the fixed header unchanged
	// (decisions-m4.md §2).
	nano::block_hash const root (block_a.hashables.link.as_block_hash ());
	bool asset_dirty (false);
	std::vector<staged_arrival> arrivals;
	boost::optional<nano::amount> credit;
	boost::optional<nano::amount> debit;
	bool collect_receivable (false);
	boost::optional<nano::asset_commit_info> commit;
	bool claimed (false);
	// The swap staging. `swap_lock_key` is the `swap_offer` block's hash for all
	// three legs — the offer's own hash when it creates the lock, and the hash
	// it references when a later block consumes one.
	boost::optional<nano::asset_lock_info> swap_lock;
	nano::block_hash swap_lock_key{ 0 };
	bool swap_lock_delete (false);
	bool swap_offer_list (false);
	bool swap_offer_unlist (false);
	nano::uint256_union swap_offer_asset{ 0 };

	// Told apart only on the failure path, and only because SPEC §9.2 asks for
	// a lost accept/cancel race to read as retryable rather than as a fault. A
	// hash that never named an offer is a different mistake from one whose lock
	// somebody else consumed first.
	auto const lock_unavailable = [this] (nano::block_hash const & offer_a) {
		auto const offer_block (ledger.store.block.get (transaction, offer_a));
		auto const offer_asset (dynamic_cast<nano::asset_block const *> (offer_block.get ()));
		auto const was_an_offer (offer_asset != nullptr && offer_asset->hashables.op == nano::asset_op::swap_offer);
		return was_an_offer ? nano::process_result::offer_consumed : nano::process_result::no_such_offer;
	};

	switch (block_a.hashables.op)
	{
		case nano::asset_op::issue:
		{
			// Identity is derived, never assigned (SPEC §5.6.1), so a block
			// that names an id other than H(issuer ‖ symbol) is lying about
			// which asset it is creating.
			if (asset_id != nano::derive_asset_id (block_a.hashables.account, block_a.hashables.payload.symbol) || amount != 0 || !block_a.hashables.link.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			std::string symbol;
			if (nano::normalize_symbol (block_a.hashables.payload.symbol, symbol) || symbol != block_a.hashables.payload.symbol)
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			if (block_a.hashables.payload.name.empty () || block_a.hashables.payload.name.size () > nano::asset_payload::max_name || block_a.hashables.payload.decimals > 18)
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			if (ledger.store.asset.exists (transaction, asset_id))
			{
				result.code = nano::process_result::asset_exists;
				return;
			}
			asset.issuer = block_a.hashables.account;
			asset.name = block_a.hashables.payload.name;
			asset.symbol = symbol;
			asset.decimals = block_a.hashables.payload.decimals;
			asset.max_supply = block_a.hashables.payload.max_supply;
			asset.transfer = block_a.hashables.payload.transfer;
			asset.swap = block_a.hashables.payload.swap;
			asset.description = block_a.hashables.payload.description;
			asset.image = block_a.hashables.payload.image;
			asset.kind = block_a.hashables.payload.kind;
			asset.circulating = 0;
			asset_dirty = true;
			break;
		}
		case nano::asset_op::mint:
		{
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			if (asset.issuer != block_a.hashables.account)
			{
				result.code = nano::process_result::not_issuer;
				return;
			}
			if (amount == 0 || block_a.hashables.link.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			// maxSupply caps circulating supply, so burning frees headroom
			// (SPEC §5.6.6). Both comparisons subtract rather than add:
			// `circulating + amount` is uint128 arithmetic and wraps, so an
			// amount large enough to carry past 2^128 would compare small and
			// then be credited as the wrapped remainder. An uncapped asset has
			// no cap to compare against but still has the arithmetic ceiling,
			// which is why the first test runs either way.
			auto const circulating (asset.circulating.number ());
			auto const arithmetic_room (std::numeric_limits<nano::uint128_t>::max () - circulating);
			if (amount > arithmetic_room || (!asset.uncapped () && amount > asset.max_supply.number () - circulating))
			{
				result.code = nano::process_result::over_max_supply;
				return;
			}
			asset.circulating = circulating + amount;
			asset_dirty = true;
			arrivals.push_back ({ block_a.hashables.link.as_account (), asset_id, block_a.hashables.amount, block_a.hashables.account, block_a.hashables.payload.memo });
			break;
		}
		case nano::asset_op::burn:
		{
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			if (amount == 0)
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
			if (held < amount)
			{
				result.code = nano::process_result::insufficient_asset_balance;
				return;
			}
			debit = nano::amount (held - amount);
			asset.circulating = asset.circulating.number () - amount;
			asset_dirty = true;
			break;
		}
		case nano::asset_op::transfer:
		{
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			if (amount == 0 || block_a.hashables.link.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			auto const to (block_a.hashables.link.as_account ());
			// The transfer policy is immutable and protocol-enforced; the SDK
			// does not get to ask for an exception (SPEC §5.4).
			auto const permitted (swap_leg_permitted (asset, block_a.hashables.account, to));
			if (permitted != nano::process_result::progress)
			{
				result.code = permitted;
				return;
			}
			auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
			if (held < amount)
			{
				result.code = nano::process_result::insufficient_asset_balance;
				return;
			}
			debit = nano::amount (held - amount);
			arrivals.push_back ({ to, asset_id, block_a.hashables.amount, block_a.hashables.account, block_a.hashables.payload.memo });
			break;
		}
		case nano::asset_op::asset_receive:
		{
			nano::pending_key const key (block_a.hashables.account, block_a.hashables.link.as_block_hash ());
			nano::asset_pending_info pending;
			if (ledger.store.asset.pending_get (transaction, key, pending))
			{
				// Either it was never receivable, or it has already been
				// collected. Both are unreceivable.
				result.code = nano::process_result::unreceivable;
				return;
			}
			asset_id = pending.asset_id;
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
			// Unbounded per-account state in consensus code is how nodes run
			// out of memory (SPEC §7). Only the account itself can add to its
			// own holdings, so this cannot be weaponised against anyone else.
			if (held == 0 && ledger.store.asset.holdings_count (transaction, block_a.hashables.account) >= nano::max_assets_per_account)
			{
				result.code = nano::process_result::too_many_assets;
				return;
			}
			credit = nano::amount (held + pending.amount.number ());
			collect_receivable = true;
			break;
		}
		case nano::asset_op::commit:
		{
			// A drop mints units of one asset, so only that asset's issuer can
			// underwrite one. This is also what stops anyone from racing an
			// issuer to publish their root: an attacker cannot commit against
			// an asset they did not issue, whatever they know about its leaves.
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			if (asset.issuer != block_a.hashables.account)
			{
				result.code = nano::process_result::not_issuer;
				return;
			}
			// `total` is the issuer's own declaration of what the drop covers.
			// The node cannot check it — it never sees the other leaves — but a
			// drop declaring nothing is a mistake worth failing loudly.
			if (root.is_zero () || amount == 0)
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			if (ledger.store.asset.commit_exists (transaction, root))
			{
				// Roots are unique across the ledger, not per issuer. Two issuers
				// colliding here would need the same leaf set, and the leaf binds
				// the asset id, so in practice this catches an issuer republishing
				// a batch it already published — which would otherwise reopen a
				// root it had closed.
				result.code = nano::process_result::commit_exists;
				return;
			}
			commit = nano::asset_commit_info (block_a.hashables.account, asset_id, block_a.hashables.payload.count, block_a.hashables.amount, hash);
			break;
		}
		case nano::asset_op::commit_close:
		{
			nano::asset_commit_info existing;
			if (ledger.store.asset.commit_get (transaction, root, existing))
			{
				result.code = nano::process_result::no_such_commit;
				return;
			}
			if (existing.issuer != block_a.hashables.account)
			{
				result.code = nano::process_result::not_issuer;
				return;
			}
			if (existing.closed)
			{
				// Closing twice would be a block that changes nothing, and a
				// no-op block is a rollback that cannot tell what to undo.
				result.code = nano::process_result::commit_closed;
				return;
			}
			if (!asset_id.is_zero () || amount != 0)
			{
				// The root is the whole statement. Naming an asset or an amount
				// here would be a second, unenforced claim about the drop.
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			existing.closed = true;
			commit = existing;
			break;
		}
		case nano::asset_op::claim:
		{
			nano::asset_commit_info existing;
			if (ledger.store.asset.commit_get (transaction, root, existing))
			{
				result.code = nano::process_result::no_such_commit;
				return;
			}
			if (existing.closed)
			{
				result.code = nano::process_result::commit_closed;
				return;
			}
			if (amount == 0)
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			if (existing.asset_id != asset_id)
			{
				result.code = nano::process_result::bad_claim_proof;
				return;
			}
			// Checked before the proof is folded: an account that has already
			// claimed gets the honest answer rather than a proof error, and the
			// cheap lookup runs before the expensive hashing.
			if (ledger.store.asset.claim_exists (transaction, block_a.hashables.account, root))
			{
				result.code = nano::process_result::already_claimed;
				return;
			}
			auto const leaf (nano::asset_claim_leaf (block_a.hashables.account, asset_id, block_a.hashables.amount));
			if (nano::asset_claim_root (leaf, block_a.hashables.payload.proof) != root)
			{
				result.code = nano::process_result::bad_claim_proof;
				return;
			}
			if (ledger.store.asset.get (transaction, asset_id, asset))
			{
				result.code = nano::process_result::no_such_asset;
				return;
			}
			// A claim is where committed units actually come into existence, so
			// this is where the cap applies. An issuer who commits to more than
			// the cap allows has published a drop whose later claims fail — the
			// node cannot tell that at commit time, because it never learns what
			// the other leaves say. Subtracting rather than adding, for the same
			// reason as the mint above: the sum is uint128 and would wrap.
			auto const circulating (asset.circulating.number ());
			auto const arithmetic_room (std::numeric_limits<nano::uint128_t>::max () - circulating);
			if (amount > arithmetic_room || (!asset.uncapped () && amount > asset.max_supply.number () - circulating))
			{
				result.code = nano::process_result::over_max_supply;
				return;
			}
			auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
			if (held == 0 && ledger.store.asset.holdings_count (transaction, block_a.hashables.account) >= nano::max_assets_per_account)
			{
				result.code = nano::process_result::too_many_assets;
				return;
			}
			asset.circulating = circulating + amount;
			asset_dirty = true;
			// The claimant writes their own block, so there is no receivable step
			// and nothing to collect later: this is the whole point of §5.5, and
			// the reason a thousand claims cost the issuer nothing.
			credit = nano::amount (held + amount);
			claimed = true;
			break;
		}
		case nano::asset_op::swap_offer:
		{
			// A signed to sell `amount` of `asset_id` for the payload's
			// `want_amount` of `want_asset`, to `link` if named or to anyone if
			// not (SPEC §9.2, §9.3). `want_amount == 0` is already refused by
			// `asset_payload::deserialize`, but a block built straight from JSON
			// never goes through that path, so it is checked again here.
			if (amount == 0 || block_a.hashables.payload.want_amount.is_zero ())
			{
				result.code = nano::process_result::bad_asset_payload;
				return;
			}
			auto const counterparty (block_a.hashables.link.as_account ());
			// An offer naming its own author could never be accepted — the
			// self-accept check below refuses exactly that account — so it is
			// refused here instead of left to lock an asset forever for nothing.
			if (!counterparty.is_zero () && counterparty == block_a.hashables.account)
			{
				result.code = nano::process_result::self_swap;
				return;
			}
			if (asset_id.is_zero ())
			{
				// Locking Kei itself: the fixed header's own balance field is the
				// lock, exactly as a send's would be.
				if (previous_balance < amount || new_balance != previous_balance - amount)
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
			}
			else
			{
				if (ledger.store.asset.get (transaction, asset_id, asset))
				{
					result.code = nano::process_result::no_such_asset;
					return;
				}
				auto const permitted (swap_leg_permitted (asset, block_a.hashables.account, counterparty));
				if (permitted != nano::process_result::progress)
				{
					result.code = permitted;
					return;
				}
				auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
				if (held < amount)
				{
					result.code = nano::process_result::insufficient_asset_balance;
					return;
				}
				// Locking an asset moves none of the account's Kei — the fixed
				// header's balance field must restate the previous one exactly,
				// the same invariant `swap_accept`/`swap_cancel` enforce on their
				// own asset branches. Without this, the block never runs the
				// Kei-branch check above either, so nothing here would stop the
				// header from claiming any balance the offerer wants.
				if (new_balance != previous_balance)
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
				debit = nano::amount (held - amount);
			}
			// Keyed by this block's own hash, not `root` — `link` here holds an
			// optional counterparty account, not a reference to an earlier block.
			swap_lock_key = hash;
			swap_lock = lock_from_offer (block_a);
			swap_offer_list = true;
			swap_offer_asset = asset_id;
			break;
		}
		case nano::asset_op::swap_accept:
		{
			swap_lock_key = root;
			nano::asset_lock_info lock;
			if (ledger.store.asset.lock_get (transaction, swap_lock_key, lock) || !lock.open ())
			{
				result.code = lock_unavailable (swap_lock_key);
				return;
			}
			// Self-accept would settle the lock for no reason and leave two
			// permanent receivable records nobody but the offerer ever reads —
			// the ledger-growth cost §5.5 asks every op to justify, spent on
			// nothing (SPEC §9.2 speaks throughout of "the accepter (B)" as a
			// second party).
			if (lock.offerer == block_a.hashables.account || (!lock.counterparty.is_zero () && lock.counterparty != block_a.hashables.account))
			{
				result.code = nano::process_result::swap_not_counterparty;
				return;
			}
			// B restates what it is paying. Without this, B would sign a block
			// whose cost is written on somebody else's chain, and a lock the
			// offerer could not have changed (the lock is derived once, from the
			// offer, and is immutable) still deserves an explicit restatement
			// rather than a blind trust of whatever this hash currently means.
			if (asset_id != lock.want_asset || amount != lock.want_amount.number ())
			{
				result.code = nano::process_result::swap_terms_mismatch;
				return;
			}
			if (asset_id.is_zero ())
			{
				if (previous_balance < amount || new_balance != previous_balance - amount)
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
			}
			else
			{
				if (ledger.store.asset.get (transaction, asset_id, asset))
				{
					result.code = nano::process_result::no_such_asset;
					return;
				}
				// Only the side B is paying is checked against policy here. The
				// side A locked was already checked at offer time against this
				// same counterparty — named explicitly, or, if the offer was
				// open, permitted for any counterparty precisely because A was
				// the issuer of an `issuer_only` asset (the one open-offer case
				// `swap_leg_permitted` allows) — and transfer policy cannot
				// change after issuance (SPEC §5.4), so re-checking it here
				// would only ever repeat the same answer.
				auto const permitted (swap_leg_permitted (asset, block_a.hashables.account, lock.offerer));
				if (permitted != nano::process_result::progress)
				{
					result.code = permitted;
					return;
				}
				auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, asset_id).number ());
				if (held < amount)
				{
					result.code = nano::process_result::insufficient_asset_balance;
					return;
				}
				if (new_balance != previous_balance)
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
				debit = nano::amount (held - amount);
			}
			// Both legs settle in the one block that consumes the lock (SPEC
			// §9.2): A receives what B paid, B receives what A locked.
			arrivals.push_back ({ lock.offerer, lock.want_asset, lock.want_amount, block_a.hashables.account, std::string () });
			arrivals.push_back ({ block_a.hashables.account, lock.asset_id, lock.amount, lock.offerer, std::string () });
			// The lock record stays — it sits on the *accepter's* chain, and
			// nothing orders that against the offerer's, so unlike a cancel
			// there is no chain to roll back to make it disappear. It is kept
			// and marked settled, the same reason `asset_claim_roots` keeps a
			// claim rather than deleting it (decisions-m4.md §4).
			lock.settled_by = hash;
			swap_lock = lock;
			swap_offer_unlist = true;
			swap_offer_asset = lock.asset_id;
			break;
		}
		case nano::asset_op::swap_cancel:
		{
			swap_lock_key = root;
			nano::asset_lock_info lock;
			if (ledger.store.asset.lock_get (transaction, swap_lock_key, lock) || !lock.open ())
			{
				result.code = lock_unavailable (swap_lock_key);
				return;
			}
			if (lock.offerer != block_a.hashables.account)
			{
				result.code = nano::process_result::not_offerer;
				return;
			}
			if (lock.asset_id.is_zero ())
			{
				if (new_balance != previous_balance + lock.amount.number ())
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
			}
			else
			{
				if (new_balance != previous_balance)
				{
					result.code = nano::process_result::asset_balance_mismatch;
					return;
				}
				auto const held (ledger.store.asset.balance (transaction, block_a.hashables.account, lock.asset_id).number ());
				credit = nano::amount (held + lock.amount.number ());
			}
			// The credit above and the `asset_dirty`-free write below both key
			// off `asset_id`, which for a cancel block is always zero on the
			// wire (there is nothing left for the block itself to say — the
			// offer already said it). What was actually locked comes from the
			// lock record, not the block, so it is substituted in here exactly
			// as `asset_receive` substitutes the source block's asset id.
			asset_id = lock.asset_id;
			swap_lock_delete = true;
			swap_offer_unlist = true;
			swap_offer_asset = lock.asset_id;
			break;
		}
	}

	ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::asset_block);

	// An asset block moves no Kei except at issuance, and is neither a send nor
	// a receive in the inherited sense — the sideband's flags describe the Kei
	// side of a block, and on this side of one nothing happens.
	nano::block_details const block_details (info.epoch (), false, false, false);
	block_a.sideband_set (nano::block_sideband (block_a.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
	ledger.store.block.put (transaction, hash, block_a);

	if (!info.head.is_zero ())
	{
		ledger.cache.rep_weights.representation_add_dual (info.representative, 0 - info.balance.number (), block_a.representative (), new_balance);
	}
	else
	{
		ledger.cache.rep_weights.representation_add (block_a.representative (), new_balance);
	}

	if (asset_dirty)
	{
		ledger.store.asset.put (transaction, asset_id, asset);
	}
	if (block_a.hashables.op == nano::asset_op::issue)
	{
		// Priced the burn above; record that it happened, so this account's
		// next asset costs one Kei more than this one did.
		ledger.store.asset.issued_put (transaction, block_a.hashables.account, issued_already + 1);
	}
	if (debit)
	{
		// Zero entries are deleted, not kept at zero, so a player's state
		// footprint shrinks when they spend (SPEC §7).
		if (debit->is_zero ())
		{
			ledger.store.asset.balance_del (transaction, block_a.hashables.account, asset_id);
		}
		else
		{
			ledger.store.asset.balance_put (transaction, block_a.hashables.account, asset_id, *debit);
		}
	}
	if (credit)
	{
		ledger.store.asset.balance_put (transaction, block_a.hashables.account, asset_id, *credit);
	}
	for (auto const & arrival : arrivals)
	{
		if (arrival.asset_id.is_zero ())
		{
			// The first time an asset-typed block ever moves Kei: a swap leg
			// locking or paying in Kei itself (SPEC §9.2). It arrives exactly
			// as a state-block send would, in the inherited `pending` table,
			// rather than in `asset_pending` — told apart by the zero asset id
			// (decisions-m5.md §3).
			ledger.store.pending.put (transaction, nano::pending_key (arrival.to, hash), nano::pending_info (arrival.source, arrival.amount, info.epoch ()));
		}
		else
		{
			ledger.store.asset.pending_put (transaction, nano::pending_key (arrival.to, hash), nano::asset_pending_info (arrival.source, arrival.asset_id, arrival.amount, arrival.memo));
		}
	}
	if (collect_receivable)
	{
		ledger.store.asset.pending_del (transaction, nano::pending_key (block_a.hashables.account, block_a.hashables.link.as_block_hash ()));
	}
	if (commit)
	{
		// Publishing a root and closing one are the same write: the record is
		// small, and a closed root differs from an open one by one byte.
		ledger.store.asset.commit_put (transaction, root, *commit);
	}
	if (claimed)
	{
		ledger.store.asset.claim_put (transaction, block_a.hashables.account, root, hash);
	}
	if (swap_lock)
	{
		// Created by `swap_offer`, or the same record with `settled_by` filled
		// in by `swap_accept` — either way the whole record is in the block,
		// so there is nothing to journal (SPEC §9.2).
		ledger.store.asset.lock_put (transaction, swap_lock_key, *swap_lock);
	}
	if (swap_lock_delete)
	{
		ledger.store.asset.lock_del (transaction, swap_lock_key);
	}
	if (swap_offer_list)
	{
		ledger.store.asset.offer_put (transaction, swap_offer_asset, swap_lock_key, block_a.hashables.account);
	}
	if (swap_offer_unlist)
	{
		ledger.store.asset.offer_del (transaction, swap_offer_asset, swap_lock_key);
	}

	nano::account_info new_info (hash, block_a.representative (), info.open_block.is_zero () ? hash : info.open_block, block_a.hashables.balance, nano::seconds_since_epoch (), info.block_count + 1, info.epoch ());
	ledger.update_account (transaction, block_a.hashables.account, info, new_info);
	if (!ledger.store.frontier.get (transaction, info.head).is_zero ())
	{
		ledger.store.frontier.del (transaction, info.head);
	}
}

void ledger_processor::change_block (nano::change_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block.get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier.get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::fork : nano::process_result::progress;
				if (result.code == nano::process_result::progress)
				{
					auto info = ledger.account_info (transaction, account);
					debug_assert (info);
					debug_assert (info->head == block_a.hashables.previous);
					result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Malformed)
					if (result.code == nano::process_result::progress)
					{
						nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
						result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
						if (result.code == nano::process_result::progress)
						{
							debug_assert (!validate_message (account, hash, block_a.signature));
							block_a.sideband_set (nano::block_sideband (account, 0, info->balance, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
							ledger.store.block.put (transaction, hash, block_a);
							auto balance (ledger.balance (transaction, block_a.hashables.previous));
							ledger.cache.rep_weights.representation_add_dual (block_a.representative (), balance, info->representative, 0 - balance);
							nano::account_info new_info (hash, block_a.representative (), info->open_block, info->balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
							ledger.update_account (transaction, account, *info, new_info);
							ledger.store.frontier.del (transaction, block_a.hashables.previous);
							ledger.store.frontier.put (transaction, hash, account);
							ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::change);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::send_block (nano::send_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block.get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier.get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::fork : nano::process_result::progress;
				if (result.code == nano::process_result::progress)
				{
					result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Malformed)
					if (result.code == nano::process_result::progress)
					{
						nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
						result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
						if (result.code == nano::process_result::progress)
						{
							debug_assert (!validate_message (account, hash, block_a.signature));
							auto info = ledger.account_info (transaction, account);
							debug_assert (info);
							debug_assert (info->head == block_a.hashables.previous);
							result.code = info->balance.number () >= block_a.hashables.balance.number () ? nano::process_result::progress : nano::process_result::negative_spend; // Is this trying to spend a negative amount (Malicious)
							if (result.code == nano::process_result::progress)
							{
								auto amount (info->balance.number () - block_a.hashables.balance.number ());
								ledger.cache.rep_weights.representation_add (info->representative, 0 - amount);
								block_a.sideband_set (nano::block_sideband (account, 0, block_a.hashables.balance /* unused */, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
								ledger.store.block.put (transaction, hash, block_a);
								nano::account_info new_info (hash, info->representative, info->open_block, block_a.hashables.balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
								ledger.update_account (transaction, account, *info, new_info);
								ledger.store.pending.put (transaction, nano::pending_key (block_a.hashables.destination, hash), { account, amount, nano::epoch::epoch_0 });
								ledger.store.frontier.del (transaction, block_a.hashables.previous);
								ledger.store.frontier.put (transaction, hash, account);
								ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::send);
							}
						}
					}
				}
			}
		}
	}
}

void ledger_processor::receive_block (nano::receive_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block already?  (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block.get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous;
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier.get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::gap_previous : nano::process_result::progress; // Have we seen the previous block? No entries for account at all (Harmless)
				if (result.code == nano::process_result::progress)
				{
					result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is the signature valid (Malformed)
					if (result.code == nano::process_result::progress)
					{
						debug_assert (!validate_message (account, hash, block_a.signature));
						result.code = ledger.block_or_pruned_exists (transaction, block_a.hashables.source) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block already? (Harmless)
						if (result.code == nano::process_result::progress)
						{
							auto info = ledger.account_info (transaction, account);
							debug_assert (info);
							result.code = info->head == block_a.hashables.previous ? nano::process_result::progress : nano::process_result::gap_previous; // Block doesn't immediately follow latest block (Harmless)
							if (result.code == nano::process_result::progress)
							{
								nano::pending_key key (account, block_a.hashables.source);
								nano::pending_info pending;
								result.code = ledger.store.pending.get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == nano::process_result::progress)
								{
									result.code = pending.epoch == nano::epoch::epoch_0 ? nano::process_result::progress : nano::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
									if (result.code == nano::process_result::progress)
									{
										nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
										result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
										if (result.code == nano::process_result::progress)
										{
											auto new_balance (info->balance.number () + pending.amount.number ());
#ifdef NDEBUG
											if (ledger.store.block.exists (transaction, block_a.hashables.source))
											{
												auto info = ledger.account_info (transaction, pending.source);
												debug_assert (info);
											}
#endif
											ledger.store.pending.del (transaction, key);
											block_a.sideband_set (nano::block_sideband (account, 0, new_balance, info->block_count + 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
											ledger.store.block.put (transaction, hash, block_a);
											nano::account_info new_info (hash, info->representative, info->open_block, new_balance, nano::seconds_since_epoch (), info->block_count + 1, nano::epoch::epoch_0);
											ledger.update_account (transaction, account, *info, new_info);
											ledger.cache.rep_weights.representation_add (info->representative, pending.amount.number ());
											ledger.store.frontier.del (transaction, block_a.hashables.previous);
											ledger.store.frontier.put (transaction, hash, account);
											ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::receive);
										}
									}
								}
							}
						}
					}
				}
				else
				{
					result.code = ledger.store.block.exists (transaction, block_a.hashables.previous) ? nano::process_result::fork : nano::process_result::gap_previous; // If we have the block but it's not the latest we have a signed fork (Malicious)
				}
			}
		}
	}
}

void ledger_processor::open_block (nano::open_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.block_or_pruned_exists (transaction, hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block already? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is the signature valid (Malformed)
		if (result.code == nano::process_result::progress)
		{
			debug_assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.code = ledger.block_or_pruned_exists (transaction, block_a.hashables.source) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block? (Harmless)
			if (result.code == nano::process_result::progress)
			{
				nano::account_info info;
				result.code = ledger.store.account.get (transaction, block_a.hashables.account, info) ? nano::process_result::progress : nano::process_result::fork; // Has this account already been opened? (Malicious)
				if (result.code == nano::process_result::progress)
				{
					nano::pending_key key (block_a.hashables.account, block_a.hashables.source);
					nano::pending_info pending;
					result.code = ledger.store.pending.get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.account == ledger.constants.burn_account ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is it burning 0 account? (Malicious)
						if (result.code == nano::process_result::progress)
						{
							result.code = pending.epoch == nano::epoch::epoch_0 ? nano::process_result::progress : nano::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
							if (result.code == nano::process_result::progress)
							{
								nano::block_details block_details (nano::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
								result.code = ledger.constants.work.difficulty (block_a) >= ledger.constants.work.threshold (block_a.work_version (), block_details) ? nano::process_result::progress : nano::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
								if (result.code == nano::process_result::progress)
								{
#ifdef NDEBUG
									if (ledger.store.block.exists (transaction, block_a.hashables.source))
									{
										nano::account_info source_info;
										[[maybe_unused]] auto error (ledger.store.account.get (transaction, pending.source, source_info));
										debug_assert (!error);
									}
#endif
									ledger.store.pending.del (transaction, key);
									block_a.sideband_set (nano::block_sideband (block_a.hashables.account, 0, pending.amount, 1, nano::seconds_since_epoch (), block_details, nano::epoch::epoch_0 /* unused */));
									ledger.store.block.put (transaction, hash, block_a);
									nano::account_info new_info (hash, block_a.representative (), hash, pending.amount.number (), nano::seconds_since_epoch (), 1, nano::epoch::epoch_0);
									ledger.update_account (transaction, block_a.hashables.account, info, new_info);
									ledger.cache.rep_weights.representation_add (block_a.representative (), pending.amount.number ());
									ledger.store.frontier.put (transaction, hash, block_a.hashables.account);
									ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::open);
								}
							}
						}
					}
				}
			}
		}
	}
}

ledger_processor::ledger_processor (nano::ledger & ledger_a, nano::write_transaction const & transaction_a) :
	ledger (ledger_a),
	transaction (transaction_a)
{
}
} // namespace

nano::ledger::ledger (nano::store & store_a, nano::stats & stat_a, nano::ledger_constants & constants, nano::generate_cache const & generate_cache_a) :
	constants{ constants },
	store{ store_a },
	stats{ stat_a },
	check_bootstrap_weights{ true }
{
	if (!store.init_error ())
	{
		initialize (generate_cache_a);
	}
}

void nano::ledger::initialize (nano::generate_cache const & generate_cache_a)
{
	if (generate_cache_a.reps || generate_cache_a.account_count || generate_cache_a.block_count)
	{
		store.account.for_each_par (
		[this] (nano::read_transaction const & /*unused*/, nano::store_iterator<nano::account, nano::account_info> i, nano::store_iterator<nano::account, nano::account_info> n) {
			uint64_t block_count_l{ 0 };
			uint64_t account_count_l{ 0 };
			decltype (this->cache.rep_weights) rep_weights_l;
			for (; i != n; ++i)
			{
				nano::account const & account (i->first);
				nano::account_info const & info (i->second);
				block_count_l += info.block_count;
				++account_count_l;
				// Reserve membership is immutable genesis data. Rebuilding the
				// cache must preserve the same exclusion as first startup, instead
				// of quietly assigning 90% of supply to the null representative.
				// A null representative on any circulating account remains a valid
				// weight bucket and must survive restart.
				if (!this->constants.is_reserve (account))
				{
					rep_weights_l.representation_add (info.representative, info.balance.number ());
				}
			}
			this->cache.block_count += block_count_l;
			this->cache.account_count += account_count_l;
			this->cache.rep_weights.copy_from (rep_weights_l);
		});
	}

	if (generate_cache_a.cemented_count)
	{
		store.confirmation_height.for_each_par (
		[this] (nano::read_transaction const & /*unused*/, nano::store_iterator<nano::account, nano::confirmation_height_info> i, nano::store_iterator<nano::account, nano::confirmation_height_info> n) {
			uint64_t cemented_count_l (0);
			for (; i != n; ++i)
			{
				cemented_count_l += i->second.height;
			}
			this->cache.cemented_count += cemented_count_l;
		});
	}

	auto transaction (store.tx_begin_read ());
	cache.pruned_count = store.pruned.count (transaction);

	// Final votes requirement for confirmation canary block
	nano::confirmation_height_info confirmation_height_info;
	if (!store.confirmation_height.get (transaction, constants.final_votes_canary_account, confirmation_height_info))
	{
		cache.final_votes_confirmation_canary = (confirmation_height_info.height >= constants.final_votes_canary_height);
	}
}

// Balance for account containing hash
nano::uint128_t nano::ledger::balance (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	return hash_a.is_zero () ? 0 : store.block.balance (transaction_a, hash_a);
}

nano::uint128_t nano::ledger::balance_safe (nano::transaction const & transaction_a, nano::block_hash const & hash_a, bool & error_a) const
{
	nano::uint128_t result (0);
	if (pruning && !hash_a.is_zero () && !store.block.exists (transaction_a, hash_a))
	{
		error_a = true;
		result = 0;
	}
	else
	{
		result = balance (transaction_a, hash_a);
	}
	return result;
}

// Balance for an account by account number
nano::uint128_t nano::ledger::account_balance (nano::transaction const & transaction_a, nano::account const & account_a, bool only_confirmed_a)
{
	nano::uint128_t result (0);
	if (only_confirmed_a)
	{
		nano::confirmation_height_info info;
		if (!store.confirmation_height.get (transaction_a, account_a, info))
		{
			result = balance (transaction_a, info.frontier);
		}
	}
	else
	{
		auto info = account_info (transaction_a, account_a);
		if (info)
		{
			result = info->balance.number ();
		}
	}
	return result;
}

nano::uint128_t nano::ledger::account_receivable (nano::transaction const & transaction_a, nano::account const & account_a, bool only_confirmed_a)
{
	nano::uint128_t result (0);
	nano::account end (account_a.number () + 1);
	for (auto i (store.pending.begin (transaction_a, nano::pending_key (account_a, 0))), n (store.pending.begin (transaction_a, nano::pending_key (end, 0))); i != n; ++i)
	{
		nano::pending_info const & info (i->second);
		if (only_confirmed_a)
		{
			if (block_confirmed (transaction_a, i->first.hash))
			{
				result += info.amount.number ();
			}
		}
		else
		{
			result += info.amount.number ();
		}
	}
	return result;
}

std::optional<nano::pending_info> nano::ledger::pending_info (nano::transaction const & transaction, nano::pending_key const & key) const
{
	nano::pending_info result;
	if (!store.pending.get (transaction, key, result))
	{
		return result;
	}
	return std::nullopt;
}

nano::process_return nano::ledger::process (nano::write_transaction const & transaction_a, nano::block & block_a)
{
	debug_assert (!constants.work.validate_entry (block_a) || constants.genesis == nano::dev::genesis);
	ledger_processor processor (*this, transaction_a);
	block_a.visit (processor);
	if (processor.result.code == nano::process_result::progress)
	{
		++cache.block_count;
	}
	return processor.result;
}

nano::block_hash nano::ledger::representative (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	auto result (representative_calculated (transaction_a, hash_a));
	debug_assert (result.is_zero () || store.block.exists (transaction_a, result));
	return result;
}

nano::block_hash nano::ledger::representative_calculated (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	representative_visitor visitor (transaction_a, store);
	visitor.compute (hash_a);
	return visitor.result;
}

bool nano::ledger::block_or_pruned_exists (nano::block_hash const & hash_a) const
{
	return block_or_pruned_exists (store.tx_begin_read (), hash_a);
}

bool nano::ledger::block_or_pruned_exists (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	if (store.pruned.exists (transaction_a, hash_a))
	{
		return true;
	}
	return store.block.exists (transaction_a, hash_a);
}

bool nano::ledger::root_exists (nano::transaction const & transaction_a, nano::root const & root_a)
{
	return store.block.exists (transaction_a, root_a.as_block_hash ()) || store.account.exists (transaction_a, root_a.as_account ());
}

std::string nano::ledger::block_text (char const * hash_a)
{
	return block_text (nano::block_hash (hash_a));
}

std::string nano::ledger::block_text (nano::block_hash const & hash_a)
{
	std::string result;
	auto transaction (store.tx_begin_read ());
	auto block (store.block.get (transaction, hash_a));
	if (block != nullptr)
	{
		block->serialize_json (result);
	}
	return result;
}

bool nano::ledger::is_send (nano::transaction const & transaction_a, nano::block const & block_a) const
{
	if (block_a.type () != nano::block_type::state)
	{
		return block_a.type () == nano::block_type::send;
	}
	nano::block_hash previous = block_a.previous ();
	/*
	 * if block_a does not have a sideband, then is_send()
	 * requires that the previous block exists in the database.
	 * This is because it must retrieve the balance of the previous block.
	 */
	debug_assert (block_a.has_sideband () || previous.is_zero () || store.block.exists (transaction_a, previous));

	bool result (false);
	if (block_a.has_sideband ())
	{
		result = block_a.sideband ().details.is_send;
	}
	else
	{
		if (!previous.is_zero ())
		{
			if (block_a.balance () < balance (transaction_a, previous))
			{
				result = true;
			}
		}
	}
	return result;
}

nano::account const & nano::ledger::block_destination (nano::transaction const & transaction_a, nano::block const & block_a)
{
	nano::send_block const * send_block (dynamic_cast<nano::send_block const *> (&block_a));
	nano::state_block const * state_block (dynamic_cast<nano::state_block const *> (&block_a));
	if (send_block != nullptr)
	{
		return send_block->hashables.destination;
	}
	else if (state_block != nullptr && is_send (transaction_a, *state_block))
	{
		return state_block->hashables.link.as_account ();
	}

	return nano::account::null ();
}

nano::block_hash nano::ledger::block_source (nano::transaction const & transaction_a, nano::block const & block_a)
{
	/*
	 * block_source() requires that the previous block of the block
	 * passed in exist in the database.  This is because it will try
	 * to check account balances to determine if it is a send block.
	 */
	debug_assert (block_a.previous ().is_zero () || store.block.exists (transaction_a, block_a.previous ()));

	// If block_a.source () is nonzero, then we have our source.
	// However, universal blocks will always return zero.
	nano::block_hash result (block_a.source ());
	nano::state_block const * state_block (dynamic_cast<nano::state_block const *> (&block_a));
	if (state_block != nullptr && !is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link.as_block_hash ();
	}
	return result;
}

std::pair<nano::block_hash, nano::block_hash> nano::ledger::hash_root_random (nano::transaction const & transaction_a) const
{
	nano::block_hash hash (0);
	nano::root root (0);
	if (!pruning)
	{
		auto block (store.block.random (transaction_a));
		hash = block->hash ();
		root = block->root ();
	}
	else
	{
		uint64_t count (cache.block_count);
		auto region = nano::random_pool::generate_word64 (0, count - 1);
		// Pruned cache cannot guarantee that pruned blocks are already commited
		if (region < cache.pruned_count)
		{
			hash = store.pruned.random (transaction_a);
		}
		if (hash.is_zero ())
		{
			auto block (store.block.random (transaction_a));
			hash = block->hash ();
			root = block->root ();
		}
	}
	return std::make_pair (hash, root.as_block_hash ());
}

// Vote weight of an account
nano::uint128_t nano::ledger::weight (nano::account const & account_a)
{
	if (check_bootstrap_weights.load ())
	{
		if (cache.block_count < bootstrap_weight_max_blocks)
		{
			auto weight = bootstrap_weights.find (account_a);
			if (weight != bootstrap_weights.end ())
			{
				return weight->second;
			}
		}
		else
		{
			check_bootstrap_weights = false;
		}
	}
	return cache.rep_weights.representation_get (account_a);
}

// Rollback blocks until `block_a' doesn't exist or it tries to penetrate the confirmation height
bool nano::ledger::rollback (nano::write_transaction const & transaction_a, nano::block_hash const & block_a, std::vector<std::shared_ptr<nano::block>> & list_a)
{
	debug_assert (store.block.exists (transaction_a, block_a));
	auto account_l (account (transaction_a, block_a));
	auto block_account_height (store.block.account_height (transaction_a, block_a));
	rollback_visitor rollback (transaction_a, *this, list_a);
	auto error (false);
	while (!error && store.block.exists (transaction_a, block_a))
	{
		nano::confirmation_height_info confirmation_height_info;
		store.confirmation_height.get (transaction_a, account_l, confirmation_height_info);
		if (block_account_height > confirmation_height_info.height)
		{
			auto info = account_info (transaction_a, account_l);
			debug_assert (info);
			auto block (store.block.get (transaction_a, info->head));
			list_a.push_back (block);
			block->visit (rollback);
			error = rollback.error;
			if (!error)
			{
				--cache.block_count;
			}
		}
		else
		{
			error = true;
		}
	}
	return error;
}

bool nano::ledger::rollback (nano::write_transaction const & transaction_a, nano::block_hash const & block_a)
{
	std::vector<std::shared_ptr<nano::block>> rollback_list;
	return rollback (transaction_a, block_a, rollback_list);
}

bool nano::ledger::rollback_swap_conflict (nano::write_transaction const & transaction_a, nano::block_hash const & offer_hash_a, nano::block_hash const & desired_hash_a, std::vector<std::shared_ptr<nano::block>> & list_a)
{
	auto const offer_block (store.block.get (transaction_a, offer_hash_a));
	if (offer_block == nullptr)
	{
		// The offer itself is gone — nothing left to reconcile a consumer of.
		return false;
	}
	auto const offer_asset (dynamic_cast<nano::asset_block const *> (offer_block.get ()));
	if (offer_asset == nullptr)
	{
		return false;
	}
	auto const offerer (offer_asset->hashables.account);
	auto error (false);
	while (!error)
	{
		nano::asset_lock_info lock;
		auto const exists (!store.asset.lock_get (transaction_a, offer_hash_a, lock));
		if (exists && lock.open ())
		{
			// Free. Either nothing has consumed it yet, or the rollback below
			// already undid whichever block did.
			break;
		}
		if (exists)
		{
			// Settled by a swap_accept, which sits on the accepter's chain —
			// a different chain than the offer's, so nothing here orders it
			// against this rollback except walking that chain down directly
			// (decisions-m5.md §7).
			if (lock.settled_by == desired_hash_a)
			{
				break;
			}
			if (lock.settled_by.is_zero ())
			{
				error = true;
				break;
			}
			[[maybe_unused]] bool settler_is_pruned (false);
			auto const settler (account_safe (transaction_a, lock.settled_by, settler_is_pruned));
			if (settler_is_pruned)
			{
				error = true;
				break;
			}
			auto const rollback_settler (latest (transaction_a, settler));
			if (rollback_settler.is_zero ())
			{
				error = true;
				break;
			}
			error = rollback (transaction_a, rollback_settler, list_a);
		}
		else
		{
			// The lock record is gone, which only a swap_cancel does, and a
			// cancel is always on the offerer's own chain (decisions-m5.md §7).
			auto const offerer_head (latest (transaction_a, offerer));
			if (offerer_head.is_zero () || offerer_head == desired_hash_a)
			{
				break;
			}
			error = rollback (transaction_a, offerer_head, list_a);
		}
	}
	return error;
}

std::shared_ptr<nano::block> nano::ledger::swap_consumer (nano::transaction const & transaction_a, nano::block_hash const & offer_hash_a)
{
	nano::asset_lock_info lock;
	if (!store.asset.lock_get (transaction_a, offer_hash_a, lock))
	{
		return lock.open () ? nullptr : store.block.get (transaction_a, lock.settled_by);
	}

	// Cancellation removes the lock record, so locate the cancel on the
	// offerer's chain. It need not immediately follow the offer.
	auto const offer = store.block.get (transaction_a, offer_hash_a);
	if (offer == nullptr)
	{
		return nullptr;
	}
	auto const offer_asset = dynamic_cast<nano::asset_block const *> (offer.get ());
	if (offer_asset == nullptr)
	{
		return nullptr;
	}
	auto hash = latest (transaction_a, offer_asset->hashables.account);
	while (!hash.is_zero () && hash != offer_hash_a)
	{
		auto block = store.block.get (transaction_a, hash);
		if (block == nullptr)
		{
			return nullptr;
		}
		auto const asset = dynamic_cast<nano::asset_block const *> (block.get ());
		if (asset != nullptr && asset->hashables.op == nano::asset_op::swap_cancel && asset->hashables.link.as_block_hash () == offer_hash_a)
		{
			return block;
		}
		hash = block->previous ();
	}
	return nullptr;
}

nano::account nano::ledger::account (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	return store.block.account (transaction_a, hash_a);
}

nano::account nano::ledger::account_safe (nano::transaction const & transaction_a, nano::block_hash const & hash_a, bool & error_a) const
{
	if (!pruning)
	{
		return store.block.account (transaction_a, hash_a);
	}
	else
	{
		auto block (store.block.get (transaction_a, hash_a));
		if (block != nullptr)
		{
			return store.block.account_calculated (*block);
		}
		else
		{
			error_a = true;
			return 0;
		}
	}
}

nano::account nano::ledger::account_safe (const nano::transaction & transaction, const nano::block_hash & hash) const
{
	auto block = store.block.get (transaction, hash);
	if (block)
	{
		return store.block.account_calculated (*block);
	}
	else
	{
		return { 0 };
	}
}

std::optional<nano::account_info> nano::ledger::account_info (nano::transaction const & transaction, nano::account const & account) const
{
	return store.account.get (transaction, account);
}

// Return amount decrease or increase for block
nano::uint128_t nano::ledger::amount (nano::transaction const & transaction_a, nano::account const & account_a)
{
	release_assert (account_a == constants.genesis->account ());
	return nano::dev::constants.genesis_amount;
}

nano::uint128_t nano::ledger::amount (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	auto block (store.block.get (transaction_a, hash_a));
	auto block_balance (balance (transaction_a, hash_a));
	auto previous_balance (balance (transaction_a, block->previous ()));
	return block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
}

nano::uint128_t nano::ledger::amount_safe (nano::transaction const & transaction_a, nano::block_hash const & hash_a, bool & error_a) const
{
	auto block (store.block.get (transaction_a, hash_a));
	debug_assert (block);
	auto block_balance (balance (transaction_a, hash_a));
	auto previous_balance (balance_safe (transaction_a, block->previous (), error_a));
	return error_a ? 0 : block_balance > previous_balance ? block_balance - previous_balance
														  : previous_balance - block_balance;
}

// Return latest block for account
nano::block_hash nano::ledger::latest (nano::transaction const & transaction_a, nano::account const & account_a)
{
	auto info = account_info (transaction_a, account_a);
	return !info ? 0 : info->head;
}

// Return latest root for account, account number if there are no blocks for this account.
nano::root nano::ledger::latest_root (nano::transaction const & transaction_a, nano::account const & account_a)
{
	auto info = account_info (transaction_a, account_a);
	if (!info)
	{
		return account_a;
	}
	else
	{
		return info->head;
	}
}

void nano::ledger::dump_account_chain (nano::account const & account_a, std::ostream & stream)
{
	auto transaction (store.tx_begin_read ());
	auto hash (latest (transaction, account_a));
	while (!hash.is_zero ())
	{
		auto block (store.block.get (transaction, hash));
		debug_assert (block != nullptr);
		stream << hash.to_string () << std::endl;
		hash = block->previous ();
	}
}

bool nano::ledger::could_fit (nano::transaction const & transaction_a, nano::block const & block_a) const
{
	auto dependencies (dependent_blocks (transaction_a, block_a));
	return std::all_of (dependencies.begin (), dependencies.end (), [this, &transaction_a] (nano::block_hash const & hash_a) {
		return hash_a.is_zero () || store.block.exists (transaction_a, hash_a);
	});
}

bool nano::ledger::dependents_confirmed (nano::transaction const & transaction_a, nano::block const & block_a) const
{
	auto dependencies (dependent_blocks (transaction_a, block_a));
	return std::all_of (dependencies.begin (), dependencies.end (), [this, &transaction_a] (nano::block_hash const & hash_a) {
		auto result (hash_a.is_zero ());
		if (!result)
		{
			result = block_confirmed (transaction_a, hash_a);
		}
		return result;
	});
}

bool nano::ledger::is_epoch_link (nano::link const & link_a) const
{
	return constants.epochs.is_epoch_link (link_a);
}

class dependent_block_visitor : public nano::block_visitor
{
public:
	dependent_block_visitor (nano::ledger const & ledger_a, nano::transaction const & transaction_a) :
		ledger (ledger_a),
		transaction (transaction_a),
		result ({ 0, 0 })
	{
	}
	void send_block (nano::send_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void receive_block (nano::receive_block const & block_a) override
	{
		result[0] = block_a.previous ();
		result[1] = block_a.source ();
	}
	void open_block (nano::open_block const & block_a) override
	{
		if (block_a.source () != ledger.constants.genesis->account ())
		{
			result[0] = block_a.source ();
		}
	}
	void change_block (nano::change_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void state_block (nano::state_block const & block_a) override
	{
		result[0] = block_a.hashables.previous;
		result[1] = block_a.hashables.link.as_block_hash ();
		// ledger.is_send will check the sideband first, if block_a has a loaded sideband the check that previous block exists can be skipped
		if (ledger.is_epoch_link (block_a.hashables.link) || ((block_a.has_sideband () || ledger.store.block.exists (transaction, block_a.hashables.previous)) && ledger.is_send (transaction, block_a)))
		{
			result[1].clear ();
		}
	}
	void asset_block (nano::asset_block const & block_a) override
	{
		result[0] = block_a.hashables.previous;
		// link is a counterparty account for issue/mint/burn/transfer, a Merkle
		// root for the M4 rooted ops, and only a dependent block hash for
		// asset_receive (decisions-m2.md §7, §10, decisions-m4.md §2).
		//
		// A claim does depend on its commit block, but it names the root rather
		// than that block's hash, so there is nothing here to key an unchecked
		// entry by. A claim that arrives before the root it proves against is
		// rejected rather than held, and comes back on rebroadcast
		// (decisions-m4.md §5).
		//
		// A swap leg's `link` is the offer it consumes, on a different chain
		// than this block. Waiting for the offer to confirm before scheduling
		// an accept or a cancel for its own election narrows the window in
		// which two nodes could each start an independent, unlinked election
		// for a competing consumer before either one's active_transactions
		// entry exists to be joined by the other (SPEC §9.2 point 4).
		if (block_a.hashables.op == nano::asset_op::asset_receive || block_a.hashables.op == nano::asset_op::swap_accept || block_a.hashables.op == nano::asset_op::swap_cancel)
		{
			result[1] = block_a.hashables.link.as_block_hash ();
		}
	}
	nano::ledger const & ledger;
	nano::transaction const & transaction;
	std::array<nano::block_hash, 2> result;
};

std::array<nano::block_hash, 2> nano::ledger::dependent_blocks (nano::transaction const & transaction_a, nano::block const & block_a) const
{
	dependent_block_visitor visitor (*this, transaction_a);
	block_a.visit (visitor);
	return visitor.result;
}

/** Given the block hash of a send block, find the associated receive block that receives that send.
 *  The send block hash is not checked in any way, it is assumed to be correct.
 * @return Return the receive block on success and null on failure
 */
std::shared_ptr<nano::block> nano::ledger::find_receive_block_by_send_hash (nano::transaction const & transaction, nano::account const & destination, nano::block_hash const & send_block_hash)
{
	std::shared_ptr<nano::block> result;
	debug_assert (send_block_hash != 0);

	// get the cemented frontier
	nano::confirmation_height_info info;
	if (store.confirmation_height.get (transaction, destination, info))
	{
		return nullptr;
	}
	auto possible_receive_block = store.block.get (transaction, info.frontier);

	// walk down the chain until the source field of a receive block matches the send block hash
	while (possible_receive_block != nullptr)
	{
		// if source is non-zero then it is a legacy receive or open block
		nano::block_hash source = possible_receive_block->source ();

		// if source is zero then it could be a state block, which needs a different kind of access
		auto state_block = dynamic_cast<nano::state_block const *> (possible_receive_block.get ());
		if (state_block != nullptr)
		{
			// we read the block from the database, so we expect it to have sideband
			debug_assert (state_block->has_sideband ());
			if (state_block->sideband ().details.is_receive)
			{
				source = state_block->hashables.link.as_block_hash ();
			}
		}

		if (send_block_hash == source)
		{
			// we have a match
			result = possible_receive_block;
			break;
		}

		possible_receive_block = store.block.get (transaction, possible_receive_block->previous ());
	}

	return result;
}

nano::account const & nano::ledger::epoch_signer (nano::link const & link_a) const
{
	return constants.epochs.signer (constants.epochs.epoch (link_a));
}

nano::link const & nano::ledger::epoch_link (nano::epoch epoch_a) const
{
	return constants.epochs.link (epoch_a);
}

void nano::ledger::update_account (nano::write_transaction const & transaction_a, nano::account const & account_a, nano::account_info const & old_a, nano::account_info const & new_a)
{
	if (!new_a.head.is_zero ())
	{
		if (old_a.head.is_zero () && new_a.open_block == new_a.head)
		{
			++cache.account_count;
		}
		if (!old_a.head.is_zero () && old_a.epoch () != new_a.epoch ())
		{
			// store.account.put won't erase existing entries if they're in different tables
			store.account.del (transaction_a, account_a);
		}
		store.account.put (transaction_a, account_a, new_a);
	}
	else
	{
		debug_assert (!store.confirmation_height.exists (transaction_a, account_a));
		store.account.del (transaction_a, account_a);
		debug_assert (cache.account_count > 0);
		--cache.account_count;
	}
}

std::shared_ptr<nano::block> nano::ledger::successor (nano::transaction const & transaction_a, nano::qualified_root const & root_a)
{
	nano::block_hash successor (0);
	auto get_from_previous = false;
	if (root_a.previous ().is_zero ())
	{
		auto info = account_info (transaction_a, root_a.root ().as_account ());
		if (info)
		{
			successor = info->open_block;
		}
		else
		{
			get_from_previous = true;
		}
	}
	else
	{
		get_from_previous = true;
	}

	if (get_from_previous)
	{
		successor = store.block.successor (transaction_a, root_a.previous ());
	}
	std::shared_ptr<nano::block> result;
	if (!successor.is_zero ())
	{
		result = store.block.get (transaction_a, successor);
	}
	debug_assert (successor.is_zero () || result != nullptr);
	return result;
}

std::shared_ptr<nano::block> nano::ledger::forked_block (nano::transaction const & transaction_a, nano::block const & block_a)
{
	debug_assert (!store.block.exists (transaction_a, block_a.hash ()));
	auto root (block_a.root ());
	debug_assert (store.block.exists (transaction_a, root.as_block_hash ()) || store.account.exists (transaction_a, root.as_account ()));
	auto result (store.block.get (transaction_a, store.block.successor (transaction_a, root.as_block_hash ())));
	if (result == nullptr)
	{
		auto info = account_info (transaction_a, root.as_account ());
		debug_assert (info);
		result = store.block.get (transaction_a, info->open_block);
		debug_assert (result != nullptr);
	}
	return result;
}

std::shared_ptr<nano::block> nano::ledger::head_block (nano::transaction const & transaction, nano::account const & account)
{
	auto info = store.account.get (transaction, account);
	if (info)
	{
		return store.block.get (transaction, info->head);
	}
	return nullptr;
}

bool nano::ledger::block_confirmed (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	if (store.pruned.exists (transaction_a, hash_a))
	{
		return true;
	}
	auto block = store.block.get (transaction_a, hash_a);
	if (block)
	{
		nano::confirmation_height_info confirmation_height_info;
		store.confirmation_height.get (transaction_a, block->account ().is_zero () ? block->sideband ().account : block->account (), confirmation_height_info);
		auto confirmed (confirmation_height_info.height >= block->sideband ().height);
		return confirmed;
	}
	return false;
}

uint64_t nano::ledger::pruning_action (nano::write_transaction & transaction_a, nano::block_hash const & hash_a, uint64_t const batch_size_a)
{
	uint64_t pruned_count (0);
	nano::block_hash hash (hash_a);
	while (!hash.is_zero () && hash != constants.genesis->hash ())
	{
		auto block (store.block.get (transaction_a, hash));
		if (block != nullptr)
		{
			store.block.del (transaction_a, hash);
			store.pruned.put (transaction_a, hash);
			hash = block->previous ();
			++pruned_count;
			++cache.pruned_count;
			if (pruned_count % batch_size_a == 0)
			{
				transaction_a.commit ();
				transaction_a.renew ();
			}
		}
		else if (store.pruned.exists (transaction_a, hash))
		{
			hash = 0;
		}
		else
		{
			hash = 0;
			release_assert (false && "Error finding block for pruning");
		}
	}
	return pruned_count;
}

std::multimap<uint64_t, nano::uncemented_info, std::greater<>> nano::ledger::unconfirmed_frontiers () const
{
	nano::locked<std::multimap<uint64_t, nano::uncemented_info, std::greater<>>> result;
	using result_t = decltype (result)::value_type;

	store.account.for_each_par ([this, &result] (nano::read_transaction const & transaction_a, nano::store_iterator<nano::account, nano::account_info> i, nano::store_iterator<nano::account, nano::account_info> n) {
		result_t unconfirmed_frontiers_l;
		for (; i != n; ++i)
		{
			auto const & account (i->first);
			auto const & account_info (i->second);

			nano::confirmation_height_info conf_height_info;
			this->store.confirmation_height.get (transaction_a, account, conf_height_info);

			if (account_info.block_count != conf_height_info.height)
			{
				// Always output as no confirmation height has been set on the account yet
				auto height_delta = account_info.block_count - conf_height_info.height;
				auto const & frontier = account_info.head;
				auto const & cemented_frontier = conf_height_info.frontier;
				unconfirmed_frontiers_l.emplace (std::piecewise_construct, std::forward_as_tuple (height_delta), std::forward_as_tuple (cemented_frontier, frontier, i->first));
			}
		}
		// Merge results
		auto result_locked = result.lock ();
		result_locked->insert (unconfirmed_frontiers_l.begin (), unconfirmed_frontiers_l.end ());
	});
	return result;
}

// A precondition is that the store is an LMDB store
bool nano::ledger::migrate_lmdb_to_rocksdb (boost::filesystem::path const & data_path_a) const
{
	boost::system::error_code error_chmod;
	nano::set_secure_perm_directory (data_path_a, error_chmod);
	auto rockdb_data_path = data_path_a / "rocksdb";
	boost::filesystem::remove_all (rockdb_data_path);

	nano::logger_mt logger;
	auto error (false);

	// Open rocksdb database
	nano::rocksdb_config rocksdb_config;
	rocksdb_config.enable = true;
	auto rocksdb_store = nano::make_store (logger, data_path_a, nano::dev::constants, false, true, rocksdb_config);

	if (!rocksdb_store->init_error ())
	{
		store.block.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::blocks }));

				std::vector<uint8_t> vector;
				{
					nano::vectorstream stream (vector);
					nano::serialize_block (stream, *i->second.block);
					i->second.sideband.serialize (stream, i->second.block->type ());
				}
				rocksdb_store->block.raw_put (rocksdb_transaction, vector, i->first);
			}
		});

		store.pending.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::pending }));
				rocksdb_store->pending.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.confirmation_height.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::confirmation_height }));
				rocksdb_store->confirmation_height.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.account.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::accounts }));
				rocksdb_store->account.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.frontier.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::frontiers }));
				rocksdb_store->frontier.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.pruned.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::pruned }));
				rocksdb_store->pruned.put (rocksdb_transaction, i->first);
			}
		});

		store.final_vote.for_each_par (
		[&rocksdb_store] (nano::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::final_votes }));
				rocksdb_store->final_vote.put (rocksdb_transaction, i->first, i->second);
			}
		});

		auto lmdb_transaction (store.tx_begin_read ());
		auto version = store.version.get (lmdb_transaction);
		auto rocksdb_transaction (rocksdb_store->tx_begin_write ());
		rocksdb_store->version.put (rocksdb_transaction, version);

		for (auto i (store.online_weight.begin (lmdb_transaction)), n (store.online_weight.end ()); i != n; ++i)
		{
			rocksdb_store->online_weight.put (rocksdb_transaction, i->first, i->second);
		}

		for (auto i (store.peer.begin (lmdb_transaction)), n (store.peer.end ()); i != n; ++i)
		{
			rocksdb_store->peer.put (rocksdb_transaction, i->first);
		}

		// Compare counts
		error |= store.peer.count (lmdb_transaction) != rocksdb_store->peer.count (rocksdb_transaction);
		error |= store.pruned.count (lmdb_transaction) != rocksdb_store->pruned.count (rocksdb_transaction);
		error |= store.final_vote.count (lmdb_transaction) != rocksdb_store->final_vote.count (rocksdb_transaction);
		error |= store.online_weight.count (lmdb_transaction) != rocksdb_store->online_weight.count (rocksdb_transaction);
		error |= store.version.get (lmdb_transaction) != rocksdb_store->version.get (rocksdb_transaction);

		// For large tables a random key is used instead and makes sure it exists
		auto random_block (store.block.random (lmdb_transaction));
		error |= rocksdb_store->block.get (rocksdb_transaction, random_block->hash ()) == nullptr;

		auto account = random_block->account ().is_zero () ? random_block->sideband ().account : random_block->account ();
		nano::account_info account_info;
		error |= rocksdb_store->account.get (rocksdb_transaction, account, account_info);

		// If confirmation height exists in the lmdb ledger for this account it should exist in the rocksdb ledger
		nano::confirmation_height_info confirmation_height_info{};
		if (!store.confirmation_height.get (lmdb_transaction, account, confirmation_height_info))
		{
			error |= rocksdb_store->confirmation_height.get (rocksdb_transaction, account, confirmation_height_info);
		}
	}
	else
	{
		error = true;
	}
	return error;
}

bool nano::ledger::bootstrap_weight_reached () const
{
	return cache.block_count >= bootstrap_weight_max_blocks;
}

nano::uncemented_info::uncemented_info (nano::block_hash const & cemented_frontier, nano::block_hash const & frontier, nano::account const & account) :
	cemented_frontier (cemented_frontier), frontier (frontier), account (account)
{
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (ledger & ledger, std::string const & name)
{
	auto count = ledger.bootstrap_weights.size ();
	auto sizeof_element = sizeof (decltype (ledger.bootstrap_weights)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "bootstrap_weights", count, sizeof_element }));
	composite->add_component (collect_container_info (ledger.cache.rep_weights, "rep_weights"));
	return composite;
}
