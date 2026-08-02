#include <nano/node/active_transactions.hpp>
#include <nano/node/block_publisher.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/secure/ledger.hpp>

nano::block_publisher::block_publisher (nano::active_transactions & active, nano::ledger & ledger) :
	active{ active },
	ledger{ ledger }
{
}

void nano::block_publisher::connect (nano::block_processor & block_processor)
{
	block_processor.processed.add ([this] (auto const & result, auto const & block) {
		switch (result.code)
		{
			case nano::process_result::fork:
				observe (block);
				break;
			case nano::process_result::offer_consumed:
				// The applied consumer may not have reached the scheduler yet.
				// Seed its resource election before publishing the rejected
				// contender, so arrival order cannot make the conflict vanish.
				if (auto const asset = dynamic_cast<nano::asset_block const *> (block.get ()))
				{
					auto transaction = ledger.store.tx_begin_read ();
					auto current = ledger.swap_consumer (transaction, asset->hashables.link.as_block_hash ());
					if (current != nullptr && !ledger.block_confirmed (transaction, current->hash ()))
					{
						active.insert (current);
						observe (block);
					}
				}
				break;
			default:
				break;
		}
	});
}

void nano::block_publisher::observe (std::shared_ptr<nano::block> block)
{
	active.publish (block);
}
