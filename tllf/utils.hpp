#pragma once
#include <string>

namespace tllf::utils
{
std::string replaceAll(std::string str, const std::string& from, const std::string& to);
std::string_view trim(const std::string_view str, const std::string_view whitespace = " \t\n");
}