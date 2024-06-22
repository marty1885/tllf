#include "utils.hpp"

namespace tllf::utils
{
std::string replaceAll(std::string str, const std::string& from, const std::string& to)
{
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

std::string_view trim(const std::string_view str, const std::string_view whitespace)
{
    size_t first = str.find_first_not_of(whitespace);
    if(std::string::npos == first)
        return std::string_view{};
    size_t last = str.find_last_not_of(whitespace);
    return str.substr(first, (last - first + 1));
}
}