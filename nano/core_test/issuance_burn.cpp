#include <nano/lib/numbers.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace
{
/** What an account pays to issue its first `count` assets, in raw. */
nano::uint128_t total_for (uint64_t count_a)
{
	nano::uint128_t total (0);
	for (uint64_t issued (0); issued < count_a; ++issued)
	{
		total += nano::issuance_burn (issued);
	}
	return total;
}
}

// SPEC §5.6.5: the nth asset an account issues burns n Kei. The first is the
// one that has to stay cheap, because it is the one a developer meets.
TEST (issuance_burn, first_asset_costs_one_kei)
{
	ASSERT_EQ (nano::BAN_ratio, nano::issuance_burn (0));
	ASSERT_EQ (nano::BAN_ratio * 2, nano::issuance_burn (1));
	ASSERT_EQ (nano::BAN_ratio * 3, nano::issuance_burn (2));
}

// SPEC §5.6.5's own worked example: a game issuing a currency and 500 item
// types. It is cheaper under the escalating burn than it was under a flat
// 1,000, which is the point — the burn should not price out a real game.
TEST (issuance_burn, a_real_game_pays_less_than_it_used_to)
{
	auto const game (total_for (501));
	ASSERT_EQ (nano::uint128_t (125751) * nano::BAN_ratio, game);

	auto const flat (nano::uint128_t (501) * nano::uint128_t (1000) * nano::BAN_ratio);
	ASSERT_LT (game, flat);
}

// The other half: one account cannot build a large asset table, because the
// cost of doing so grows with the square of the table.
TEST (issuance_burn, one_account_cannot_issue_a_million)
{
	// 10^6 assets costs sum(1..10^6) = 500,000,500,000 Kei.
	auto const spam (nano::uint128_t ("500000500000") * nano::BAN_ratio);
	ASSERT_EQ (spam, total_for (1000000));

	// Circulating supply is 10^11 Kei — a tenth of the 10^12 total (SPEC §5.7).
	auto const circulating (nano::uint128_t ("100000000000") * nano::BAN_ratio);
	ASSERT_GT (spam, circulating * 4);
}

// Quadratic in the count, so each doubling of the table quadruples the bill.
TEST (issuance_burn, cost_grows_with_the_square_of_the_table)
{
	auto const hundred (total_for (100));
	auto const two_hundred (total_for (200));
	ASSERT_GT (two_hundred, hundred * 3);
	ASSERT_LT (two_hundred, hundred * 5);
}

// Nothing here may exceed a 128-bit balance, which is what holds every amount
// the ledger compares this against.
TEST (issuance_burn, stays_inside_a_128_bit_balance)
{
	auto const largest (nano::issuance_burn (std::numeric_limits<uint32_t>::max ()));
	ASSERT_GT (largest, nano::uint128_t (0));
	ASSERT_LT (largest, std::numeric_limits<nano::uint128_t>::max ());
}
