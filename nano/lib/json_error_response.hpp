#pragma once

#include <nano/lib/json_response.hpp>

#include <boost/property_tree/ptree.hpp>

#include <functional>
#include <string>

namespace nano
{
inline void json_error_response (std::function<void (std::string const &)> response_a, std::string const & message_a)
{
	boost::property_tree::ptree response_l;
	response_l.put ("error", message_a);
	response_a (nano::json::to_string (response_l));
}
}
