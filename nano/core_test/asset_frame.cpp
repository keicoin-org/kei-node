#include <nano/lib/blocks.hpp>
#include <nano/node/asset_frame.hpp>
#include <nano/secure/buffer.hpp>
#include <nano/secure/common.hpp>

#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
/**
 * A read function with nano::transport::socket::async_read's semantics: it
 * fills the front of the caller's buffer, never resizes it, and refuses a read
 * larger than the buffer with no_buffer_space — which is how a shrunken reused
 * buffer takes a connection down in production.
 */
class fake_socket
{
public:
	std::vector<uint8_t> bytes;
	std::size_t offset{ 0 };
	std::size_t rejected_reads{ 0 };

	nano::asset_frame::read_query query ()
	{
		return [this] (std::shared_ptr<std::vector<uint8_t>> const & buffer_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a) {
			if (size_a > buffer_a->size ())
			{
				++rejected_reads;
				callback_a (boost::system::errc::make_error_code (boost::system::errc::no_buffer_space), 0);
				return;
			}
			auto const remaining = bytes.size () - std::min (offset, bytes.size ());
			auto const readable = std::min (size_a, remaining);
			std::copy (bytes.begin () + offset, bytes.begin () + offset + readable, buffer_a->data ());
			offset += readable;
			callback_a (boost::system::error_code{}, readable);
		};
	}
};

nano::asset_payload issue_payload (std::string const & description_a)
{
	nano::asset_payload payload;
	payload.name = "Gems";
	payload.symbol = "GEM";
	payload.decimals = 0;
	payload.max_supply = 1000000;
	payload.transfer = nano::transfer_policy::issuer_only;
	payload.swap = nano::swap_policy::one_way;
	payload.description = description_a;
	payload.image = "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi";
	payload.kind = nano::asset_kind::token;
	return payload;
}

// The framing helper never looks at the signature or the work, so a fixed
// keypair and a made-up work value keep these tests free of a running node.
std::vector<uint8_t> serialized_issue_block (std::string const & description_a)
{
	nano::keypair key;
	auto const payload (issue_payload (description_a));
	auto const id (nano::derive_asset_id (key.pub, payload.symbol));
	nano::asset_block block (key.pub, nano::block_hash (0), key.pub, 1000, nano::asset_op::issue, id, 0, 0, payload, key.prv, key.pub, 0xdeadbeef);
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		block.serialize (stream);
	}
	return bytes;
}
}

// The frame has to be assembled contiguously from offset 0, prefix then
// suffix. Writing the suffix anywhere else — at the end of a buffer that was
// never resized to the prefix, say — leaves the block's signature and work
// reading whatever the buffer held before.
TEST (asset_frame, assembles_prefix_and_suffix_contiguously)
{
	auto const block_bytes (serialized_issue_block ("The shop currency"));
	ASSERT_GT (block_bytes.size (), nano::asset_block::serialized_minimum_size ());

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t frame_size) {
		called = true;
		ASSERT_FALSE (ec);
		ASSERT_EQ (frame_size, block_bytes.size ());
		ASSERT_GE (buffer->size (), frame_size);
		ASSERT_TRUE (std::equal (block_bytes.begin (), block_bytes.end (), buffer->begin ()));
	});
	ASSERT_TRUE (called);
	// Exactly the frame was read, nothing beyond it.
	ASSERT_EQ (socket.offset, block_bytes.size ());
	ASSERT_EQ (socket.rejected_reads, 0u);

	// And the assembled bytes really are a block.
	bool error{ false };
	nano::bufferstream stream (buffer->data (), block_bytes.size ());
	nano::asset_block parsed (error, stream);
	ASSERT_FALSE (error);
}

// nano::bulk_push_server's receive_buffer is 256 bytes at construction, which
// is below the 267-byte minimum frame. This is the bulk_push shape: the helper
// has to grow the buffer to fit the frame, and the frame must still land
// contiguously at offset 0.
TEST (asset_frame, grows_a_buffer_smaller_than_the_frame)
{
	auto const block_bytes (serialized_issue_block ("The shop currency"));

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();
	buffer->resize (256);

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t frame_size) {
		called = true;
		ASSERT_FALSE (ec);
		ASSERT_EQ (frame_size, block_bytes.size ());
		ASSERT_TRUE (std::equal (block_bytes.begin (), block_bytes.end (), buffer->begin ()));
	});
	ASSERT_TRUE (called);
	ASSERT_EQ (socket.rejected_reads, 0u);
	ASSERT_GE (buffer->size (), block_bytes.size ());
}

// The mirror image, and the one that poisons a channel: the transport's read
// buffer is MAX_MESSAGE_SIZE and is reused for every message. Sizing it down
// to the frame makes the next ordinary message fail with no_buffer_space.
TEST (asset_frame, never_shrinks_a_buffer_larger_than_the_frame)
{
	auto const block_bytes (serialized_issue_block ("The shop currency"));
	constexpr std::size_t max_message_size{ 1024 * 65 };

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();
	buffer->resize (max_message_size);

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t frame_size) {
		called = true;
		ASSERT_FALSE (ec);
		ASSERT_EQ (frame_size, block_bytes.size ());
		ASSERT_TRUE (std::equal (block_bytes.begin (), block_bytes.end (), buffer->begin ()));
	});
	ASSERT_TRUE (called);
	ASSERT_EQ (buffer->size (), max_message_size);
}

// Two frames of different sizes down one reused buffer, the way a bulk_push or
// bulk_pull stream delivers them. The second must be reported at its own size,
// not at the buffer's, or the tail of the first block is parsed as part of it.
TEST (asset_frame, consecutive_frames_do_not_bleed_into_each_other)
{
	auto const large (serialized_issue_block (std::string (200, 'x')));
	auto const small (serialized_issue_block ("s"));
	ASSERT_GT (large.size (), small.size ());

	fake_socket socket;
	socket.bytes = large;
	socket.bytes.insert (socket.bytes.end (), small.begin (), small.end ());
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	std::size_t first_size{ 0 };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t frame_size) {
		ASSERT_FALSE (ec);
		first_size = frame_size;
	});
	ASSERT_EQ (first_size, large.size ());

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t frame_size) {
		called = true;
		ASSERT_FALSE (ec);
		ASSERT_EQ (frame_size, small.size ());
		ASSERT_TRUE (std::equal (small.begin (), small.end (), buffer->begin ()));
		bool error{ false };
		nano::bufferstream stream (buffer->data (), frame_size);
		nano::asset_block parsed (error, stream);
		ASSERT_FALSE (error);
	});
	ASSERT_TRUE (called);
	ASSERT_EQ (socket.offset, socket.bytes.size ());
}

// A peer that stops short of the frame it declared.
TEST (asset_frame, rejects_a_truncated_suffix)
{
	auto block_bytes (serialized_issue_block ("The shop currency"));
	auto const full_size (block_bytes.size ());
	block_bytes.resize (full_size - 12);

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t) {
		called = true;
		ASSERT_TRUE (ec == boost::asio::error::fault) << ec.message ();
	});
	ASSERT_TRUE (called);
}

// Cut before the length field arrives.
TEST (asset_frame, rejects_a_truncated_prefix)
{
	auto block_bytes (serialized_issue_block ("The shop currency"));
	block_bytes.resize (nano::asset_block::serialized_length_field_offset);

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t) {
		called = true;
		ASSERT_TRUE (ec == boost::asio::error::fault) << ec.message ();
	});
	ASSERT_TRUE (called);
}

// A length field declaring more payload than the peer sends. The uint16 caps a
// frame at 65802, below the 66560 message ceiling, so on the wire an over-long
// declaration shows up as a read that runs out rather than as a bound being
// breached. Nothing past the declared frame may be consumed.
TEST (asset_frame, rejects_a_payload_longer_than_the_peer_sent)
{
	auto block_bytes (serialized_issue_block ("The shop currency"));
	uint16_t const inflated = boost::endian::native_to_little (static_cast<uint16_t> (4096));
	std::memcpy (block_bytes.data () + nano::asset_block::serialized_length_field_offset, &inflated, sizeof (inflated));

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_frame::max_frame_size, [&] (boost::system::error_code const & ec, std::size_t) {
		called = true;
		ASSERT_TRUE (ec == boost::asio::error::fault) << ec.message ();
	});
	ASSERT_TRUE (called);
	ASSERT_EQ (socket.offset, block_bytes.size ());
}

// The explicit bound, which the uint16 makes unreachable on the wire but which
// a caller can still set lower than 65802.
TEST (asset_frame, rejects_a_frame_over_the_caller_s_maximum)
{
	auto const block_bytes (serialized_issue_block (std::string (200, 'x')));

	fake_socket socket;
	socket.bytes = block_bytes;
	auto buffer = std::make_shared<std::vector<uint8_t>> ();

	bool called{ false };
	nano::asset_frame::read (socket.query (), buffer, nano::asset_block::serialized_minimum_size () + 8, [&] (boost::system::error_code const & ec, std::size_t) {
		called = true;
		ASSERT_TRUE (ec == boost::asio::error::message_size) << ec.message ();
	});
	ASSERT_TRUE (called);
	// Rejected on the prefix alone; the suffix was never read.
	ASSERT_EQ (socket.offset, nano::asset_block::serialized_prefix_size);
}
