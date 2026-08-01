#pragma once

#include <nano/secure/store.hpp>

namespace nano
{
namespace rocksdb
{
	class store;
	class asset_store : public nano::asset_store
	{
	private:
		nano::rocksdb::store & store;

	public:
		explicit asset_store (nano::rocksdb::store & store_a);
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

		void commit_put (nano::write_transaction const &, nano::uint256_union const &, nano::asset_commit_info const &) override;
		bool commit_get (nano::transaction const &, nano::uint256_union const &, nano::asset_commit_info &) override;
		void commit_del (nano::write_transaction const &, nano::uint256_union const &) override;
		bool commit_exists (nano::transaction const &, nano::uint256_union const &) override;
		nano::store_iterator<nano::uint256_union, nano::asset_commit_info> commit_begin (nano::transaction const &, nano::uint256_union const &) const override;
		nano::store_iterator<nano::uint256_union, nano::asset_commit_info> commit_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::uint256_union, nano::asset_commit_info> commit_end () const override;

		void claim_put (nano::write_transaction const &, nano::account const &, nano::uint256_union const &, nano::block_hash const &) override;
		void claim_del (nano::write_transaction const &, nano::account const &, nano::uint256_union const &) override;
		bool claim_exists (nano::transaction const &, nano::account const &, nano::uint256_union const &) override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claims_begin (nano::transaction const &, nano::asset_key const &) const override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claims_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claims_end () const override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claim_roots_begin (nano::transaction const &, nano::asset_key const &) const override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claim_roots_begin (nano::transaction const &) const override;
		nano::store_iterator<nano::asset_key, nano::block_hash> claim_roots_end () const override;
	};
}
}
