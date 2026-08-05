#include <nano/node/asset_frame.hpp>

#include <boost/asio/error.hpp>
#include <boost/endian/conversion.hpp>

#include <cstring>

namespace
{
void grow_to (std::shared_ptr<std::vector<uint8_t>> const & buffer_a, std::size_t size_a)
{
	if (buffer_a->size () < size_a)
	{
		buffer_a->resize (size_a);
	}
}
}

std::size_t nano::asset_frame::frame_size (std::vector<uint8_t> const & prefix_a)
{
	debug_assert (prefix_a.size () >= nano::asset_block::serialized_prefix_size);
	uint16_t payload_size{ 0 };
	std::memcpy (&payload_size, prefix_a.data () + nano::asset_block::serialized_length_field_offset, sizeof (payload_size));
	boost::endian::little_to_native_inplace (payload_size);
	return nano::asset_block::serialized_size (payload_size);
}

void nano::asset_frame::read (nano::asset_frame::read_query const & read_op_a, std::shared_ptr<std::vector<uint8_t>> const & buffer_a, std::size_t max_size_a, nano::asset_frame::callback_type callback_a)
{
	debug_assert (read_op_a);
	debug_assert (callback_a);

	grow_to (buffer_a, nano::asset_block::serialized_prefix_size);
	read_op_a (buffer_a, nano::asset_block::serialized_prefix_size, [read_op = read_op_a, buffer_a, max_size_a, callback = std::move (callback_a)] (boost::system::error_code const & ec, std::size_t size_a) {
		if (ec)
		{
			callback (ec, size_a);
			return;
		}
		if (size_a != nano::asset_block::serialized_prefix_size)
		{
			callback (boost::asio::error::fault, size_a);
			return;
		}
		auto const full_size = nano::asset_frame::frame_size (*buffer_a);
		if (full_size < nano::asset_block::serialized_minimum_size () || full_size > max_size_a)
		{
			callback (boost::asio::error::message_size, size_a);
			return;
		}
		// Guaranteed non-zero by the lower bound above.
		auto const suffix_size = full_size - nano::asset_block::serialized_prefix_size;
		// The suffix cannot be read straight into the tail of `buffer_a`
		// because async_read always fills from offset 0, so it lands in a
		// scratch buffer and is copied into place.
		auto suffix = std::make_shared<std::vector<uint8_t>> (suffix_size);
		read_op (suffix, suffix_size, [buffer_a, suffix, full_size, callback] (boost::system::error_code const & ec, std::size_t size_a) {
			if (ec)
			{
				callback (ec, size_a);
				return;
			}
			if (size_a != suffix->size ())
			{
				callback (boost::asio::error::fault, size_a);
				return;
			}
			grow_to (buffer_a, full_size);
			std::memcpy (buffer_a->data () + nano::asset_block::serialized_prefix_size, suffix->data (), suffix->size ());
			callback (boost::system::error_code{}, full_size);
		});
	});
}
