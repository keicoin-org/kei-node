#include <nano/lib/json_response.hpp>

#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace
{
// A key whose value is already JSON — a number, a boolean, or null — and is
// emitted unquoted. No value under this marker is empty except null's, which is
// what makes "absent" expressible at all.
constexpr char raw_marker = '\x01';
// A key whose child is an array even when it has no elements. Boost infers an
// array from children with empty keys, and the empty array has none to infer
// from, so it would otherwise encode as "".
constexpr char array_marker = '\x02';

std::string mark (char marker_a, std::string const & key_a)
{
	return std::string (1, marker_a) + key_a;
}

bool marked (std::string const & key_a, char marker_a)
{
	return !key_a.empty () && key_a.front () == marker_a;
}

bool is_array (boost::property_tree::ptree const & tree_a)
{
	for (auto const & child : tree_a)
	{
		if (!child.first.empty ())
		{
			return false;
		}
	}
	return !tree_a.empty ();
}

void write_escaped (std::ostream & stream_a, std::string const & value_a)
{
	stream_a << '"';
	for (auto const c : value_a)
	{
		switch (c)
		{
			case '"':
				stream_a << "\\\"";
				break;
			case '\\':
				stream_a << "\\\\";
				break;
			case '\b':
				stream_a << "\\b";
				break;
			case '\f':
				stream_a << "\\f";
				break;
			case '\n':
				stream_a << "\\n";
				break;
			case '\r':
				stream_a << "\\r";
				break;
			case '\t':
				stream_a << "\\t";
				break;
			default:
				// JSON requires an escape below 0x20 and permits one nowhere
				// else, so everything above passes through and a UTF-8 sequence
				// arrives as the character it encodes.
				if (static_cast<unsigned char> (c) < 0x20)
				{
					stream_a << "\\u" << std::hex << std::setw (4) << std::setfill ('0') << static_cast<int> (static_cast<unsigned char> (c)) << std::dec;
				}
				else
				{
					stream_a << c;
				}
				break;
		}
	}
	stream_a << '"';
}

void write_node (std::ostream & stream_a, boost::property_tree::ptree const & tree_a, unsigned depth_a, bool raw_a, bool array_a)
{
	if (tree_a.empty ())
	{
		if (array_a)
		{
			stream_a << "[]";
		}
		else if (raw_a)
		{
			stream_a << (tree_a.data ().empty () ? "null" : tree_a.data ());
		}
		else
		{
			write_escaped (stream_a, tree_a.data ());
		}
		return;
	}
	auto const array (array_a || is_array (tree_a));
	std::string const indent ((depth_a + 1) * 4, ' ');
	stream_a << (array ? "[\n" : "{\n");
	for (auto i (tree_a.begin ()), n (tree_a.end ()); i != n; ++i)
	{
		stream_a << indent;
		if (array)
		{
			write_node (stream_a, i->second, depth_a + 1, false, false);
		}
		else
		{
			auto const raw (marked (i->first, raw_marker));
			auto const child_array (marked (i->first, array_marker));
			write_escaped (stream_a, raw || child_array ? i->first.substr (1) : i->first);
			stream_a << ": ";
			write_node (stream_a, i->second, depth_a + 1, raw, child_array);
		}
		if (std::next (i) != n)
		{
			stream_a << ',';
		}
		stream_a << '\n';
	}
	stream_a << std::string (depth_a * 4, ' ') << (array ? ']' : '}');
}
}

void nano::json::put_number (boost::property_tree::ptree & tree_a, std::string const & key_a, uint64_t value_a)
{
	tree_a.push_back (std::make_pair (mark (raw_marker, key_a), boost::property_tree::ptree (std::to_string (value_a))));
}

void nano::json::put_boolean (boost::property_tree::ptree & tree_a, std::string const & key_a, bool value_a)
{
	tree_a.push_back (std::make_pair (mark (raw_marker, key_a), boost::property_tree::ptree (value_a ? "true" : "false")));
}

void nano::json::put_null (boost::property_tree::ptree & tree_a, std::string const & key_a)
{
	tree_a.push_back (std::make_pair (mark (raw_marker, key_a), boost::property_tree::ptree ()));
}

void nano::json::add_array (boost::property_tree::ptree & tree_a, std::string const & key_a, boost::property_tree::ptree const & value_a)
{
	tree_a.push_back (std::make_pair (mark (array_marker, key_a), value_a));
}

void nano::json::write (std::ostream & stream_a, boost::property_tree::ptree const & tree_a)
{
	// A response is an object even when it carries nothing, where a childless
	// node would otherwise encode as the empty string.
	if (tree_a.empty ())
	{
		stream_a << "{}";
	}
	else
	{
		write_node (stream_a, tree_a, 0, false, false);
	}
	stream_a << '\n';
}

std::string nano::json::to_string (boost::property_tree::ptree const & tree_a)
{
	std::ostringstream stream;
	write (stream, tree_a);
	return stream.str ();
}
