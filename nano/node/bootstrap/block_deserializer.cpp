#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/block_deserializer.hpp>
#include <nano/node/transport/socket.hpp>
#include <nano/secure/buffer.hpp>

#include <boost/endian/conversion.hpp>
#include <cstring>

nano::bootstrap::block_deserializer::block_deserializer () :
	read_buffer{ std::make_shared<std::vector<uint8_t>> () }
{
}

namespace
{
constexpr std::size_t max_payload_length{ 1024 * 65 };

std::size_t read_asset_block_size (std::vector<uint8_t> const & data)
{
	uint16_t payload_size{ 0 };
	std::memcpy (&payload_size, data.data () + nano::asset_block::serialized_length_field_offset, sizeof (payload_size));
	boost::endian::little_to_native_inplace (payload_size);
	return nano::asset_block::serialized_size (payload_size);
}
}

void nano::bootstrap::block_deserializer::read (nano::transport::socket & socket, callback_type const && callback)
{
	debug_assert (callback);
	read_buffer->resize (1);
	socket.async_read (read_buffer, 1, [this_l = shared_from_this (), &socket, callback = std::move (callback)] (boost::system::error_code const & ec, std::size_t size_a) {
		if (ec)
		{
			callback (ec, nullptr);
			return;
		}
		if (size_a != 1)
		{
			callback (boost::asio::error::fault, nullptr);
			return;
		}
		this_l->received_type (socket, std::move (callback));
	});
}

void nano::bootstrap::block_deserializer::received_type (nano::transport::socket & socket, callback_type const && callback)
{
	nano::block_type type = static_cast<nano::block_type> (read_buffer->data ()[0]);
	if (type == nano::block_type::not_a_block)
	{
		callback (boost::system::error_code{}, nullptr);
		return;
	}
	if (type == nano::block_type::asset)
	{
		auto const size = nano::asset_block::serialized_prefix_size;
		read_buffer->resize (size);
		socket.async_read (read_buffer, size, [this_l = shared_from_this (), callback = std::move (callback)] (boost::system::error_code const & ec, std::size_t size_a) {
			if (ec)
			{
				callback (ec, nullptr);
				return;
			}
			if (size_a != nano::asset_block::serialized_prefix_size)
			{
				callback (boost::asio::error::fault, nullptr);
				return;
			}
			auto const full_size = read_asset_block_size (*this_l->read_buffer);
			if (full_size < nano::asset_block::serialized_minimum_size () || full_size > max_payload_length)
			{
				callback (boost::asio::error::fault, nullptr);
				return;
			}
			auto const suffix_size = full_size - nano::asset_block::serialized_prefix_size;
			auto suffix = std::make_shared<std::vector<uint8_t>> ();
			suffix->resize (suffix_size);
			this_l->read_buffer->reserve (full_size);
			socket.async_read (suffix, suffix_size, [this_l, suffix, full_size, &socket, callback = std::move (callback)] (boost::system::error_code const & ec, std::size_t size_a) {
				if (ec)
				{
					callback (ec, nullptr);
					return;
				}
				if (size_a != suffix->size ())
				{
					callback (boost::asio::error::fault, nullptr);
					return;
				}
				this_l->read_buffer->insert (this_l->read_buffer->end (), suffix->begin (), suffix->end ());
				this_l->read_buffer->resize (full_size);
				this_l->received_block (nano::block_type::asset, std::move (callback));
			});
		});
	}
	else
	{
		auto size = nano::block::size (type);
		if (size == 0)
		{
			callback (boost::asio::error::fault, nullptr);
			return;
		}
		read_buffer->resize (size);
		socket.async_read (read_buffer, size, [this_l = shared_from_this (), size, type, callback = std::move (callback)] (boost::system::error_code const & ec, std::size_t size_a) {
			if (ec)
			{
				callback (ec, nullptr);
				return;
			}
			if (size_a != size)
			{
				callback (boost::asio::error::fault, nullptr);
				return;
			}
			this_l->received_block (type, std::move (callback));
		});
	}
}

void nano::bootstrap::block_deserializer::received_block (nano::block_type type, callback_type const && callback)
{
	nano::bufferstream stream{ read_buffer->data (), read_buffer->size () };
	auto block = nano::deserialize_block (stream, type);
	callback (boost::system::error_code{}, block);
}
