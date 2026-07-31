#pragma once

#include <nano/secure/store.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano
{
namespace lmdb
{
	class store;
	class asset_store : public nano::asset_store
	{
	private:
		nano::lmdb::store & store;

	public:
		explicit asset_store (nano::lmdb::store & store_a);
		void put (nano::write_transaction const &, nano::uint256_union const &, nano::asset_info const &) override;
		bool get (nano::transaction const &, nano::uint256_union const &, nano::asset_info &) override;
		void del (nano::write_transaction const &, nano::uint256_union const &) override;
		bool exists (nano::transaction const &, nano::uint256_union const &) override;
		size_t count (nano::transaction const &) override;
		nano::store_iterator<nano::uint256_union, nano::asset_info> begin (nano::transaction const &) const override;
		nano::store_iterator<nano::uint256_union, nano::asset_info> begin (nano::transaction const &, nano::uint256_union const &) const override;
		nano::store_iterator<nano::uint256_union, nano::asset_info> end () const override;

		void balance_put (nano::write_transaction const &, nano::account const &, nano::uint256_union const &, nano::amount const &) override;
		void balance_del (nano::write_transaction const &, nano::account const &, nano::uint256_union const &) override;
		nano::amount balance (nano::transaction const &, nano::account const &, nano::uint256_union const &) override;
		size_t holdings_count (nano::transaction const &, nano::account const &) override;

		nano::store_iterator<nano::asset_key, nano::amount> holdings_begin (nano::transaction const &, nano::asset_key const &) const override;
		nano::store_iterator<nano::asset_key, nano::amount> holdings_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::asset_key, nano::amount> holdings_end () const override;
		nano::store_iterator<nano::asset_key, nano::amount> holders_begin (nano::transaction const &, nano::asset_key const &) const override;
		nano::store_iterator<nano::asset_key, nano::amount> holders_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::asset_key, nano::amount> holders_end () const override;

		uint64_t issued_count (nano::transaction const &, nano::account const &) override;
		void issued_put (nano::write_transaction const &, nano::account const &, uint64_t) override;

		void pending_put (nano::write_transaction const &, nano::pending_key const &, nano::asset_pending_info const &) override;
		bool pending_get (nano::transaction const &, nano::pending_key const &, nano::asset_pending_info &) override;
		void pending_del (nano::write_transaction const &, nano::pending_key const &) override;
		bool pending_exists (nano::transaction const &, nano::pending_key const &) override;
		nano::store_iterator<nano::pending_key, nano::asset_pending_info> pending_begin (nano::transaction const &, nano::pending_key const &) const override;
		nano::store_iterator<nano::pending_key, nano::asset_pending_info> pending_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::pending_key, nano::asset_pending_info> pending_end () const override;

		/**
		 * Maps an asset id to its record.
		 * nano::uint256_union -> nano::asset_info
		 */
		MDB_dbi assets_handle{ 0 };

		/**
		 * Maps (account, asset id) to a balance. Prefix-scannable by account.
		 * nano::asset_key -> nano::amount
		 */
		MDB_dbi holdings_handle{ 0 };

		/**
		 * Maps (asset id, account) to a balance. Prefix-scannable by asset.
		 * nano::asset_key -> nano::amount
		 */
		MDB_dbi holders_handle{ 0 };

		/**
		 * Maps (destination account, asset block) to an uncollected arrival.
		 * nano::pending_key -> nano::asset_pending_info
		 */
		MDB_dbi asset_pending_handle{ 0 };

		/**
		 * Maps an account to how many assets it has issued, which is what
		 * prices its next one (SPEC 5.6.5).
		 * nano::account -> uint64_t
		 */
		MDB_dbi issued_handle{ 0 };
	};
}
}
