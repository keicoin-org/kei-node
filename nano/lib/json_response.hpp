#pragma once

#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <ostream>
#include <string>

/*
 * The RPC response encoder.
 *
 * boost::property_tree stores every value as a string, so its JSON writer has
 * nothing to go on and quotes all of them: `decimals` leaves as "0" and an
 * absent record as "". kei-transaction/docs/rpc.md asks for 0 and null, and
 * HttpNode passes the parsed body straight through without coercing, so the
 * type has to survive as far as the writer.
 *
 * It rides on the key, not the value. Every key in a response is a literal in
 * this source. Values are not: an asset's name and a transfer's memo are chosen
 * by whoever signed the block, and are length-capped but not otherwise
 * constrained, so a marker carried in a value would let an issuer name a token
 * such that the node emitted it as raw JSON.
 *
 * A tree with no marked keys encodes as boost would have encoded it, so every
 * inherited endpoint's output is unchanged — with one deliberate exception,
 * described on `write`.
 */
namespace nano::json
{
/** A JSON number. `put_number (tree, "height", 12)` emits `"height": 12`. */
void put_number (boost::property_tree::ptree &, std::string const & key, uint64_t value);
void put_boolean (boost::property_tree::ptree &, std::string const & key, bool value);
/** JSON `null` — an absent record, which docs/rpc.md distinguishes from an error. */
void put_null (boost::property_tree::ptree &, std::string const & key);
/** A JSON array, which stays `[]` when empty instead of collapsing to `""`. */
void add_array (boost::property_tree::ptree &, std::string const & key, boost::property_tree::ptree const & value);

/**
 * Encodes as boost's pretty writer does — four-space indent, one member per
 * line, trailing newline — so inherited responses are unchanged apart from
 * bytes at or above 0x80, which are passed through rather than escaped one at a
 * time. Boost's escaper mangles those into a run of \u00XX that no longer
 * decodes as the character it came from, and an asset's name and a memo are
 * user text that has to survive the trip.
 */
void write (std::ostream &, boost::property_tree::ptree const &);
std::string to_string (boost::property_tree::ptree const &);
}
