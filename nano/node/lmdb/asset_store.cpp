#include <nano/node/lmdb/asset_store.hpp>
#include <nano/node/lmdb/lmdb.hpp>

nano::lmdb::asset_store::asset_store (nano::lmdb::store & store) :
	store{ store } {};

void nano::lmdb::asset_store::put (nano::write_transaction const & transaction, nano::uint256_union const & asset_id, nano::asset_info const & info)
{
	auto status = store.put (transaction, tables::assets, asset_id, info);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::get (nano::transaction const & transaction, nano::uint256_union const & asset_id, nano::asset_info & info_a)
{
	nano::mdb_val value;
	auto status = store.get (transaction, tables::assets, asset_id, value);
	release_assert (store.success (status) || store.not_found (status));
	bool result (true);
	if (store.success (status))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = info_a.deserialize (stream);
	}
	return result;
}

void nano::lmdb::asset_store::del (nano::write_transaction const & transaction, nano::uint256_union const & asset_id)
{
	auto status = store.del (transaction, tables::assets, asset_id);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::exists (nano::transaction const & transaction, nano::uint256_union const & asset_id)
{
	nano::mdb_val junk;
	auto status = store.get (transaction, tables::assets, asset_id, junk);
	release_assert (store.success (status) || store.not_found (status));
	return store.success (status);
}

size_t nano::lmdb::asset_store::count (nano::transaction const & transaction)
{
	return store.count (transaction, tables::assets);
}

nano::store_iterator<nano::uint256_union, nano::asset_info> nano::lmdb::asset_store::begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::uint256_union, nano::asset_info> (transaction, tables::assets);
}

nano::store_iterator<nano::uint256_union, nano::asset_info> nano::lmdb::asset_store::begin (nano::transaction const & transaction, nano::uint256_union const & asset_id) const
{
	return store.make_iterator<nano::uint256_union, nano::asset_info> (transaction, tables::assets, asset_id);
}

nano::store_iterator<nano::uint256_union, nano::asset_info> nano::lmdb::asset_store::end () const
{
	return nano::store_iterator<nano::uint256_union, nano::asset_info> (nullptr);
}

void nano::lmdb::asset_store::balance_put (nano::write_transaction const & transaction, nano::account const & account, nano::uint256_union const & asset_id, nano::amount const & balance)
{
	// Both indexes always move together. Writing one without the other is the
	// single way these two tables can disagree, so there is no API that can.
	auto status = store.put (transaction, tables::holdings, nano::holding_key (account, asset_id), balance);
	store.release_assert_success (status);
	status = store.put (transaction, tables::holders, nano::holder_key (asset_id, account), balance);
	store.release_assert_success (status);
}

void nano::lmdb::asset_store::balance_del (nano::write_transaction const & transaction, nano::account const & account, nano::uint256_union const & asset_id)
{
	auto status = store.del (transaction, tables::holdings, nano::holding_key (account, asset_id));
	store.release_assert_success (status);
	status = store.del (transaction, tables::holders, nano::holder_key (asset_id, account));
	store.release_assert_success (status);
}

nano::amount nano::lmdb::asset_store::balance (nano::transaction const & transaction, nano::account const & account, nano::uint256_union const & asset_id)
{
	// One lookup in `holders`, which is acceptance criterion §14.3 and the
	// reason the same facts are indexed twice.
	nano::mdb_val value;
	auto status = store.get (transaction, tables::holders, nano::holder_key (asset_id, account), value);
	release_assert (store.success (status) || store.not_found (status));
	if (!store.success (status))
	{
		return nano::amount (0);
	}
	return static_cast<nano::amount> (value);
}

size_t nano::lmdb::asset_store::holdings_count (nano::transaction const & transaction, nano::account const & account)
{
	size_t result (0);
	for (auto i (holdings_begin (transaction, nano::holding_key (account, 0))), n (holdings_end ()); i != n && i->first.first == account; ++i)
	{
		++result;
	}
	return result;
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holdings_begin (nano::transaction const & transaction, nano::asset_key const & key) const
{
	return store.make_iterator<nano::asset_key, nano::amount> (transaction, tables::holdings, key);
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holdings_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::asset_key, nano::amount> (transaction, tables::holdings);
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holdings_end () const
{
	return nano::store_iterator<nano::asset_key, nano::amount> (nullptr);
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holders_begin (nano::transaction const & transaction, nano::asset_key const & key) const
{
	return store.make_iterator<nano::asset_key, nano::amount> (transaction, tables::holders, key);
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holders_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::asset_key, nano::amount> (transaction, tables::holders);
}

nano::store_iterator<nano::asset_key, nano::amount> nano::lmdb::asset_store::holders_end () const
{
	return nano::store_iterator<nano::asset_key, nano::amount> (nullptr);
}

uint64_t nano::lmdb::asset_store::issued_count (nano::transaction const & transaction, nano::account const & account)
{
	nano::mdb_val value;
	auto status = store.get (transaction, tables::issued, account, value);
	release_assert (store.success (status) || store.not_found (status));
	if (!store.success (status))
	{
		// An account that has never issued has no entry, so its next asset is
		// its first and costs 1 Kei.
		return 0;
	}
	return static_cast<uint64_t> (value);
}

void nano::lmdb::asset_store::issued_put (nano::write_transaction const & transaction, nano::account const & account, uint64_t count)
{
	if (count == 0)
	{
		// Zero is absence, the same rule the holdings tables follow (§9): a
		// rolled-back first issuance leaves the account as it was found.
		auto status = store.del (transaction, tables::issued, account);
		release_assert (store.success (status) || store.not_found (status));
		return;
	}
	auto status = store.put (transaction, tables::issued, account, count);
	store.release_assert_success (status);
}

void nano::lmdb::asset_store::pending_put (nano::write_transaction const & transaction, nano::pending_key const & key, nano::asset_pending_info const & info)
{
	auto status = store.put (transaction, tables::asset_pending, key, info);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::pending_get (nano::transaction const & transaction, nano::pending_key const & key, nano::asset_pending_info & info_a)
{
	nano::mdb_val value;
	auto status = store.get (transaction, tables::asset_pending, key, value);
	release_assert (store.success (status) || store.not_found (status));
	bool result (true);
	if (store.success (status))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = info_a.deserialize (stream);
	}
	return result;
}

void nano::lmdb::asset_store::pending_del (nano::write_transaction const & transaction, nano::pending_key const & key)
{
	auto status = store.del (transaction, tables::asset_pending, key);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::pending_exists (nano::transaction const & transaction, nano::pending_key const & key)
{
	auto iterator (pending_begin (transaction, key));
	return iterator != pending_end () && nano::pending_key (iterator->first) == key;
}

nano::store_iterator<nano::pending_key, nano::asset_pending_info> nano::lmdb::asset_store::pending_begin (nano::transaction const & transaction, nano::pending_key const & key) const
{
	return store.make_iterator<nano::pending_key, nano::asset_pending_info> (transaction, tables::asset_pending, key);
}

nano::store_iterator<nano::pending_key, nano::asset_pending_info> nano::lmdb::asset_store::pending_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::pending_key, nano::asset_pending_info> (transaction, tables::asset_pending);
}

nano::store_iterator<nano::pending_key, nano::asset_pending_info> nano::lmdb::asset_store::pending_end () const
{
	return nano::store_iterator<nano::pending_key, nano::asset_pending_info> (nullptr);
}

void nano::lmdb::asset_store::commit_put (nano::write_transaction const & transaction, nano::uint256_union const & root, nano::asset_commit_info const & info)
{
	auto status = store.put (transaction, tables::asset_commits, root, info);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::commit_get (nano::transaction const & transaction, nano::uint256_union const & root, nano::asset_commit_info & info_a)
{
	nano::mdb_val value;
	auto status = store.get (transaction, tables::asset_commits, root, value);
	release_assert (store.success (status) || store.not_found (status));
	bool result (true);
	if (store.success (status))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = info_a.deserialize (stream);
	}
	return result;
}

void nano::lmdb::asset_store::commit_del (nano::write_transaction const & transaction, nano::uint256_union const & root)
{
	auto status = store.del (transaction, tables::asset_commits, root);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::commit_exists (nano::transaction const & transaction, nano::uint256_union const & root)
{
	nano::mdb_val junk;
	auto status = store.get (transaction, tables::asset_commits, root, junk);
	release_assert (store.success (status) || store.not_found (status));
	return store.success (status);
}

nano::store_iterator<nano::uint256_union, nano::asset_commit_info> nano::lmdb::asset_store::commit_begin (nano::transaction const & transaction, nano::uint256_union const & root) const
{
	return store.make_iterator<nano::uint256_union, nano::asset_commit_info> (transaction, tables::asset_commits, root);
}

nano::store_iterator<nano::uint256_union, nano::asset_commit_info> nano::lmdb::asset_store::commit_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::uint256_union, nano::asset_commit_info> (transaction, tables::asset_commits);
}

nano::store_iterator<nano::uint256_union, nano::asset_commit_info> nano::lmdb::asset_store::commit_end () const
{
	return nano::store_iterator<nano::uint256_union, nano::asset_commit_info> (nullptr);
}

void nano::lmdb::asset_store::claim_put (nano::write_transaction const & transaction, nano::account const & account, nano::uint256_union const & root, nano::block_hash const & block)
{
	// Both orderings always move together, exactly as the two holdings tables do.
	auto status = store.put (transaction, tables::asset_claims, nano::claim_key (account, root), block);
	store.release_assert_success (status);
	status = store.put (transaction, tables::asset_claim_roots, nano::claim_root_key (root, account), block);
	store.release_assert_success (status);
}

void nano::lmdb::asset_store::claim_del (nano::write_transaction const & transaction, nano::account const & account, nano::uint256_union const & root)
{
	auto status = store.del (transaction, tables::asset_claims, nano::claim_key (account, root));
	store.release_assert_success (status);
	status = store.del (transaction, tables::asset_claim_roots, nano::claim_root_key (root, account));
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::claim_exists (nano::transaction const & transaction, nano::account const & account, nano::uint256_union const & root)
{
	nano::mdb_val junk;
	auto status = store.get (transaction, tables::asset_claims, nano::claim_key (account, root), junk);
	release_assert (store.success (status) || store.not_found (status));
	return store.success (status);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claims_begin (nano::transaction const & transaction, nano::asset_key const & key) const
{
	return store.make_iterator<nano::asset_key, nano::block_hash> (transaction, tables::asset_claims, key);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claims_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::asset_key, nano::block_hash> (transaction, tables::asset_claims);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claims_end () const
{
	return nano::store_iterator<nano::asset_key, nano::block_hash> (nullptr);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claim_roots_begin (nano::transaction const & transaction, nano::asset_key const & key) const
{
	return store.make_iterator<nano::asset_key, nano::block_hash> (transaction, tables::asset_claim_roots, key);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claim_roots_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::asset_key, nano::block_hash> (transaction, tables::asset_claim_roots);
}

nano::store_iterator<nano::asset_key, nano::block_hash> nano::lmdb::asset_store::claim_roots_end () const
{
	return nano::store_iterator<nano::asset_key, nano::block_hash> (nullptr);
}

void nano::lmdb::asset_store::lock_put (nano::write_transaction const & transaction, nano::block_hash const & offer, nano::asset_lock_info const & info)
{
	auto status = store.put (transaction, tables::swap_locks, offer, info);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::lock_get (nano::transaction const & transaction, nano::block_hash const & offer, nano::asset_lock_info & info_a)
{
	nano::mdb_val value;
	auto status = store.get (transaction, tables::swap_locks, offer, value);
	release_assert (store.success (status) || store.not_found (status));
	bool result (true);
	if (store.success (status))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = info_a.deserialize (stream);
	}
	return result;
}

void nano::lmdb::asset_store::lock_del (nano::write_transaction const & transaction, nano::block_hash const & offer)
{
	auto status = store.del (transaction, tables::swap_locks, offer);
	store.release_assert_success (status);
}

bool nano::lmdb::asset_store::lock_exists (nano::transaction const & transaction, nano::block_hash const & offer)
{
	nano::mdb_val junk;
	auto status = store.get (transaction, tables::swap_locks, offer, junk);
	release_assert (store.success (status) || store.not_found (status));
	return store.success (status);
}

nano::store_iterator<nano::block_hash, nano::asset_lock_info> nano::lmdb::asset_store::locks_begin (nano::transaction const & transaction, nano::block_hash const & offer) const
{
	return store.make_iterator<nano::block_hash, nano::asset_lock_info> (transaction, tables::swap_locks, offer);
}

nano::store_iterator<nano::block_hash, nano::asset_lock_info> nano::lmdb::asset_store::locks_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::block_hash, nano::asset_lock_info> (transaction, tables::swap_locks);
}

nano::store_iterator<nano::block_hash, nano::asset_lock_info> nano::lmdb::asset_store::locks_end () const
{
	return nano::store_iterator<nano::block_hash, nano::asset_lock_info> (nullptr);
}

void nano::lmdb::asset_store::offer_put (nano::write_transaction const & transaction, nano::uint256_union const & asset_id, nano::block_hash const & offer, nano::account const & offerer)
{
	auto status = store.put (transaction, tables::swap_offers, nano::offer_key (asset_id, offer), offerer);
	store.release_assert_success (status);
}

void nano::lmdb::asset_store::offer_del (nano::write_transaction const & transaction, nano::uint256_union const & asset_id, nano::block_hash const & offer)
{
	auto status = store.del (transaction, tables::swap_offers, nano::offer_key (asset_id, offer));
	store.release_assert_success (status);
}

nano::store_iterator<nano::asset_key, nano::account> nano::lmdb::asset_store::offers_begin (nano::transaction const & transaction, nano::asset_key const & key) const
{
	return store.make_iterator<nano::asset_key, nano::account> (transaction, tables::swap_offers, key);
}

nano::store_iterator<nano::asset_key, nano::account> nano::lmdb::asset_store::offers_begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::asset_key, nano::account> (transaction, tables::swap_offers);
}

nano::store_iterator<nano::asset_key, nano::account> nano::lmdb::asset_store::offers_end () const
{
	return nano::store_iterator<nano::asset_key, nano::account> (nullptr);
}
