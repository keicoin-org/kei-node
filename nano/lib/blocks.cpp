#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/convert.hpp>
#include <nano/lib/json_response.hpp>
#include <nano/lib/memory.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/threading.hpp>
#include <nano/secure/buffer.hpp>
#include <nano/secure/common.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <bitset>
#include <cstring>
#include <limits>

#include <cryptopp/words.h>

/** Compare blocks, first by type, then content. This is an optimization over dynamic_cast, which is very slow on some platforms. */
namespace
{
template <typename T>
bool blocks_equal (T const & first, nano::block const & second)
{
	static_assert (std::is_base_of<nano::block, T>::value, "Input parameter is not a block type");
	return (first.type () == second.type ()) && (static_cast<T const &> (second)) == first;
}

template <typename block>
std::shared_ptr<block> deserialize_block (nano::stream & stream_a)
{
	auto error (false);
	auto result = nano::make_shared<block> (error, stream_a);
	if (error)
	{
		result = nullptr;
	}

	return result;
}
}

void nano::block_memory_pool_purge ()
{
	nano::purge_shared_ptr_singleton_pool_memory<nano::open_block> ();
	nano::purge_shared_ptr_singleton_pool_memory<nano::state_block> ();
	nano::purge_shared_ptr_singleton_pool_memory<nano::send_block> ();
	nano::purge_shared_ptr_singleton_pool_memory<nano::change_block> ();
}

std::string nano::block::to_json () const
{
	std::string result;
	serialize_json (result);
	return result;
}

size_t nano::block::size (nano::block_type type_a)
{
	size_t result (0);
	switch (type_a)
	{
		case nano::block_type::invalid:
		case nano::block_type::not_a_block:
			debug_assert (false);
			break;
		case nano::block_type::send:
			result = nano::send_block::size;
			break;
		case nano::block_type::receive:
			result = nano::receive_block::size;
			break;
		case nano::block_type::change:
			result = nano::change_block::size;
			break;
		case nano::block_type::open:
			result = nano::open_block::size;
			break;
		case nano::block_type::state:
			result = nano::state_block::size;
			break;
		case nano::block_type::asset:
			// asset_block is variable-length (decisions-m2.md §7) and has no
			// compile-time size, so it cannot answer the fixed-size wire read
			// this dispatcher backs (nano::bootstrap::block_deserializer).
			// M2's definition of done is a single local node — no bootstrap or
			// block relay — so that path deliberately does not support asset
			// blocks yet; 0 makes the deserializer reject rather than
			// misread. Revisit when P2P relay of asset blocks is scoped.
			result = 0;
			break;
	}
	return result;
}

nano::work_version nano::block::work_version () const
{
	return nano::work_version::work_1;
}

nano::block_hash nano::block::generate_hash () const
{
	nano::block_hash result;
	blake2b_state hash_l;
	auto status (blake2b_init (&hash_l, sizeof (result.bytes)));
	debug_assert (status == 0);
	hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

void nano::block::refresh ()
{
	if (!cached_hash.is_zero ())
	{
		cached_hash = generate_hash ();
	}
}

nano::block_hash const & nano::block::hash () const
{
	if (!cached_hash.is_zero ())
	{
		// Once a block is created, it should not be modified (unless using refresh ())
		// This would invalidate the cache; check it hasn't changed.
		debug_assert (cached_hash == generate_hash ());
	}
	else
	{
		cached_hash = generate_hash ();
	}

	return cached_hash;
}

nano::block_hash nano::block::full_hash () const
{
	nano::block_hash result;
	blake2b_state state;
	blake2b_init (&state, sizeof (result.bytes));
	blake2b_update (&state, hash ().bytes.data (), sizeof (hash ()));
	auto signature (block_signature ());
	blake2b_update (&state, signature.bytes.data (), sizeof (signature));
	auto work (block_work ());
	blake2b_update (&state, &work, sizeof (work));
	blake2b_final (&state, result.bytes.data (), sizeof (result.bytes));
	return result;
}

nano::block_sideband const & nano::block::sideband () const
{
	debug_assert (sideband_m.is_initialized ());
	return *sideband_m;
}

void nano::block::sideband_set (nano::block_sideband const & sideband_a)
{
	sideband_m = sideband_a;
}

bool nano::block::has_sideband () const
{
	return sideband_m.is_initialized ();
}

nano::account const & nano::block::representative () const
{
	static nano::account representative{};
	return representative;
}

nano::block_hash const & nano::block::source () const
{
	static nano::block_hash source{ 0 };
	return source;
}

nano::account const & nano::block::destination () const
{
	static nano::account destination{};
	return destination;
}

nano::link const & nano::block::link () const
{
	static nano::link link{ 0 };
	return link;
}

nano::account const & nano::block::account () const
{
	static nano::account account{};
	return account;
}

nano::qualified_root nano::block::qualified_root () const
{
	return nano::qualified_root (root (), previous ());
}

nano::amount const & nano::block::balance () const
{
	static nano::amount amount{ 0 };
	return amount;
}

void nano::send_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.send_block (*this);
}

void nano::send_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.send_block (*this);
}

/**
 * Every Kei block hash begins with this, and no Nano or Banano block hash does.
 *
 * Without it the two chains are separated only at the transport layer — the
 * network id in the packet header, and the fact that the peers do not talk to
 * each other. That is a weaker guarantee than it sounds: the ledger itself
 * cannot tell a Banano state block from a Kei one, because the bytes hashed are
 * identical. A distinct genesis makes replay impractical, since no inherited
 * block chains back to it; this makes it impossible, because an inherited block
 * does not hash to a value its own signature covers.
 *
 * The version suffix is the same device decisions-m0 §2 used for the SDK's
 * "kei-block-v0" preamble, and for the same reason: a later wire format must be
 * unable to collide with this one.
 */
nano::uint256_union const & nano::kei_block_domain ()
{
	static nano::uint256_union const domain = [] () {
		nano::uint256_union result;
		blake2b_state hash;
		auto status (blake2b_init (&hash, sizeof (result.bytes)));
		debug_assert (status == 0);
		char const * const label = "kei-block-v1";
		blake2b_update (&hash, label, std::strlen (label));
		status = blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
		debug_assert (status == 0);
		return result;
	}();
	return domain;
}

void nano::hash_preamble (blake2b_state & hash_a, nano::block_type type_a)
{
	auto const & domain (nano::kei_block_domain ());
	blake2b_update (&hash_a, domain.bytes.data (), domain.bytes.size ());
	nano::uint256_union const type (static_cast<uint64_t> (type_a));
	blake2b_update (&hash_a, type.bytes.data (), type.bytes.size ());
}

void nano::send_block::hash (blake2b_state & hash_a) const
{
	nano::hash_preamble (hash_a, nano::block_type::send);
	hashables.hash (hash_a);
}

uint64_t nano::send_block::block_work () const
{
	return work;
}

void nano::send_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

nano::send_hashables::send_hashables (nano::block_hash const & previous_a, nano::account const & destination_a, nano::amount const & balance_a) :
	previous (previous_a),
	destination (destination_a),
	balance (balance_a)
{
}

nano::send_hashables::send_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, previous.bytes);
		nano::read (stream_a, destination.bytes);
		nano::read (stream_a, balance.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

nano::send_hashables::send_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = destination.decode_account (destination_l);
			if (!error_a)
			{
				error_a = balance.decode_hex (balance_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void nano::send_hashables::hash (blake2b_state & hash_a) const
{
	auto status (blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes)));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, destination.bytes.data (), sizeof (destination.bytes));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	debug_assert (status == 0);
}

void nano::send_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.destination.bytes);
	write (stream_a, hashables.balance.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool nano::send_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.destination.bytes);
		read (stream_a, hashables.balance.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::exception const &)
	{
		error = true;
	}

	return error;
}

void nano::send_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::send_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "send");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	tree.put ("destination", hashables.destination.to_account ());
	std::string balance;
	hashables.balance.encode_hex (balance);
	tree.put ("balance", balance);
	tree.put ("balance_decimal", convert_raw_to_dec (hashables.balance.to_string_dec ()));
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", nano::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool nano::send_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "send");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.destination.decode_account (destination_l);
			if (!error)
			{
				error = hashables.balance.decode_hex (balance_l);
				if (!error)
				{
					error = nano::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

nano::send_block::send_block (nano::block_hash const & previous_a, nano::account const & destination_a, nano::amount const & balance_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (previous_a, destination_a, balance_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (destination_a != nullptr);
	debug_assert (pub_a != nullptr);
}

nano::send_block::send_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature.bytes);
			nano::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::send_block::send_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = nano::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

bool nano::send_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::send_block::valid_predecessor (nano::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case nano::block_type::send:
		case nano::block_type::receive:
		case nano::block_type::open:
		case nano::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

nano::block_type nano::send_block::type () const
{
	return nano::block_type::send;
}

bool nano::send_block::operator== (nano::send_block const & other_a) const
{
	auto result (hashables.destination == other_a.hashables.destination && hashables.previous == other_a.hashables.previous && hashables.balance == other_a.hashables.balance && work == other_a.work && signature == other_a.signature);
	return result;
}

nano::block_hash const & nano::send_block::previous () const
{
	return hashables.previous;
}

nano::account const & nano::send_block::destination () const
{
	return hashables.destination;
}

nano::root const & nano::send_block::root () const
{
	return hashables.previous;
}

nano::amount const & nano::send_block::balance () const
{
	return hashables.balance;
}

nano::signature const & nano::send_block::block_signature () const
{
	return signature;
}

void nano::send_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

nano::open_hashables::open_hashables (nano::block_hash const & source_a, nano::account const & representative_a, nano::account const & account_a) :
	source (source_a),
	representative (representative_a),
	account (account_a)
{
}

nano::open_hashables::open_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, source.bytes);
		nano::read (stream_a, representative.bytes);
		nano::read (stream_a, account.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

nano::open_hashables::open_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		error_a = source.decode_hex (source_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
			if (!error_a)
			{
				error_a = account.decode_account (account_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void nano::open_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
}

nano::open_block::open_block (nano::block_hash const & source_a, nano::account const & representative_a, nano::account const & account_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (source_a, representative_a, account_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (representative_a != nullptr);
	debug_assert (account_a != nullptr);
	debug_assert (pub_a != nullptr);
}

nano::open_block::open_block (nano::block_hash const & source_a, nano::account const & representative_a, nano::account const & account_a, std::nullptr_t) :
	hashables (source_a, representative_a, account_a),
	work (0)
{
	debug_assert (representative_a != nullptr);
	debug_assert (account_a != nullptr);

	signature.clear ();
}

nano::open_block::open_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature);
			nano::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::open_block::open_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = nano::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void nano::open_block::hash (blake2b_state & hash_a) const
{
	nano::hash_preamble (hash_a, nano::block_type::open);
	hashables.hash (hash_a);
}

uint64_t nano::open_block::block_work () const
{
	return work;
}

void nano::open_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

nano::block_hash const & nano::open_block::previous () const
{
	static nano::block_hash result{ 0 };
	return result;
}

nano::account const & nano::open_block::account () const
{
	return hashables.account;
}

void nano::open_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.source);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.account);
	write (stream_a, signature);
	write (stream_a, work);
}

bool nano::open_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.source);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.account);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void nano::open_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::open_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "open");
	tree.put ("source", hashables.source.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("account", hashables.account.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", nano::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool nano::open_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "open");
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.source.decode_hex (source_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = hashables.account.decode_hex (account_l);
				if (!error)
				{
					error = nano::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void nano::open_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.open_block (*this);
}

void nano::open_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.open_block (*this);
}

nano::block_type nano::open_block::type () const
{
	return nano::block_type::open;
}

bool nano::open_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::open_block::operator== (nano::open_block const & other_a) const
{
	return hashables.source == other_a.hashables.source && hashables.representative == other_a.hashables.representative && hashables.account == other_a.hashables.account && work == other_a.work && signature == other_a.signature;
}

bool nano::open_block::valid_predecessor (nano::block const & block_a) const
{
	return false;
}

nano::block_hash const & nano::open_block::source () const
{
	return hashables.source;
}

nano::root const & nano::open_block::root () const
{
	return hashables.account;
}

nano::account const & nano::open_block::representative () const
{
	return hashables.representative;
}

nano::signature const & nano::open_block::block_signature () const
{
	return signature;
}

void nano::open_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

nano::change_hashables::change_hashables (nano::block_hash const & previous_a, nano::account const & representative_a) :
	previous (previous_a),
	representative (representative_a)
{
}

nano::change_hashables::change_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, previous);
		nano::read (stream_a, representative);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

nano::change_hashables::change_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void nano::change_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
}

nano::change_block::change_block (nano::block_hash const & previous_a, nano::account const & representative_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (previous_a, representative_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (representative_a != nullptr);
	debug_assert (pub_a != nullptr);
}

nano::change_block::change_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature);
			nano::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::change_block::change_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = nano::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void nano::change_block::hash (blake2b_state & hash_a) const
{
	nano::hash_preamble (hash_a, nano::block_type::change);
	hashables.hash (hash_a);
}

uint64_t nano::change_block::block_work () const
{
	return work;
}

void nano::change_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

nano::block_hash const & nano::change_block::previous () const
{
	return hashables.previous;
}

void nano::change_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, signature);
	write (stream_a, work);
}

bool nano::change_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void nano::change_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::change_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "change");
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("work", nano::to_string_hex (work));
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
}

bool nano::change_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "change");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = nano::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void nano::change_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.change_block (*this);
}

void nano::change_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.change_block (*this);
}

nano::block_type nano::change_block::type () const
{
	return nano::block_type::change;
}

bool nano::change_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::change_block::operator== (nano::change_block const & other_a) const
{
	return hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && work == other_a.work && signature == other_a.signature;
}

bool nano::change_block::valid_predecessor (nano::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case nano::block_type::send:
		case nano::block_type::receive:
		case nano::block_type::open:
		case nano::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

nano::root const & nano::change_block::root () const
{
	return hashables.previous;
}

nano::account const & nano::change_block::representative () const
{
	return hashables.representative;
}

nano::signature const & nano::change_block::block_signature () const
{
	return signature;
}

void nano::change_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

nano::state_hashables::state_hashables (nano::account const & account_a, nano::block_hash const & previous_a, nano::account const & representative_a, nano::amount const & balance_a, nano::link const & link_a) :
	account (account_a),
	previous (previous_a),
	representative (representative_a),
	balance (balance_a),
	link (link_a)
{
}

nano::state_hashables::state_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, account);
		nano::read (stream_a, previous);
		nano::read (stream_a, representative);
		nano::read (stream_a, balance);
		nano::read (stream_a, link);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

nano::state_hashables::state_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		error_a = account.decode_account (account_l);
		if (!error_a)
		{
			error_a = previous.decode_hex (previous_l);
			if (!error_a)
			{
				error_a = representative.decode_account (representative_l);
				if (!error_a)
				{
					error_a = balance.decode_dec (balance_l);
					if (!error_a)
					{
						error_a = link.decode_account (link_l) && link.decode_hex (link_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void nano::state_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	blake2b_update (&hash_a, link.bytes.data (), sizeof (link.bytes));
}

nano::state_block::state_block (nano::account const & account_a, nano::block_hash const & previous_a, nano::account const & representative_a, nano::amount const & balance_a, nano::link const & link_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (account_a, previous_a, representative_a, balance_a, link_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (account_a != nullptr);
	// A zero representative is ordinarily useless, but it is the consensus
	// null representative required for reserve accounts by SPEC §5.7. Ledger
	// validation decides where it is permitted; block construction must be able
	// to represent it.
	debug_assert (link_a.as_account () != nullptr);
	debug_assert (pub_a != nullptr);
}

nano::state_block::state_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature);
			nano::read (stream_a, work);
			boost::endian::big_to_native_inplace (work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::state_block::state_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto type_l (tree_a.get<std::string> ("type"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = type_l != "state";
			if (!error_a)
			{
				error_a = nano::from_string_hex (work_l, work);
				if (!error_a)
				{
					error_a = signature.decode_hex (signature_l);
				}
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void nano::state_block::hash (blake2b_state & hash_a) const
{
	nano::hash_preamble (hash_a, nano::block_type::state);
	hashables.hash (hash_a);
}

uint64_t nano::state_block::block_work () const
{
	return work;
}

void nano::state_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

nano::block_hash const & nano::state_block::previous () const
{
	return hashables.previous;
}

nano::account const & nano::state_block::account () const
{
	return hashables.account;
}

void nano::state_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.account);
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.balance);
	write (stream_a, hashables.link);
	write (stream_a, signature);
	write (stream_a, boost::endian::native_to_big (work));
}

bool nano::state_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.account);
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.balance);
		read (stream_a, hashables.link);
		read (stream_a, signature);
		read (stream_a, work);
		boost::endian::big_to_native_inplace (work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void nano::state_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::state_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "state");
	tree.put ("account", hashables.account.to_account ());
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("balance", hashables.balance.to_string_dec ());
	tree.put ("balance_decimal", convert_raw_to_dec (hashables.balance.to_string_dec ()));
	tree.put ("link", hashables.link.to_string ());
	tree.put ("link_as_account", hashables.link.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
	tree.put ("work", nano::to_string_hex (work));
}

bool nano::state_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "state");
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.account.decode_account (account_l);
		if (!error)
		{
			error = hashables.previous.decode_hex (previous_l);
			if (!error)
			{
				error = hashables.representative.decode_account (representative_l);
				if (!error)
				{
					error = hashables.balance.decode_dec (balance_l);
					if (!error)
					{
						error = hashables.link.decode_account (link_l) && hashables.link.decode_hex (link_l);
						if (!error)
						{
							error = nano::from_string_hex (work_l, work);
							if (!error)
							{
								error = signature.decode_hex (signature_l);
							}
						}
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void nano::state_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.state_block (*this);
}

void nano::state_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.state_block (*this);
}

nano::block_type nano::state_block::type () const
{
	return nano::block_type::state;
}

bool nano::state_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::state_block::operator== (nano::state_block const & other_a) const
{
	return hashables.account == other_a.hashables.account && hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && hashables.balance == other_a.hashables.balance && hashables.link == other_a.hashables.link && signature == other_a.signature && work == other_a.work;
}

bool nano::state_block::valid_predecessor (nano::block const & block_a) const
{
	return true;
}

nano::root const & nano::state_block::root () const
{
	if (!hashables.previous.is_zero ())
	{
		return hashables.previous;
	}
	else
	{
		return hashables.account;
	}
}

nano::link const & nano::state_block::link () const
{
	return hashables.link;
}

nano::account const & nano::state_block::representative () const
{
	return hashables.representative;
}

nano::amount const & nano::state_block::balance () const
{
	return hashables.balance;
}

nano::signature const & nano::state_block::block_signature () const
{
	return signature;
}

void nano::state_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

bool nano::asset_op_valid (uint8_t raw_a)
{
	return raw_a <= static_cast<uint8_t> (nano::asset_op::claim);
}

char const * nano::asset_op_to_string (nano::asset_op op_a)
{
	switch (op_a)
	{
		case nano::asset_op::issue:
			return "issue";
		case nano::asset_op::mint:
			return "mint";
		case nano::asset_op::burn:
			return "burn";
		case nano::asset_op::transfer:
			return "transfer";
		case nano::asset_op::asset_receive:
			return "asset_receive";
		case nano::asset_op::commit:
			return "commit";
		case nano::asset_op::commit_close:
			return "commit_close";
		case nano::asset_op::claim:
			return "claim";
	}
	return "invalid";
}

bool nano::asset_op_from_string (std::string const & text_a, nano::asset_op & op_a)
{
	bool error (false);
	if (text_a == "issue")
	{
		op_a = nano::asset_op::issue;
	}
	else if (text_a == "mint")
	{
		op_a = nano::asset_op::mint;
	}
	else if (text_a == "burn")
	{
		op_a = nano::asset_op::burn;
	}
	else if (text_a == "transfer")
	{
		op_a = nano::asset_op::transfer;
	}
	else if (text_a == "asset_receive")
	{
		op_a = nano::asset_op::asset_receive;
	}
	else if (text_a == "commit")
	{
		op_a = nano::asset_op::commit;
	}
	else if (text_a == "commit_close")
	{
		op_a = nano::asset_op::commit_close;
	}
	else if (text_a == "claim")
	{
		op_a = nano::asset_op::claim;
	}
	else
	{
		error = true;
	}
	return error;
}

namespace
{
/** Strings in the payload are length-prefixed, little-endian like payload_len itself. */
void write_payload_string (nano::stream & stream_a, std::string const & value_a)
{
	uint16_t const length (static_cast<uint16_t> (value_a.size ()));
	nano::write (stream_a, boost::endian::native_to_little (length));
	if (length > 0)
	{
		auto written (stream_a.sputn (reinterpret_cast<uint8_t const *> (value_a.data ()), length));
		(void)written;
		debug_assert (written == length);
	}
}

bool read_payload_string (nano::stream & stream_a, std::string & value_a, std::size_t max_a)
{
	uint16_t length{ 0 };
	nano::read (stream_a, length);
	boost::endian::little_to_native_inplace (length);
	if (length > max_a)
	{
		return true;
	}
	value_a.resize (length);
	if (length > 0 && stream_a.sgetn (reinterpret_cast<uint8_t *> (&value_a[0]), length) != length)
	{
		throw std::runtime_error ("Failed to read payload string");
	}
	return false;
}

bool transfer_policy_from_string (std::string const & text_a, nano::transfer_policy & policy_a)
{
	bool error (false);
	if (text_a == "open")
	{
		policy_a = nano::transfer_policy::open;
	}
	else if (text_a == "issuer-only")
	{
		policy_a = nano::transfer_policy::issuer_only;
	}
	else if (text_a == "none")
	{
		policy_a = nano::transfer_policy::none;
	}
	else
	{
		error = true;
	}
	return error;
}

bool swap_policy_from_string (std::string const & text_a, nano::swap_policy & policy_a)
{
	bool error (false);
	if (text_a == "two-way")
	{
		policy_a = nano::swap_policy::two_way;
	}
	else if (text_a == "one-way")
	{
		policy_a = nano::swap_policy::one_way;
	}
	else if (text_a == "off")
	{
		policy_a = nano::swap_policy::off;
	}
	else
	{
		error = true;
	}
	return error;
}

bool asset_kind_from_string (std::string const & text_a, nano::asset_kind & kind_a)
{
	bool error (false);
	if (text_a == "token")
	{
		kind_a = nano::asset_kind::token;
	}
	else if (text_a == "item")
	{
		kind_a = nano::asset_kind::item;
	}
	else
	{
		error = true;
	}
	return error;
}
}

char const * nano::transfer_policy_to_string (nano::transfer_policy policy_a)
{
	switch (policy_a)
	{
		case nano::transfer_policy::open:
			return "open";
		case nano::transfer_policy::issuer_only:
			return "issuer-only";
		case nano::transfer_policy::none:
			return "none";
	}
	return "invalid";
}

char const * nano::swap_policy_to_string (nano::swap_policy policy_a)
{
	switch (policy_a)
	{
		case nano::swap_policy::two_way:
			return "two-way";
		case nano::swap_policy::one_way:
			return "one-way";
		case nano::swap_policy::off:
			return "off";
	}
	return "invalid";
}

char const * nano::asset_kind_to_string (nano::asset_kind kind_a)
{
	switch (kind_a)
	{
		case nano::asset_kind::unspecified:
			return "";
		case nano::asset_kind::token:
			return "token";
		case nano::asset_kind::item:
			return "item";
	}
	return "";
}

bool nano::normalize_symbol (std::string const & symbol_a, std::string & result_a)
{
	std::string trimmed (symbol_a);
	auto const whitespace = " \t\n\r\f\v";
	auto first (trimmed.find_first_not_of (whitespace));
	if (first == std::string::npos)
	{
		return true;
	}
	trimmed = trimmed.substr (first, trimmed.find_last_not_of (whitespace) - first + 1);
	if (trimmed.empty () || trimmed.size () > nano::asset_payload::max_symbol)
	{
		return true;
	}
	std::string upper;
	upper.reserve (trimmed.size ());
	for (auto c : trimmed)
	{
		if (c >= 'a' && c <= 'z')
		{
			c = static_cast<char> (c - 'a' + 'A');
		}
		auto const alphanumeric = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!alphanumeric && !(c == '-' && !upper.empty ()))
		{
			// The leading character cannot be "-", which is what stops a symbol
			// from being visually confusable with a negative number.
			return true;
		}
		upper.push_back (c);
	}
	result_a = upper;
	return false;
}

nano::uint256_union nano::derive_asset_id (nano::public_key const & issuer_a, std::string const & symbol_a)
{
	nano::uint256_union result;
	blake2b_state hash;
	auto status (blake2b_init (&hash, sizeof (result.bytes)));
	debug_assert (status == 0);
	blake2b_update (&hash, issuer_a.bytes.data (), issuer_a.bytes.size ());
	blake2b_update (&hash, symbol_a.data (), symbol_a.size ());
	status = blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

namespace
{
/**
 * The two separators that keep a leaf from ever being read as an interior
 * node. This is the frozen SDK's own encoding (`packages/core/src/merkle.ts`
 * in kei-transaction): a single tag byte, not a hashed label. The two must
 * match bit for bit, because the SDK builds the tree and the node only
 * verifies against it — there is no negotiation between them.
 */
constexpr uint8_t claim_leaf_tag = 0x00;
constexpr uint8_t claim_node_tag = 0x01;
}

nano::uint256_union nano::asset_claim_leaf (nano::account const & account_a, nano::uint256_union const & asset_id_a, nano::amount const & amount_a)
{
	nano::uint256_union result;
	blake2b_state hash;
	auto status (blake2b_init (&hash, sizeof (result.bytes)));
	debug_assert (status == 0);
	blake2b_update (&hash, &claim_leaf_tag, sizeof (claim_leaf_tag));
	blake2b_update (&hash, account_a.bytes.data (), account_a.bytes.size ());
	blake2b_update (&hash, asset_id_a.bytes.data (), asset_id_a.bytes.size ());
	blake2b_update (&hash, amount_a.bytes.data (), amount_a.bytes.size ());
	status = blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

nano::uint256_union nano::asset_claim_root (nano::uint256_union const & leaf_a, std::vector<nano::uint256_union> const & proof_a)
{
	auto result (leaf_a);
	for (auto const & sibling : proof_a)
	{
		auto const & first (result < sibling ? result : sibling);
		auto const & second (result < sibling ? sibling : result);
		blake2b_state hash;
		auto status (blake2b_init (&hash, sizeof (result.bytes)));
		debug_assert (status == 0);
		blake2b_update (&hash, &claim_node_tag, sizeof (claim_node_tag));
		blake2b_update (&hash, first.bytes.data (), first.bytes.size ());
		blake2b_update (&hash, second.bytes.data (), second.bytes.size ());
		status = blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
		debug_assert (status == 0);
	}
	return result;
}

bool nano::asset_payload::operator== (nano::asset_payload const & other_a) const
{
	return name == other_a.name && symbol == other_a.symbol && decimals == other_a.decimals && max_supply == other_a.max_supply && transfer == other_a.transfer && swap == other_a.swap && description == other_a.description && image == other_a.image && kind == other_a.kind && memo == other_a.memo && count == other_a.count && proof == other_a.proof;
}

void nano::asset_payload::serialize (nano::stream & stream_a, nano::asset_op op_a) const
{
	switch (op_a)
	{
		case nano::asset_op::issue:
			write_payload_string (stream_a, name);
			write_payload_string (stream_a, symbol);
			nano::write (stream_a, decimals);
			nano::write (stream_a, max_supply);
			nano::write (stream_a, static_cast<uint8_t> (transfer));
			nano::write (stream_a, static_cast<uint8_t> (swap));
			write_payload_string (stream_a, description);
			write_payload_string (stream_a, image);
			nano::write (stream_a, static_cast<uint8_t> (kind));
			break;
		case nano::asset_op::mint:
		case nano::asset_op::transfer:
			write_payload_string (stream_a, memo);
			break;
		case nano::asset_op::commit:
			// Big-endian, like every other integer in a block's hashables. The
			// two length prefixes are the one little-endian exception (§7).
			nano::write (stream_a, boost::endian::native_to_big (count));
			break;
		case nano::asset_op::claim:
		{
			// One length byte is enough for a bound of 48, and it is what keeps
			// a proof canonical: one length, that many siblings, nothing else.
			nano::write (stream_a, static_cast<uint8_t> (proof.size ()));
			for (auto const & sibling : proof)
			{
				nano::write (stream_a, sibling.bytes);
			}
			break;
		}
		case nano::asset_op::burn:
		case nano::asset_op::asset_receive:
		case nano::asset_op::commit_close:
			break;
	}
}

bool nano::asset_payload::deserialize (nano::stream & stream_a, nano::asset_op op_a, std::size_t size_a)
{
	// `burn` and `asset_receive` carry no payload, and an empty one cannot be
	// read through a stream: an empty vector's `data ()` is null, and
	// `nano::bufferstream` is a boost *direct* device, whose first read throws
	// `bad_read` over a null buffer rather than reporting end-of-stream. That
	// throw made every burn and every collect block unreadable once stored —
	// they serialised fine and then failed to parse coming back out.
	//
	// Every other op writes at least a length prefix (a memo's is written even
	// when the memo is empty), so for them a zero-length payload is a malformed
	// encoding rather than the ordinary case.
	if (size_a == 0)
	{
		return op_a != nano::asset_op::burn && op_a != nano::asset_op::asset_receive && op_a != nano::asset_op::commit_close;
	}

	std::vector<uint8_t> bytes;
	try
	{
		nano::read (stream_a, bytes, size_a);
	}
	catch (std::runtime_error const &)
	{
		return true;
	}

	bool error (false);
	nano::bufferstream payload_stream (bytes.data (), bytes.size ());
	try
	{
		switch (op_a)
		{
			case nano::asset_op::issue:
			{
				error = read_payload_string (payload_stream, name, nano::asset_payload::max_name);
				error = error || read_payload_string (payload_stream, symbol, nano::asset_payload::max_symbol);
				if (!error)
				{
					nano::read (payload_stream, decimals);
					nano::read (payload_stream, max_supply);
					uint8_t transfer_raw{ 0 };
					uint8_t swap_raw{ 0 };
					nano::read (payload_stream, transfer_raw);
					nano::read (payload_stream, swap_raw);
					error = transfer_raw > static_cast<uint8_t> (nano::transfer_policy::none) || swap_raw > static_cast<uint8_t> (nano::swap_policy::off);
					transfer = static_cast<nano::transfer_policy> (transfer_raw);
					swap = static_cast<nano::swap_policy> (swap_raw);
				}
				error = error || read_payload_string (payload_stream, description, nano::asset_payload::max_description);
				error = error || read_payload_string (payload_stream, image, nano::asset_payload::max_image);
				if (!error)
				{
					uint8_t kind_raw{ 0 };
					nano::read (payload_stream, kind_raw);
					error = kind_raw > static_cast<uint8_t> (nano::asset_kind::item);
					kind = static_cast<nano::asset_kind> (kind_raw);
				}
				break;
			}
			case nano::asset_op::mint:
			case nano::asset_op::transfer:
				error = read_payload_string (payload_stream, memo, nano::asset_payload::max_memo);
				break;
			case nano::asset_op::commit:
				nano::read (payload_stream, count);
				boost::endian::big_to_native_inplace (count);
				// A drop with no recipients is not a drop, and the ledger says
				// so too — but a block that cannot describe one should not
				// parse into a valid-looking payload in the first place.
				error = count == 0;
				break;
			case nano::asset_op::claim:
			{
				uint8_t proof_length{ 0 };
				nano::read (payload_stream, proof_length);
				error = proof_length > nano::asset_payload::max_proof;
				if (!error)
				{
					proof.resize (proof_length);
					for (auto & sibling : proof)
					{
						nano::read (payload_stream, sibling.bytes);
					}
				}
				break;
			}
			case nano::asset_op::burn:
			case nano::asset_op::asset_receive:
			case nano::asset_op::commit_close:
				break;
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	// A canonical encoding has exactly one representation, so trailing bytes are
	// not "extra data to ignore" — they are a second encoding of the same block,
	// which is precisely what canonical means to rule out.
	return error || !nano::at_end (payload_stream);
}

std::vector<uint8_t> nano::asset_payload::to_bytes (nano::asset_op op_a) const
{
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		serialize (stream, op_a);
	}
	return bytes;
}

nano::asset_hashables::asset_hashables (nano::account const & account_a, nano::block_hash const & previous_a, nano::account const & representative_a, nano::amount const & balance_a, nano::asset_op op_a, nano::uint256_union const & asset_id_a, nano::amount const & amount_a, nano::link const & link_a, nano::asset_payload const & payload_a) :
	account (account_a),
	previous (previous_a),
	representative (representative_a),
	balance (balance_a),
	op (op_a),
	asset_id (asset_id_a),
	amount (amount_a),
	link (link_a),
	payload (payload_a)
{
}

nano::asset_hashables::asset_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, account);
		nano::read (stream_a, previous);
		nano::read (stream_a, representative);
		nano::read (stream_a, balance);
		uint8_t op_raw{ 0 };
		nano::read (stream_a, op_raw);
		error_a = !nano::asset_op_valid (op_raw);
		if (!error_a)
		{
			op = static_cast<nano::asset_op> (op_raw);
			nano::read (stream_a, asset_id);
			nano::read (stream_a, amount);
			nano::read (stream_a, link);
			// payload_len is little-endian on the wire — decisions-m2.md §7, the
			// one field in this layout that departs from Nano's big-endian
			// convention.
			uint16_t payload_len{ 0 };
			nano::read (stream_a, payload_len);
			boost::endian::little_to_native_inplace (payload_len);
			error_a = payload.deserialize (stream_a, op, payload_len);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

/**
 * The JSON shape is the SDK's, not a second one of the node's own invention:
 * `op` is a nested object keyed by `kind`, exactly as `AssetBlockBody` in
 * `@keicoin/core` defines it, because `process` has to accept what the SDK
 * already signs and `docs/rpc.md` promises that reads come back in the shape
 * `process` accepts.
 *
 * The flat fields of the §7 wire layout are derived from it: an `issue` names no
 * asset id because the id is `H(issuer ‖ symbol)`, and a `mint` or `transfer`
 * names a recipient address where the layout carries a public key.
 */
nano::asset_hashables::asset_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		error_a = account.decode_account (account_l);
		error_a = error_a || previous.decode_hex (previous_l);
		error_a = error_a || representative.decode_account (representative_l);
		error_a = error_a || balance.decode_dec (balance_l);
		if (!error_a)
		{
			error_a = deserialize_op_json (tree_a.get_child ("op"));
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
	catch (boost::property_tree::ptree_error const &)
	{
		error_a = true;
	}
}

bool nano::asset_hashables::deserialize_op_json (boost::property_tree::ptree const & op_a)
{
	auto error (nano::asset_op_from_string (op_a.get<std::string> ("kind"), op));
	if (error)
	{
		return error;
	}
	asset_id.clear ();
	amount.clear ();
	link.clear ();
	payload = nano::asset_payload{};

	switch (op)
	{
		case nano::asset_op::issue:
		{
			payload.name = op_a.get<std::string> ("name");
			error = payload.name.empty () || payload.name.size () > nano::asset_payload::max_name;
			error = error || nano::normalize_symbol (op_a.get<std::string> ("symbol"), payload.symbol);
			if (error)
			{
				return error;
			}
			auto const decimals_l (op_a.get<int> ("decimals"));
			error = decimals_l < 0 || decimals_l > 18;
			if (error)
			{
				return error;
			}
			payload.decimals = static_cast<uint8_t> (decimals_l);
			// An absent or null maxSupply is uncapped, which is stored as zero.
			// A maxSupply that is explicitly zero is not the same statement and
			// is refused here rather than silently read as "uncapped".
			auto const max_supply_l (op_a.get<std::string> ("maxSupply", ""));
			if (!max_supply_l.empty ())
			{
				error = payload.max_supply.decode_dec (max_supply_l) || payload.max_supply.is_zero ();
				if (error)
				{
					return error;
				}
			}
			error = transfer_policy_from_string (op_a.get<std::string> ("transfer"), payload.transfer);
			error = error || swap_policy_from_string (op_a.get<std::string> ("swap"), payload.swap);
			if (error)
			{
				return error;
			}
			auto const metadata (op_a.get_child_optional ("metadata"));
			if (metadata)
			{
				payload.description = metadata->get<std::string> ("description", "");
				payload.image = metadata->get<std::string> ("image", "");
				auto const kind_l (metadata->get<std::string> ("kind", ""));
				if (!kind_l.empty ())
				{
					error = asset_kind_from_string (kind_l, payload.kind);
				}
				error = error || payload.description.size () > nano::asset_payload::max_description || payload.image.size () > nano::asset_payload::max_image;
			}
			// Derived, never assigned (SPEC §5.6.1), so an issuance cannot name
			// an id that is not its own.
			asset_id = nano::derive_asset_id (account, payload.symbol);
			break;
		}
		case nano::asset_op::mint:
		case nano::asset_op::transfer:
		{
			nano::account to;
			error = asset_id.decode_hex (op_a.get<std::string> ("asset"));
			error = error || to.decode_account (op_a.get<std::string> ("to"));
			error = error || amount.decode_dec (op_a.get<std::string> ("amount"));
			link = to;
			payload.memo = op_a.get<std::string> ("memo", "");
			error = error || payload.memo.size () > nano::asset_payload::max_memo;
			break;
		}
		case nano::asset_op::burn:
		{
			error = asset_id.decode_hex (op_a.get<std::string> ("asset"));
			error = error || amount.decode_dec (op_a.get<std::string> ("amount"));
			break;
		}
		case nano::asset_op::asset_receive:
		{
			// The source block hash. Which asset it pays is the receivable's
			// business, not the collecting block's (decisions-m2.md §10).
			nano::block_hash source;
			error = source.decode_hex (op_a.get<std::string> ("link"));
			link = source;
			break;
		}
		case nano::asset_op::commit:
		{
			// The root travels in `link`: it is 32 bytes and the fixed header
			// already carries one such field, so the rooted ops need no new
			// layout at all (decisions-m4.md §2).
			nano::block_hash root;
			error = asset_id.decode_hex (op_a.get<std::string> ("asset"));
			error = error || amount.decode_dec (op_a.get<std::string> ("total"));
			error = error || root.decode_hex (op_a.get<std::string> ("root"));
			link = root;
			auto const count_l (op_a.get<uint64_t> ("count"));
			error = error || count_l == 0 || count_l > std::numeric_limits<uint32_t>::max ();
			payload.count = static_cast<uint32_t> (count_l);
			break;
		}
		case nano::asset_op::commit_close:
		{
			nano::block_hash root;
			error = root.decode_hex (op_a.get<std::string> ("root"));
			link = root;
			break;
		}
		case nano::asset_op::claim:
		{
			nano::block_hash root;
			error = asset_id.decode_hex (op_a.get<std::string> ("asset"));
			error = error || amount.decode_dec (op_a.get<std::string> ("amount"));
			error = error || root.decode_hex (op_a.get<std::string> ("root"));
			link = root;
			for (auto const & entry : op_a.get_child ("proof"))
			{
				nano::uint256_union sibling;
				error = error || sibling.decode_hex (entry.second.get_value<std::string> ());
				payload.proof.push_back (sibling);
			}
			error = error || payload.proof.size () > nano::asset_payload::max_proof;
			break;
		}
	}
	return error;
}

void nano::asset_hashables::serialize_op_json (boost::property_tree::ptree & op_a) const
{
	op_a.put ("kind", nano::asset_op_to_string (op));
	switch (op)
	{
		case nano::asset_op::issue:
		{
			op_a.put ("name", payload.name);
			op_a.put ("symbol", payload.symbol);
			op_a.put ("decimals", static_cast<int> (payload.decimals));
			if (payload.max_supply.is_zero ())
			{
				op_a.put ("maxSupply", "");
			}
			else
			{
				op_a.put ("maxSupply", payload.max_supply.to_string_dec ());
			}
			op_a.put ("transfer", nano::transfer_policy_to_string (payload.transfer));
			op_a.put ("swap", nano::swap_policy_to_string (payload.swap));
			if (!payload.description.empty () || !payload.image.empty () || payload.kind != nano::asset_kind::unspecified)
			{
				boost::property_tree::ptree metadata;
				if (!payload.description.empty ())
				{
					metadata.put ("description", payload.description);
				}
				if (!payload.image.empty ())
				{
					metadata.put ("image", payload.image);
				}
				if (payload.kind != nano::asset_kind::unspecified)
				{
					metadata.put ("kind", nano::asset_kind_to_string (payload.kind));
				}
				op_a.add_child ("metadata", metadata);
			}
			break;
		}
		case nano::asset_op::mint:
		case nano::asset_op::transfer:
		{
			op_a.put ("asset", asset_id.to_string ());
			op_a.put ("to", link.as_account ().to_account ());
			op_a.put ("amount", amount.to_string_dec ());
			if (!payload.memo.empty ())
			{
				op_a.put ("memo", payload.memo);
			}
			break;
		}
		case nano::asset_op::burn:
		{
			op_a.put ("asset", asset_id.to_string ());
			op_a.put ("amount", amount.to_string_dec ());
			break;
		}
		case nano::asset_op::asset_receive:
		{
			op_a.put ("link", link.as_block_hash ().to_string ());
			break;
		}
		case nano::asset_op::commit:
		{
			op_a.put ("asset", asset_id.to_string ());
			op_a.put ("root", link.as_block_hash ().to_string ());
			nano::json::put_number (op_a, "count", payload.count);
			op_a.put ("total", amount.to_string_dec ());
			break;
		}
		case nano::asset_op::commit_close:
		{
			op_a.put ("root", link.as_block_hash ().to_string ());
			break;
		}
		case nano::asset_op::claim:
		{
			op_a.put ("root", link.as_block_hash ().to_string ());
			op_a.put ("asset", asset_id.to_string ());
			op_a.put ("amount", amount.to_string_dec ());
			boost::property_tree::ptree proof_l;
			for (auto const & sibling : payload.proof)
			{
				boost::property_tree::ptree entry;
				entry.put ("", sibling.to_string ());
				proof_l.push_back (std::make_pair ("", entry));
			}
			// A one-leaf drop has no siblings, and an array that collapses to ""
			// when empty is a different type to whatever parses this.
			nano::json::add_array (op_a, "proof", proof_l);
			break;
		}
	}
}

void nano::asset_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	uint8_t const op_byte (static_cast<uint8_t> (op));
	blake2b_update (&hash_a, &op_byte, sizeof (op_byte));
	blake2b_update (&hash_a, asset_id.bytes.data (), sizeof (asset_id.bytes));
	blake2b_update (&hash_a, amount.bytes.data (), sizeof (amount.bytes));
	blake2b_update (&hash_a, link.bytes.data (), sizeof (link.bytes));
	// The length is hashed alongside the bytes. The payload is the last field,
	// so length-prefixing is not strictly needed for injectivity here, but it
	// keeps the hash covering exactly what the wire carries.
	auto const bytes (payload.to_bytes (op));
	uint16_t const payload_len (boost::endian::native_to_little (static_cast<uint16_t> (bytes.size ())));
	blake2b_update (&hash_a, &payload_len, sizeof (payload_len));
	if (!bytes.empty ())
	{
		blake2b_update (&hash_a, bytes.data (), bytes.size ());
	}
}

nano::asset_block::asset_block (nano::account const & account_a, nano::block_hash const & previous_a, nano::account const & representative_a, nano::amount const & balance_a, nano::asset_op op_a, nano::uint256_union const & asset_id_a, nano::amount const & amount_a, nano::link const & link_a, nano::asset_payload const & payload_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (account_a, previous_a, representative_a, balance_a, op_a, asset_id_a, amount_a, link_a, payload_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (account_a != nullptr);
	// Reserve asset blocks retain the required null representative. Whether a
	// given operation is permitted is a ledger rule, not a representability
	// rule for the block object.
	debug_assert (pub_a != nullptr);
}

nano::asset_block::asset_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature);
			nano::read (stream_a, work);
			boost::endian::big_to_native_inplace (work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::asset_block::asset_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto type_l (tree_a.get<std::string> ("type"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = type_l != "asset";
			if (!error_a)
			{
				error_a = nano::from_string_hex (work_l, work);
				if (!error_a)
				{
					error_a = signature.decode_hex (signature_l);
				}
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void nano::asset_block::hash (blake2b_state & hash_a) const
{
	// A preamble distinct from every inherited block type's, so an asset block
	// cannot collide with a state block even given equal field content —
	// decisions-m2.md §7.
	nano::hash_preamble (hash_a, nano::block_type::asset);
	hashables.hash (hash_a);
}

uint64_t nano::asset_block::block_work () const
{
	return work;
}

void nano::asset_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

nano::block_hash const & nano::asset_block::previous () const
{
	return hashables.previous;
}

nano::account const & nano::asset_block::account () const
{
	return hashables.account;
}

void nano::asset_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.account);
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.balance);
	write (stream_a, static_cast<uint8_t> (hashables.op));
	write (stream_a, hashables.asset_id);
	write (stream_a, hashables.amount);
	write (stream_a, hashables.link);
	auto const payload_bytes (hashables.payload.to_bytes (hashables.op));
	uint16_t const payload_len (static_cast<uint16_t> (payload_bytes.size ()));
	write (stream_a, boost::endian::native_to_little (payload_len));
	write (stream_a, payload_bytes);
	write (stream_a, signature);
	write (stream_a, boost::endian::native_to_big (work));
}

bool nano::asset_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.account);
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.balance);
		uint8_t op_raw{ 0 };
		read (stream_a, op_raw);
		error = !nano::asset_op_valid (op_raw);
		if (!error)
		{
			hashables.op = static_cast<nano::asset_op> (op_raw);
			read (stream_a, hashables.asset_id);
			read (stream_a, hashables.amount);
			read (stream_a, hashables.link);
			uint16_t payload_len{ 0 };
			read (stream_a, payload_len);
			boost::endian::little_to_native_inplace (payload_len);
			error = hashables.payload.deserialize (stream_a, hashables.op, payload_len);
		}
		if (!error)
		{
			read (stream_a, signature);
			read (stream_a, work);
			boost::endian::big_to_native_inplace (work);
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void nano::asset_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::asset_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "asset");
	tree.put ("account", hashables.account.to_account ());
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("balance", hashables.balance.to_string_dec ());
	boost::property_tree::ptree op;
	hashables.serialize_op_json (op);
	tree.add_child ("op", op);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
	tree.put ("work", nano::to_string_hex (work));
}

bool nano::asset_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "asset");
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.account.decode_account (account_l);
		error = error || hashables.previous.decode_hex (previous_l);
		error = error || hashables.representative.decode_account (representative_l);
		error = error || hashables.balance.decode_dec (balance_l);
		if (!error)
		{
			error = hashables.deserialize_op_json (tree_a.get_child ("op"));
		}
		error = error || nano::from_string_hex (work_l, work);
		error = error || signature.decode_hex (signature_l);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	catch (boost::property_tree::ptree_error const &)
	{
		error = true;
	}
	return error;
}

void nano::asset_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.asset_block (*this);
}

void nano::asset_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.asset_block (*this);
}

nano::block_type nano::asset_block::type () const
{
	return nano::block_type::asset;
}

bool nano::asset_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::asset_block::operator== (nano::asset_block const & other_a) const
{
	return hashables.account == other_a.hashables.account && hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && hashables.balance == other_a.hashables.balance && hashables.op == other_a.hashables.op && hashables.asset_id == other_a.hashables.asset_id && hashables.amount == other_a.hashables.amount && hashables.link == other_a.hashables.link && hashables.payload == other_a.hashables.payload && signature == other_a.signature && work == other_a.work;
}

bool nano::asset_block::valid_predecessor (nano::block const & block_a) const
{
	return true;
}

nano::root const & nano::asset_block::root () const
{
	if (!hashables.previous.is_zero ())
	{
		return hashables.previous;
	}
	else
	{
		return hashables.account;
	}
}

nano::link const & nano::asset_block::link () const
{
	return hashables.link;
}

nano::account const & nano::asset_block::representative () const
{
	return hashables.representative;
}

nano::amount const & nano::asset_block::balance () const
{
	return hashables.balance;
}

nano::signature const & nano::asset_block::block_signature () const
{
	return signature;
}

void nano::asset_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

std::shared_ptr<nano::block> nano::deserialize_block_json (boost::property_tree::ptree const & tree_a, nano::block_uniquer * uniquer_a)
{
	std::shared_ptr<nano::block> result;
	try
	{
		auto type (tree_a.get<std::string> ("type"));
		bool error (false);
		std::unique_ptr<nano::block> obj;
		if (type == "receive")
		{
			obj = std::make_unique<nano::receive_block> (error, tree_a);
		}
		else if (type == "send")
		{
			obj = std::make_unique<nano::send_block> (error, tree_a);
		}
		else if (type == "open")
		{
			obj = std::make_unique<nano::open_block> (error, tree_a);
		}
		else if (type == "change")
		{
			obj = std::make_unique<nano::change_block> (error, tree_a);
		}
		else if (type == "state")
		{
			obj = std::make_unique<nano::state_block> (error, tree_a);
		}
		else if (type == "asset")
		{
			obj = std::make_unique<nano::asset_block> (error, tree_a);
		}

		if (!error)
		{
			result = std::move (obj);
		}
	}
	catch (std::runtime_error const &)
	{
	}
	if (uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

void nano::serialize_block_type (nano::stream & stream, const nano::block_type & type)
{
	nano::write (stream, type);
}

void nano::serialize_block (nano::stream & stream_a, nano::block const & block_a)
{
	nano::serialize_block_type (stream_a, block_a.type ());
	block_a.serialize (stream_a);
}

std::shared_ptr<nano::block> nano::deserialize_block (nano::stream & stream_a)
{
	nano::block_type type;
	auto error (try_read (stream_a, type));
	std::shared_ptr<nano::block> result;
	if (!error)
	{
		result = nano::deserialize_block (stream_a, type);
	}
	return result;
}

std::shared_ptr<nano::block> nano::deserialize_block (nano::stream & stream_a, nano::block_type type_a, nano::block_uniquer * uniquer_a)
{
	std::shared_ptr<nano::block> result;
	switch (type_a)
	{
		case nano::block_type::receive:
		{
			result = ::deserialize_block<nano::receive_block> (stream_a);
			break;
		}
		case nano::block_type::send:
		{
			result = ::deserialize_block<nano::send_block> (stream_a);
			break;
		}
		case nano::block_type::open:
		{
			result = ::deserialize_block<nano::open_block> (stream_a);
			break;
		}
		case nano::block_type::change:
		{
			result = ::deserialize_block<nano::change_block> (stream_a);
			break;
		}
		case nano::block_type::state:
		{
			result = ::deserialize_block<nano::state_block> (stream_a);
			break;
		}
		case nano::block_type::asset:
		{
			result = ::deserialize_block<nano::asset_block> (stream_a);
			break;
		}
		default:
		{
			return {};
		}
	}
	if (result && uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

void nano::receive_block::visit (nano::block_visitor & visitor_a) const
{
	visitor_a.receive_block (*this);
}

void nano::receive_block::visit (nano::mutable_block_visitor & visitor_a)
{
	visitor_a.receive_block (*this);
}

bool nano::receive_block::operator== (nano::receive_block const & other_a) const
{
	auto result (hashables.previous == other_a.hashables.previous && hashables.source == other_a.hashables.source && work == other_a.work && signature == other_a.signature);
	return result;
}

void nano::receive_block::serialize (nano::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.source.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool nano::receive_block::deserialize (nano::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.source.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void nano::receive_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void nano::receive_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "receive");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	std::string source;
	hashables.source.encode_hex (source);
	tree.put ("source", source);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", nano::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool nano::receive_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "receive");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.source.decode_hex (source_l);
			if (!error)
			{
				error = nano::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

nano::receive_block::receive_block (nano::block_hash const & previous_a, nano::block_hash const & source_a, nano::raw_key const & prv_a, nano::public_key const & pub_a, uint64_t work_a) :
	hashables (previous_a, source_a),
	signature (nano::sign_message (prv_a, pub_a, hash ())),
	work (work_a)
{
	debug_assert (pub_a != nullptr);
}

nano::receive_block::receive_block (bool & error_a, nano::stream & stream_a) :
	hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			nano::read (stream_a, signature);
			nano::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

nano::receive_block::receive_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
	hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = nano::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void nano::receive_block::hash (blake2b_state & hash_a) const
{
	nano::hash_preamble (hash_a, nano::block_type::receive);
	hashables.hash (hash_a);
}

uint64_t nano::receive_block::block_work () const
{
	return work;
}

void nano::receive_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

bool nano::receive_block::operator== (nano::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool nano::receive_block::valid_predecessor (nano::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case nano::block_type::send:
		case nano::block_type::receive:
		case nano::block_type::open:
		case nano::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

nano::block_hash const & nano::receive_block::previous () const
{
	return hashables.previous;
}

nano::block_hash const & nano::receive_block::source () const
{
	return hashables.source;
}

nano::root const & nano::receive_block::root () const
{
	return hashables.previous;
}

nano::signature const & nano::receive_block::block_signature () const
{
	return signature;
}

void nano::receive_block::signature_set (nano::signature const & signature_a)
{
	signature = signature_a;
}

nano::block_type nano::receive_block::type () const
{
	return nano::block_type::receive;
}

nano::receive_hashables::receive_hashables (nano::block_hash const & previous_a, nano::block_hash const & source_a) :
	previous (previous_a),
	source (source_a)
{
}

nano::receive_hashables::receive_hashables (bool & error_a, nano::stream & stream_a)
{
	try
	{
		nano::read (stream_a, previous.bytes);
		nano::read (stream_a, source.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

nano::receive_hashables::receive_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = source.decode_hex (source_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void nano::receive_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
}

nano::block_details::block_details (nano::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a) :
	epoch (epoch_a), is_send (is_send_a), is_receive (is_receive_a), is_epoch (is_epoch_a)
{
}

bool nano::block_details::operator== (nano::block_details const & other_a) const
{
	return epoch == other_a.epoch && is_send == other_a.is_send && is_receive == other_a.is_receive && is_epoch == other_a.is_epoch;
}

uint8_t nano::block_details::packed () const
{
	std::bitset<8> result (static_cast<uint8_t> (epoch));
	result.set (7, is_send);
	result.set (6, is_receive);
	result.set (5, is_epoch);
	return static_cast<uint8_t> (result.to_ulong ());
}

void nano::block_details::unpack (uint8_t details_a)
{
	constexpr std::bitset<8> epoch_mask{ 0b00011111 };
	auto as_bitset = static_cast<std::bitset<8>> (details_a);
	is_send = as_bitset.test (7);
	is_receive = as_bitset.test (6);
	is_epoch = as_bitset.test (5);
	epoch = static_cast<nano::epoch> ((as_bitset & epoch_mask).to_ulong ());
}

void nano::block_details::serialize (nano::stream & stream_a) const
{
	nano::write (stream_a, packed ());
}

bool nano::block_details::deserialize (nano::stream & stream_a)
{
	bool result (false);
	try
	{
		uint8_t packed{ 0 };
		nano::read (stream_a, packed);
		unpack (packed);
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::string nano::state_subtype (nano::block_details const details_a)
{
	debug_assert (details_a.is_epoch + details_a.is_receive + details_a.is_send <= 1);
	if (details_a.is_send)
	{
		return "send";
	}
	else if (details_a.is_receive)
	{
		return "receive";
	}
	else if (details_a.is_epoch)
	{
		return "epoch";
	}
	else
	{
		return "change";
	}
}

nano::block_sideband::block_sideband (nano::account const & account_a, nano::block_hash const & successor_a, nano::amount const & balance_a, uint64_t const height_a, nano::seconds_t const timestamp_a, nano::block_details const & details_a, nano::epoch const source_epoch_a) :
	successor (successor_a),
	account (account_a),
	balance (balance_a),
	height (height_a),
	timestamp (timestamp_a),
	details (details_a),
	source_epoch (source_epoch_a)
{
}

nano::block_sideband::block_sideband (nano::account const & account_a, nano::block_hash const & successor_a, nano::amount const & balance_a, uint64_t const height_a, nano::seconds_t const timestamp_a, nano::epoch const epoch_a, bool const is_send, bool const is_receive, bool const is_epoch, nano::epoch const source_epoch_a) :
	successor (successor_a),
	account (account_a),
	balance (balance_a),
	height (height_a),
	timestamp (timestamp_a),
	details (epoch_a, is_send, is_receive, is_epoch),
	source_epoch (source_epoch_a)
{
}

size_t nano::block_sideband::size (nano::block_type type_a)
{
	size_t result (0);
	result += sizeof (successor);
	if (type_a != nano::block_type::state && type_a != nano::block_type::open && type_a != nano::block_type::asset)
	{
		result += sizeof (account);
	}
	if (type_a != nano::block_type::open)
	{
		result += sizeof (height);
	}
	if (type_a == nano::block_type::receive || type_a == nano::block_type::change || type_a == nano::block_type::open)
	{
		result += sizeof (balance);
	}
	result += sizeof (timestamp);
	if (type_a == nano::block_type::state || type_a == nano::block_type::asset)
	{
		static_assert (sizeof (nano::epoch) == nano::block_details::size (), "block_details is larger than the epoch enum");
		result += nano::block_details::size () + sizeof (nano::epoch);
	}
	return result;
}

void nano::block_sideband::serialize (nano::stream & stream_a, nano::block_type type_a) const
{
	nano::write (stream_a, successor.bytes);
	if (type_a != nano::block_type::state && type_a != nano::block_type::open && type_a != nano::block_type::asset)
	{
		nano::write (stream_a, account.bytes);
	}
	if (type_a != nano::block_type::open)
	{
		nano::write (stream_a, boost::endian::native_to_big (height));
	}
	if (type_a == nano::block_type::receive || type_a == nano::block_type::change || type_a == nano::block_type::open)
	{
		nano::write (stream_a, balance.bytes);
	}
	nano::write (stream_a, boost::endian::native_to_big (timestamp));
	if (type_a == nano::block_type::state || type_a == nano::block_type::asset)
	{
		details.serialize (stream_a);
		nano::write (stream_a, static_cast<uint8_t> (source_epoch));
	}
}

bool nano::block_sideband::deserialize (nano::stream & stream_a, nano::block_type type_a)
{
	bool result (false);
	try
	{
		nano::read (stream_a, successor.bytes);
		if (type_a != nano::block_type::state && type_a != nano::block_type::open && type_a != nano::block_type::asset)
		{
			nano::read (stream_a, account.bytes);
		}
		if (type_a != nano::block_type::open)
		{
			nano::read (stream_a, height);
			boost::endian::big_to_native_inplace (height);
		}
		else
		{
			height = 1;
		}
		if (type_a == nano::block_type::receive || type_a == nano::block_type::change || type_a == nano::block_type::open)
		{
			nano::read (stream_a, balance.bytes);
		}
		nano::read (stream_a, timestamp);
		boost::endian::big_to_native_inplace (timestamp);
		if (type_a == nano::block_type::state || type_a == nano::block_type::asset)
		{
			result = details.deserialize (stream_a);
			uint8_t source_epoch_uint8_t{ 0 };
			nano::read (stream_a, source_epoch_uint8_t);
			source_epoch = static_cast<nano::epoch> (source_epoch_uint8_t);
		}
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::shared_ptr<nano::block> nano::block_uniquer::unique (std::shared_ptr<nano::block> const & block_a)
{
	auto result (block_a);
	if (result != nullptr)
	{
		nano::uint256_union key (block_a->full_hash ());
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto & existing (blocks[key]);
		if (auto block_l = existing.lock ())
		{
			result = block_l;
		}
		else
		{
			existing = block_a;
		}
		release_assert (std::numeric_limits<CryptoPP::word32>::max () > blocks.size ());
		for (auto i (0); i < cleanup_count && !blocks.empty (); ++i)
		{
			auto random_offset (nano::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (blocks.size () - 1)));
			auto existing (std::next (blocks.begin (), random_offset));
			if (existing == blocks.end ())
			{
				existing = blocks.begin ();
			}
			if (existing != blocks.end ())
			{
				if (auto block_l = existing->second.lock ())
				{
					// Still live
				}
				else
				{
					blocks.erase (existing);
				}
			}
		}
	}
	return result;
}

size_t nano::block_uniquer::size ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return blocks.size ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (block_uniquer & block_uniquer, std::string const & name)
{
	auto count = block_uniquer.size ();
	auto sizeof_element = sizeof (block_uniquer::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", count, sizeof_element }));
	return composite;
}
