#include <nano/lib/json_response.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <sstream>

namespace
{
/** What the writer produced, with the trailing newline dropped. */
std::string encoded (boost::property_tree::ptree const & tree_a)
{
	auto result (nano::json::to_string (tree_a));
	if (!result.empty () && result.back () == '\n')
	{
		result.pop_back ();
	}
	return result;
}
}

// An unmarked tree has to encode exactly as boost's pretty writer encoded it,
// because every inherited endpoint's output goes through here now.
TEST (json_response, matches_boost_for_plain_values)
{
	boost::property_tree::ptree tree;
	tree.put ("frontier", "A1B2");
	boost::property_tree::ptree child;
	child.put ("count", "3");
	tree.add_child ("nested", child);
	boost::property_tree::ptree array;
	array.push_back (std::make_pair ("", boost::property_tree::ptree ("first")));
	array.push_back (std::make_pair ("", boost::property_tree::ptree ("second")));
	tree.add_child ("list", array);

	std::stringstream expected;
	boost::property_tree::write_json (expected, tree);
	ASSERT_EQ (expected.str (), nano::json::to_string (tree));
}

TEST (json_response, numbers_are_unquoted)
{
	boost::property_tree::ptree tree;
	nano::json::put_number (tree, "height", 12);
	nano::json::put_number (tree, "decimals", 0);
	ASSERT_EQ ("{\n    \"height\": 12,\n    \"decimals\": 0\n}", encoded (tree));
}

TEST (json_response, booleans_and_null)
{
	boost::property_tree::ptree tree;
	nano::json::put_boolean (tree, "closed", false);
	nano::json::put_boolean (tree, "claimed", true);
	nano::json::put_null (tree, "account");
	ASSERT_EQ ("{\n    \"closed\": false,\n    \"claimed\": true,\n    \"account\": null\n}", encoded (tree));
}

// docs/rpc.md: anything absent is null or an empty array, never an error, and
// an empty ptree child is otherwise indistinguishable from an empty string.
TEST (json_response, empty_array_stays_an_array)
{
	boost::property_tree::ptree tree;
	nano::json::add_array (tree, "holdings", boost::property_tree::ptree ());
	ASSERT_EQ ("{\n    \"holdings\": []\n}", encoded (tree));
}

TEST (json_response, populated_array)
{
	boost::property_tree::ptree entry;
	entry.put ("asset", "A1B2");
	entry.put ("balance", "500");
	boost::property_tree::ptree holdings;
	holdings.push_back (std::make_pair ("", entry));
	boost::property_tree::ptree tree;
	nano::json::add_array (tree, "holdings", holdings);
	ASSERT_EQ ("{\n    \"holdings\": [\n        {\n            \"asset\": \"A1B2\",\n            \"balance\": \"500\"\n        }\n    ]\n}", encoded (tree));
}

// The marker travels on the key precisely so that a value cannot carry one. An
// asset's name is chosen by whoever signed the issuance block.
TEST (json_response, a_marker_in_a_value_is_only_ever_a_string)
{
	boost::property_tree::ptree tree;
	tree.put ("name", std::string ("\x01") + "true");
	ASSERT_EQ ("{\n    \"name\": \"\\u0001true\"\n}", encoded (tree));
}

TEST (json_response, escapes_what_json_requires_and_nothing_else)
{
	boost::property_tree::ptree tree;
	tree.put ("memo", "a\"b\\c\nd\te");
	ASSERT_EQ ("{\n    \"memo\": \"a\\\"b\\\\c\\nd\\te\"\n}", encoded (tree));
}

// Boost escapes each byte of a UTF-8 sequence separately, which does not decode
// back to the character. An asset name and a memo are user text.
TEST (json_response, utf8_survives)
{
	boost::property_tree::ptree tree;
	tree.put ("name", "Gems \xE2\x9C\x93");
	auto const result (encoded (tree));
	ASSERT_NE (std::string::npos, result.find ("Gems \xE2\x9C\x93"));

	std::stringstream stream (result);
	boost::property_tree::ptree parsed;
	boost::property_tree::read_json (stream, parsed);
	ASSERT_EQ ("Gems \xE2\x9C\x93", parsed.get<std::string> ("name"));
}

TEST (json_response, empty_response_is_an_object)
{
	ASSERT_EQ ("{}", encoded (boost::property_tree::ptree ()));
}

// Everything above has to survive a round trip, or the SDK cannot read it.
TEST (json_response, reparses)
{
	boost::property_tree::ptree account;
	account.put ("address", "kei_3abc");
	nano::json::put_number (account, "height", 12);
	account.put ("balance", "5000000000000000000");
	nano::json::put_number (account, "receivableCount", 0);
	boost::property_tree::ptree tree;
	tree.add_child ("account", account);
	nano::json::add_array (tree, "history", boost::property_tree::ptree ());

	std::stringstream stream (nano::json::to_string (tree));
	boost::property_tree::ptree parsed;
	ASSERT_NO_THROW (boost::property_tree::read_json (stream, parsed));
	ASSERT_EQ (uint64_t (12), parsed.get<uint64_t> ("account.height"));
	ASSERT_EQ ("kei_3abc", parsed.get<std::string> ("account.address"));
}
