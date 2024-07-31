#pragma once
#include <string>
#include <map>
#include <vector>
#include <variant>
#include <set>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace tllf
{
/**
 * Parses a reply in a markdown-like format. (like. meaning it is not a full markdown parser)
 * For example:
 * interest:
 * - music
 * - sports
 *
 * Other interests are not important.
 *
 * Will be parsed as:
 * parsed["-"] = "Other interests are not important."
 * parsed["interest"] = MarkDownListNodes{ListNods{"music"}, ListNods{"sports"}}
 *
 * This is good enough for most simple use cases.
*/
struct MarkdownLikeParser
{
    MarkdownLikeParser() = default;
    MarkdownLikeParser(const std::set<std::string>& altname_for_plaintext) : altname_for_plaintext(altname_for_plaintext) {}

    struct ListNode
    {
        std::string value;
        std::vector<ListNode> children;

        ListNode& operator[](size_t idx)
        {
            return children[idx];
        }

        const ListNode& operator[](size_t idx) const
        {
            return children[idx];
        }
    };

    struct MarkdownLikeData : public std::variant<std::string, std::vector<ListNode>>
    {
        using std::variant<std::string, std::vector<ListNode>>::variant;

        template <typename T>
        T& get()
        {
            return std::get<T>(*this);
        }

        template <typename T>
        const T& get() const
        {
            return std::get<T>(*this);
        }
    };

    std::map<std::string, MarkdownLikeData> parseReply(const std::string& reply);
    std::set<std::string> altname_for_plaintext;
};
using MarkDownListNodes = std::vector<MarkdownLikeParser::ListNode>;

void to_json(nlohmann::json& json, const MarkdownLikeParser::MarkdownLikeData& data);
void to_json(nlohmann::json& json, const MarkdownLikeParser::MarkdownLikeData& data);

void to_json(nlohmann::json& json, const MarkdownLikeParser::ListNode& node);
inline nlohmann::json to_json(const MarkdownLikeParser::ListNode& node)
{
    nlohmann::json json;
    to_json(json, node);
    return json;
}

struct MarkdownListParser
{
    std::vector<std::string> parseReply(const std::string& reply);
};

struct JsonParser
{
    nlohmann::json parseReply(const std::string& reply);
};

struct PlaintextParser
{
    std::string parseReply(const std::string& reply);
};

struct YamlParser
{
    YAML::Node parseReply(const std::string& reply);
};
}