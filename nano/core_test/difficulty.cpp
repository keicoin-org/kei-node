#include <nano/lib/blocks.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/epoch.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/common.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

TEST (difficultyDeathTest, multipliers)
{
	// For ASSERT_DEATH_IF_SUPPORTED
	testing::FLAGS_gtest_death_test_style = "threadsafe";

	{
		uint64_t base = 0xff00000000000000;
		uint64_t difficulty = 0xfff27e7a57c285cd;
		double expected_multiplier = 18.95461493377003;

		ASSERT_NEAR (expected_multiplier, nano::difficulty::to_multiplier (difficulty, base), 1e-10);
		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (expected_multiplier, base));
	}

	{
		uint64_t base = 0xffffffc000000000;
		uint64_t difficulty = 0xfffffe0000000000;
		double expected_multiplier = 0.125;

		ASSERT_NEAR (expected_multiplier, nano::difficulty::to_multiplier (difficulty, base), 1e-10);
		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (expected_multiplier, base));
	}

	{
		uint64_t base = std::numeric_limits<std::uint64_t>::max ();
		uint64_t difficulty = 0xffffffffffffff00;
		double expected_multiplier = 0.00390625;

		ASSERT_NEAR (expected_multiplier, nano::difficulty::to_multiplier (difficulty, base), 1e-10);
		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (expected_multiplier, base));
	}

	{
		uint64_t base = 0x8000000000000000;
		uint64_t difficulty = 0xf000000000000000;
		double expected_multiplier = 8.0;

		ASSERT_NEAR (expected_multiplier, nano::difficulty::to_multiplier (difficulty, base), 1e-10);
		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (expected_multiplier, base));
	}

	// The death checks don't fail on a release config, so guard against them
#ifndef NDEBUG
	// Causes valgrind to be noisy
	if (!nano::running_within_valgrind ())
	{
		uint64_t base = 0xfffffe0000000000;
		uint64_t difficulty_nil = 0;
		double multiplier_nil = 0.;

		ASSERT_DEATH_IF_SUPPORTED (nano::difficulty::to_multiplier (difficulty_nil, base), "");
		ASSERT_DEATH_IF_SUPPORTED (nano::difficulty::from_multiplier (multiplier_nil, base), "");
	}
#endif
}

TEST (difficulty, overflow)
{
	// Overflow max (attempt to overflow & receive lower difficulty)
	{
		uint64_t base = std::numeric_limits<std::uint64_t>::max (); // Max possible difficulty
		uint64_t difficulty = std::numeric_limits<std::uint64_t>::max ();
		double multiplier = 1.001; // Try to increase difficulty above max

		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (multiplier, base));
	}

	// Overflow min (attempt to overflow & receive higher difficulty)
	{
		uint64_t base = 1; // Min possible difficulty before 0
		uint64_t difficulty = 0;
		double multiplier = 0.999; // Decrease difficulty

		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (multiplier, base));
	}
}

TEST (difficulty, zero)
{
	// Tests with base difficulty 0 should return 0 with any multiplier
	{
		uint64_t base = 0; // Min possible difficulty
		uint64_t difficulty = 0;
		double multiplier = 0.000000001; // Decrease difficulty

		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (multiplier, base));
	}

	{
		uint64_t base = 0; // Min possible difficulty
		uint64_t difficulty = 0;
		double multiplier = 1000000000.0; // Increase difficulty

		ASSERT_EQ (difficulty, nano::difficulty::from_multiplier (multiplier, base));
	}
}

TEST (difficulty, network_constants)
{
	auto & full_thresholds = nano::work_thresholds::publish_full;
	auto & beta_thresholds = nano::work_thresholds::publish_beta;
	auto & dev_thresholds = nano::work_thresholds::publish_dev;

	ASSERT_NEAR (1e-10, nano::difficulty::to_multiplier (full_thresholds.epoch_2, full_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1 / 8., nano::difficulty::to_multiplier (full_thresholds.epoch_2_receive, full_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (full_thresholds.epoch_2_receive, full_thresholds.entry), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (full_thresholds.epoch_2, full_thresholds.base), 1e-10);

	ASSERT_NEAR (1 / 64., nano::difficulty::to_multiplier (beta_thresholds.epoch_1, full_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (beta_thresholds.epoch_2, beta_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1 / 2., nano::difficulty::to_multiplier (beta_thresholds.epoch_2_receive, beta_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (beta_thresholds.epoch_2_receive, beta_thresholds.entry), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (beta_thresholds.epoch_2, beta_thresholds.base), 1e-10);

	ASSERT_NEAR (8., nano::difficulty::to_multiplier (dev_thresholds.epoch_2, dev_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1 / 8., nano::difficulty::to_multiplier (dev_thresholds.epoch_2_receive, dev_thresholds.epoch_1), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (dev_thresholds.epoch_2_receive, dev_thresholds.entry), 1e-10);
	ASSERT_NEAR (1., nano::difficulty::to_multiplier (dev_thresholds.epoch_2, dev_thresholds.base), 1e-10);

	nano::work_version version{ nano::work_version::work_1 };
	ASSERT_EQ (nano::dev::network_params.work.base, nano::dev::network_params.work.epoch_2);
	ASSERT_EQ (nano::dev::network_params.work.base, nano::dev::network_params.work.threshold_base (version));
	ASSERT_EQ (nano::dev::network_params.work.entry, nano::dev::network_params.work.threshold_entry (version, nano::block_type::state));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold_entry (version, nano::block_type::send));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold_entry (version, nano::block_type::receive));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold_entry (version, nano::block_type::open));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold_entry (version, nano::block_type::change));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_0, false, false, false)));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_1, false, false, false)));
	ASSERT_EQ (nano::dev::network_params.work.epoch_1, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_1, false, false, false)));

	// Send [+ change]
	ASSERT_EQ (nano::dev::network_params.work.epoch_2, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_2, true, false, false)));
	// Change
	ASSERT_EQ (nano::dev::network_params.work.epoch_2, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_2, false, false, false)));
	// Receive [+ change] / Open
	ASSERT_EQ (nano::dev::network_params.work.epoch_2_receive, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_2, false, true, false)));
	// Epoch
	ASSERT_EQ (nano::dev::network_params.work.epoch_2_receive, nano::dev::network_params.work.threshold (version, nano::block_details (nano::epoch::epoch_2, false, false, true)));
}

// SPEC §5.6.4's tier table, checked as constants rather than by processing a
// block. That is deliberate: the defect this guards was the *value* of tier C
// on `banano_live_network`, and the live network is the one configuration no
// ledger test here can run against. A test that drives a `claim` through the
// dev ledger would have passed throughout.
TEST (asset_work_tiers, tier_c_is_never_free_on_any_network)
{
	for (auto const * thresholds : { &nano::work_thresholds::publish_full, &nano::work_thresholds::publish_beta, &nano::work_thresholds::publish_dev, &nano::work_thresholds::publish_test })
	{
		auto const & work (*thresholds);
		// The whole point. `difficulty >= 0` is true for `work = 0`, so a zero
		// here is not a low price, it is no price: one asset issuance and one
		// commit bought a million permanent accounts, each with an `account`
		// record, a `holdings` row, a `holders` row and an `asset_claim_roots`
		// row, on every node forever.
		ASSERT_NE (0, work.tier_c ());
		ASSERT_NE (0, work.threshold_asset (nano::asset_op::claim));
		ASSERT_NE (0, work.threshold_asset (nano::asset_op::burn));
		ASSERT_NE (0, work.threshold_asset (nano::asset_op::asset_receive));

		// Cheap is still the point. §5.5's design depends on a thousand players
		// claiming at once without a visible pause, so tier C must stay the
		// cheapest of the three — the failure this pairs with is someone
		// "fixing" the line above by raising tier C to tier B.
		ASSERT_LT (work.tier_c (), work.tier_b ());
		ASSERT_LT (work.tier_b (), work.tier_a);
		// And work generated at the pool's default must satisfy every tier.
		ASSERT_LE (work.tier_a, work.base);
	}

	// Ordinary Kei receives are deliberately *not* repriced. They are `state`
	// blocks and read `epoch_2_receive` through `threshold (version, details)`,
	// which stays zero on the live network so a Banano-shaped zero-work receive
	// is still valid (SPEC §5.6.8). Only `threshold_asset` reads tier C.
	// Asserted rather than left in a comment, because the two being separate
	// fields is exactly what someone tidying up would merge back together.
	ASSERT_EQ (0, nano::work_thresholds::publish_full.epoch_2_receive);
	ASSERT_NE (nano::work_thresholds::publish_full.epoch_2_receive, nano::work_thresholds::publish_full.tier_c ());
	ASSERT_EQ (nano::work_thresholds::publish_full.epoch_1, nano::work_thresholds::publish_full.threshold (nano::work_version::work_1, nano::block_details (nano::epoch::epoch_1, false, true, false)));
	ASSERT_EQ (0, nano::work_thresholds::publish_full.threshold (nano::work_version::work_1, nano::block_details (nano::epoch::epoch_2, false, true, false)));
}
