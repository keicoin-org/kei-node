#pragma once

#include <nano/lib/blocks.hpp>

#include <boost/system/error_code.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace nano
{
/**
 * Wire framing for the variable-length asset block.
 *
 * Every other block type has a compile-time size, so a reader can ask
 * nano::block::size () once and issue a single read. An asset block carries a
 * uint16 payload length in the middle of its own serialisation, so it has to be
 * read in two goes: a fixed prefix up to and including that length field, then
 * a suffix whose size the length field determines.
 *
 * Three separate readers need this — nano::transport::message_deserializer for
 * `publish`, nano::bootstrap::block_deserializer for bulk_pull, and
 * nano::bulk_push_server — and when the framing was written out three times
 * they disagreed about where the suffix goes. This is the one copy.
 */
namespace asset_frame
{
	/**
	 * Largest frame this helper will assemble. The uint16 length field already
	 * caps a frame at serialized_size (65535) == 65802, so this is defence in
	 * depth rather than the operative bound; it stays below the 1024*65 message
	 * ceiling the transport enforces.
	 */
	constexpr std::size_t max_frame_size{ 1024 * 65 };

	/** Same shape as nano::transport::socket::async_read: fill the front of the buffer with `size` bytes. */
	using read_query = std::function<void (std::shared_ptr<std::vector<uint8_t>> const &, std::size_t, std::function<void (boost::system::error_code const &, std::size_t)>)>;

	/** Reports the assembled frame size on success, or why the frame could not be read. */
	using callback_type = std::function<void (boost::system::error_code const &, std::size_t)>;

	/**
	 * Total serialised size of the asset block whose prefix occupies the front
	 * of `prefix`, which must hold at least serialized_prefix_size bytes.
	 */
	std::size_t frame_size (std::vector<uint8_t> const & prefix);

	/**
	 * Read one complete asset block through `read_op` into `buffer`.
	 *
	 * On success the frame occupies buffer[0, frame_size) and the callback
	 * receives frame_size. Note that this is the *frame* size, not the buffer
	 * size: `buffer` is only ever grown, never shrunk, so callers must use the
	 * reported size rather than buffer->size ().
	 *
	 * Never shrinking is the point. All three callers reuse one buffer for
	 * every read on a connection, and nano::transport::socket::async_read
	 * refuses a read larger than buffer->size () — it debug_asserts, and in
	 * release completes with no_buffer_space, which the callers turn into a
	 * dropped connection. A helper that sized the buffer to the frame would
	 * leave a ~267 byte buffer behind and tear the connection down on the next
	 * message of ordinary size.
	 *
	 * Errors: boost::asio::error::message_size if the declared frame is out of
	 * bounds, boost::asio::error::fault on a short read, otherwise whatever
	 * `read_op` reported. A rejected frame is rejected before any of it is
	 * treated as a block, and no read ever runs past the frame boundary.
	 */
	void read (read_query const & read_op, std::shared_ptr<std::vector<uint8_t>> const & buffer, std::size_t max_size, callback_type callback);
}
}
