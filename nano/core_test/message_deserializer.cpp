#include <nano/node/transport/message_deserializer.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/endian/conversion.hpp>
#include <boost/none.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
/**
 * Stands in for the socket a deserializer normally reads from. It deliberately
 * copies nano::transport::socket::async_read rather than being convenient: it
 * fills the front of the caller's buffer, it never resizes that buffer, and it
 * refuses a read larger than the buffer with no_buffer_space.
 *
 * The convenient version — resizing the buffer to each read — is what hid the
 * asset framing bugs these tests now guard. Resizing hands the deserializer a
 * correctly sized buffer for free, which is precisely the thing the
 * deserializer has to get right on its own against a real socket.
 */
class fake_channel
{
public:
	std::vector<uint8_t> bytes;
	std::size_t offset{ 0 };

	void append (nano::message & message_a)
	{
		nano::vectorstream stream (bytes);
		message_a.serialize (stream);
	}

	nano::transport::message_deserializer::read_query query ()
	{
		return [this] (std::shared_ptr<std::vector<uint8_t>> const & data_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a) {
			if (size_a > data_a->size ())
			{
				callback_a (boost::system::errc::make_error_code (boost::system::errc::no_buffer_space), 0);
				return;
			}
			// A short read is how a truncated frame reaches the deserializer:
			// fewer bytes than were asked for, and no error.
			auto const remaining = bytes.size () - std::min (offset, bytes.size ());
			auto const readable = std::min (size_a, remaining);
			std::copy (bytes.begin () + offset, bytes.begin () + offset + readable, data_a->data ());
			offset += readable;
			callback_a (boost::system::error_code{}, readable);
		};
	}
};

nano::asset_payload issue_payload ()
{
	nano::asset_payload payload;
	payload.name = "Gems";
	payload.symbol = "GEM";
	payload.decimals = 0;
	payload.max_supply = 1000000;
	payload.transfer = nano::transfer_policy::issuer_only;
	payload.swap = nano::swap_policy::one_way;
	payload.description = "The shop currency";
	payload.image = "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi";
	payload.kind = nano::asset_kind::token;
	return payload;
}

std::shared_ptr<nano::asset_block> issue_block (nano::keypair const & key_a, uint64_t work_a)
{
	auto const payload (issue_payload ());
	auto const id (nano::derive_asset_id (key_a.pub, payload.symbol));
	nano::root const root (1);
	return std::make_shared<nano::asset_block> (key_a.pub, root.previous (), key_a.pub, 1000, nano::asset_op::issue, id, 0, 0, payload, key_a.prv, key_a.pub, work_a);
}
}

// Test the successful cases for message_deserializer, checking the supported message types and
// the integrity of the deserialized outcome.
template <class message_type>
auto message_deserializer_success_checker (message_type & message_original) -> void
{
	// Dependencies for the message deserializer.
	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (message_original);

	auto const message_deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());

	// Deserializing and testing the success path.
	message_deserializer->read (
	[&message_original] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		auto deserialized_message = dynamic_cast<message_type *> (message_a.get ());
		// Ensure the message type is supported.
		ASSERT_NE (deserialized_message, nullptr);
		auto deserialized_bytes = deserialized_message->to_bytes ();
		auto original_bytes = message_original.to_bytes ();
		// Ensure the integrity of the deserialized message.
		ASSERT_EQ (*deserialized_bytes, *original_bytes);
	});
	// This is a sanity test, to ensure the successful deserialization case passes.
	ASSERT_EQ (message_deserializer->status, nano::transport::message_deserializer::parse_status::success);
}

TEST (message_deserializer, exact_confirm_ack)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build_shared ();
	auto vote (std::make_shared<nano::vote> (0, nano::keypair ().prv, 0, 0, std::vector<nano::block_hash>{ block->hash () }));
	nano::confirm_ack message{ nano::dev::network_params.network, vote };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_confirm_req)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build_shared ();
	nano::confirm_req message{ nano::dev::network_params.network, block };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_confirm_req_hash)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build ();
	// This test differs from the previous `exact_confirm_req` because this tests the confirm_req created from the block hash.
	nano::confirm_req message{ nano::dev::network_params.network, block->hash (), block->root () };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_publish)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build_shared ();
	nano::publish message{ nano::dev::network_params.network, block };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_publish_asset)
{
	nano::test::system system{ 1 };
	nano::keypair key1;
	nano::root const root (1);
	auto block = issue_block (key1, *system.work.generate (root));
	nano::publish message{ nano::dev::network_params.network, block };

	message_deserializer_success_checker<decltype (message)> (message);
}

// An asset block is framed by the uint16 length field inside the block, so it
// arrives in two reads. This checks the second read lands where the block
// expects it: if the suffix is written anywhere but offset
// serialized_prefix_size, the signature and work parse as whatever was in the
// buffer before, and the round-trip comparison below fails.
TEST (message_deserializer, publish_asset_assembles_the_whole_frame)
{
	nano::test::system system{ 1 };
	nano::keypair key1;
	nano::root const root (1);
	auto block = issue_block (key1, *system.work.generate (root));
	nano::publish message{ nano::dev::network_params.network, block };

	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (message);

	auto const deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());
	deserializer->read ([&block] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		ASSERT_EQ (ec_a.value (), 0);
		auto published = dynamic_cast<nano::publish *> (message_a.get ());
		ASSERT_NE (published, nullptr);
		ASSERT_NE (published->block.get (), nullptr);
		ASSERT_EQ (published->block->hash (), block->hash ());
		ASSERT_EQ (published->block->block_signature (), block->block_signature ());
		ASSERT_EQ (published->block->block_work (), block->block_work ());
	});
	ASSERT_EQ (deserializer->status, nano::transport::message_deserializer::parse_status::success);
	// Exactly the frame was consumed: no over-read past the block, and no
	// bytes of it left behind to desync the next message on the channel.
	ASSERT_EQ (channel.offset, channel.bytes.size ());
}

// The deserializer's read buffer is a member reused for every message on the
// channel. Sizing it to an asset frame (~267 bytes) leaves the next ordinary
// message with nowhere to go: nano::transport::socket::async_read refuses a
// read larger than the buffer, so a single asset publish would tear the
// connection down on whatever followed it.
TEST (message_deserializer, publish_asset_leaves_the_channel_usable)
{
	nano::test::system system{ 1 };
	nano::keypair key1;
	nano::root const root (1);
	auto block = issue_block (key1, *system.work.generate (root));
	nano::publish asset_message{ nano::dev::network_params.network, block };

	// 15 hashes is the most the header's 4-bit count can carry: a 584-byte
	// payload, comfortably larger than the asset frame, so a buffer left at the
	// frame size cannot serve it.
	std::vector<nano::block_hash> hashes;
	for (auto i = 0; i < 15; ++i)
	{
		hashes.push_back (nano::test::random_hash ());
	}
	auto vote (std::make_shared<nano::vote> (0, nano::keypair ().prv, 0, 0, hashes));
	nano::confirm_ack follow_up{ nano::dev::network_params.network, vote };

	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (asset_message);
	auto const asset_frame_size = channel.bytes.size () - 8;
	channel.append (follow_up);
	ASSERT_GT (follow_up.header.payload_length_bytes (), asset_frame_size);

	auto const deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());
	deserializer->read ([] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		ASSERT_EQ (ec_a.value (), 0);
		ASSERT_NE (dynamic_cast<nano::publish *> (message_a.get ()), nullptr);
	});
	ASSERT_EQ (deserializer->status, nano::transport::message_deserializer::parse_status::success);

	deserializer->read ([&follow_up] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		ASSERT_EQ (ec_a.value (), 0);
		auto acked = dynamic_cast<nano::confirm_ack *> (message_a.get ());
		ASSERT_NE (acked, nullptr);
		ASSERT_EQ (*acked->to_bytes (), *follow_up.to_bytes ());
	});
	ASSERT_EQ (deserializer->status, nano::transport::message_deserializer::parse_status::success);
	ASSERT_EQ (channel.offset, channel.bytes.size ());
}

// A peer that declares a frame and then stops short of it must fail cleanly
// rather than parse whatever the buffer already held.
TEST (message_deserializer, publish_asset_truncated_suffix)
{
	nano::keypair key1;
	// The frame never gets far enough for work to be validated.
	auto block = issue_block (key1, 0);
	nano::publish message{ nano::dev::network_params.network, block };

	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (message);
	// Drop the work, and part of the signature with it.
	channel.bytes.resize (channel.bytes.size () - 12);

	auto const deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());
	bool called{ false };
	deserializer->read ([&called] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		called = true;
		ASSERT_NE (ec_a.value (), 0);
		ASSERT_EQ (message_a.get (), nullptr);
	});
	ASSERT_TRUE (called);
}

// The same, cut before the length field has even arrived.
TEST (message_deserializer, publish_asset_truncated_prefix)
{
	nano::keypair key1;
	auto block = issue_block (key1, 0);
	nano::publish message{ nano::dev::network_params.network, block };

	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (message);
	channel.bytes.resize (8 + nano::asset_block::serialized_length_field_offset);

	auto const deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());
	bool called{ false };
	deserializer->read ([&called] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		called = true;
		ASSERT_NE (ec_a.value (), 0);
		ASSERT_EQ (message_a.get (), nullptr);
	});
	ASSERT_TRUE (called);
}

// A length field claiming more payload than the peer actually sends. The
// uint16 caps the frame below MAX_MESSAGE_SIZE, so this is what an over-long
// declaration looks like on the wire: the read runs out, and the deserializer
// must stop at the frame boundary rather than consume into the next message.
TEST (message_deserializer, publish_asset_declared_length_overruns_the_message)
{
	nano::keypair key1;
	auto block = issue_block (key1, 0);
	nano::publish message{ nano::dev::network_params.network, block };

	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer (block_uniquer);

	fake_channel channel;
	channel.append (message);
	// The length field sits 8 bytes into the message, past the header.
	auto const length_offset = 8 + nano::asset_block::serialized_length_field_offset;
	uint16_t const inflated = boost::endian::native_to_little (static_cast<uint16_t> (4096));
	std::memcpy (channel.bytes.data () + length_offset, &inflated, sizeof (inflated));

	auto const deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer, channel.query ());
	bool called{ false };
	deserializer->read ([&called] (boost::system::error_code ec_a, std::unique_ptr<nano::message> message_a) {
		called = true;
		ASSERT_NE (ec_a.value (), 0);
		ASSERT_EQ (message_a.get (), nullptr);
	});
	ASSERT_TRUE (called);
}

TEST (message_deserializer, exact_keepalive)
{
	nano::keepalive message{ nano::dev::network_params.network };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_frontier_req)
{
	nano::frontier_req message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_telemetry_req)
{
	nano::telemetry_req message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_telemetry_ack)
{
	nano::telemetry_data data;
	data.unknown_data.push_back (0xFF);

	nano::telemetry_ack message{ nano::dev::network_params.network, data };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_pull)
{
	nano::bulk_pull message{ nano::dev::network_params.network };
	message.header.flag_set (nano::message_header::bulk_pull_ascending_flag);

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_pull_account)
{
	nano::bulk_pull_account message{ nano::dev::network_params.network };
	message.flags = nano::bulk_pull_account_flags::pending_address_only;

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_push)
{
	nano::bulk_push message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_node_id_handshake)
{
	nano::node_id_handshake message{ nano::dev::network_params.network, std::nullopt, std::nullopt };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_asc_pull_req)
{
	nano::asc_pull_req message{ nano::dev::network_params.network };

	// The asc_pull_req checks for the message fields and the payload to be filled.
	message.id = 7;
	message.type = nano::asc_pull_type::account_info;

	nano::asc_pull_req::account_info_payload message_payload;
	message_payload.target = nano::test::random_account ();
	message_payload.target_type = nano::asc_pull_req::hash_type::account;

	message.payload = message_payload;
	message.update_header ();

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_asc_pull_ack)
{
	nano::asc_pull_ack message{ nano::dev::network_params.network };

	// The asc_pull_ack checks for the message fields and the payload to be filled.
	message.id = 11;
	message.type = nano::asc_pull_type::account_info;

	nano::asc_pull_ack::account_info_payload message_payload;
	message_payload.account = nano::test::random_account ();
	message_payload.account_open = nano::test::random_hash ();
	message_payload.account_head = nano::test::random_hash ();
	message_payload.account_block_count = 932932132;
	message_payload.account_conf_frontier = nano::test::random_hash ();
	message_payload.account_conf_height = 847312;

	message.payload = message_payload;
	message.update_header ();

	message_deserializer_success_checker<decltype (message)> (message);
}
