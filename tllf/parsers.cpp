#include "parsers.hpp"
#include "utils.hpp"
#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <variant>

using namespace tllf;

std::map<std::string, MarkdownLikeParser::MarkdownLikeData> MarkdownLikeParser::parseReply(const std::string& reply)
{
    std::map<std::string, MarkdownLikeData> parsed;
    std::string_view remaining(reply);
    size_t last_remaining_size = -1;

    auto peek_next_line = [&](){
        size_t next_line_end = remaining.find('\n');
        if(next_line_end == std::string::npos)
            return std::string(remaining);
        return std::string(remaining.substr(0, next_line_end));
    };

    auto consume_line = [&](){
        size_t next_line_end = remaining.find('\n');
        if(next_line_end == std::string::npos) {
            remaining = std::string_view();
            return;
        }
        remaining = remaining.substr(next_line_end + 1);
    };

    while(!remaining.empty()) {
        if(remaining.size() == last_remaining_size)
            throw std::runtime_error("Parser stuck in infinite loop. THIS IS A BUG.");
        last_remaining_size = remaining.size();

        std::string line = peek_next_line();
        consume_line();
        std::string trimmed = std::string(utils::trim(line));
        if(trimmed.empty()) {
            continue;
        }

        // This is a list item
        if(trimmed.ends_with(":")) {
            std::string key = trimmed.substr(0, trimmed.size() - 1);
            key = utils::trim(key, " *_");
            std::string line;
            while(remaining.empty() == false) {
                line = utils::trim(peek_next_line());
                if(!line.empty()) {
                    break;
                }
                consume_line();
            }
            
            ListNode root_node;
            std::vector<ListNode*> list_stack = {&root_node};
            while(remaining.empty() == false) {
                line = peek_next_line();
                auto trimmed = utils::trim(line);
                if(trimmed.empty()) {
                    consume_line();
                    continue;
                }
                if(!trimmed.starts_with("- "))
                    break;

                // Assume 2 spaces per indent
                size_t leading_spaces = line.find_first_not_of(" ");
                size_t indent_level = leading_spaces / 2;
                assert(list_stack.size() != 0);
                size_t last_indent_level = list_stack.size() - 1;
                if(indent_level > last_indent_level+1)
                    throw std::runtime_error("Invalid list indentation");

                if(indent_level == last_indent_level) {
                    // no op
                }
                else if(indent_level < last_indent_level) {
                    list_stack.resize(indent_level + 1);
                }
                else {
                    // assert(indent_level == last_indent_level + 1);
                    auto current_back = list_stack.back();
                    current_back->children.push_back({});
                    list_stack.push_back(&current_back->children.back());
                }
                assert(list_stack.size() != 0);
                auto current = list_stack.back();

                std::string value = std::string(trimmed.substr(2));
                current->children.push_back(ListNode{.value = value});
                consume_line();
                list_stack.push_back(&current->children.back());

            }

            std::transform(key.begin(), key.end(), key.begin(), ::tolower); 
            if(altname_for_plaintext.contains(key))
                key = "-";
            parsed[key] = root_node.children;
        }
        // HACK: There is an ambiguity here. Usually we treat lines with a colon as key-value pairs. ex:
        // name: Tom
        // however, there are other cases where a colon is used in a sentence. ex:
        // The book "The Lord of the Rings: The Fellowship of the Ring" is a good book.
        // We can't reliably distinguish between these two cases. So a heuristic is used.
        else if(auto sep_pos = trimmed.find(": "); sep_pos != std::string::npos
            && trimmed.substr(0, sep_pos).find_first_of("\"'") == std::string::npos
            && sep_pos < 48) {
            size_t colon_pos = trimmed.find(": ");
            std::string key = trimmed.substr(0, colon_pos);
            std::string value = trimmed.substr(colon_pos + 2);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            parsed[key] = value;
        }
        else {
            if(!parsed.contains("-"))
                parsed["-"] = trimmed;
            else
                parsed["-"] = std::get<std::string>(parsed["-"]) + "\n" + trimmed;
        }
    }
    
    return parsed;
}

static void to_json_internal(const MarkdownLikeParser::ListNode& node, nlohmann::json& json)
{
    auto& value = node.value;
    if(node.children.size() != 0) {
        nlohmann::json child_json;
        for(auto& child : node.children) {
            to_json_internal(child, child_json);
        }
        std::string key = value;
        if(key.ends_with(":"))
            key = key.substr(0, key.size() - 1);
        json[key] = child_json;
        return;
    }

    size_t pos = value.find(": ");
    if(pos == std::string::npos)
        throw std::runtime_error("Invalid node. No value");

    std::string key = value.substr(0, pos);
    std::string val = value.substr(pos + 2);

    try {
        size_t v = std::stod(val, &pos);
        if(pos == val.size())
            json[key] = v;
    }
    catch(...) {
        json[key] = val;
    }
}

void tllf::to_json(nlohmann::json& json, const MarkdownLikeParser::ListNode& node)
{
    to_json_internal(node, json);
}

void tllf::to_json(nlohmann::json& json, const MarkdownLikeParser::MarkdownLikeData& data)
{
    if(std::holds_alternative<std::string>(data)) {
        json = std::get<std::string>(data);
    }
    else {
        auto& nodes = std::get<std::vector<MarkdownLikeParser::ListNode>>(data);
        json = nlohmann::json::array();
        for(auto& node : nodes) {
            nlohmann::json child_json;
            to_json(child_json, node);
            json.push_back(child_json);
        }
    }
}


std::vector<std::string> MarkdownListParser::parseReply(const std::string& reply)
{
    std::string_view remaining(reply);
    size_t last_remaining_size = -1;

    auto peek_next_line = [&](){
        size_t next_line_end = remaining.find('\n');
        if(next_line_end == std::string::npos)
            return std::string(remaining);
        return std::string(remaining.substr(0, next_line_end));
    };

    auto consume_line = [&](){
        size_t next_line_end = remaining.find('\n');
        if(next_line_end == std::string::npos) {
            remaining = std::string_view();
            return;
        }
        remaining = remaining.substr(next_line_end + 1);
    };

    std::vector<std::string> res;
    while(remaining.size() > 0) {
        if(remaining.size() == last_remaining_size)
            throw std::runtime_error("Parser stuck in infinite loop. THIS IS A BUG.");
        last_remaining_size = remaining.size();

        std::string line = peek_next_line();
        consume_line();
        std::string_view trimmed = utils::trim(line);
        if(trimmed.empty()) {
            continue;
        }

        if(trimmed.starts_with("- ") || trimmed.starts_with("* ") || trimmed.starts_with("+ ")) {
            res.push_back(std::string(trimmed.substr(2)));
        }
    }

    return res;
}

nlohmann::json JsonParser::parseReply(const std::string& reply)
{
    std::string_view remaining(reply);
    if(remaining.starts_with("```json"))
        remaining = remaining.substr(7);
    if(remaining.starts_with("```"))
        remaining = remaining.substr(3);
    if(remaining.ends_with("```"))
        remaining = remaining.substr(0, remaining.size() - 3);
    remaining = utils::trim(remaining, " \n\r\t");

    return nlohmann::json::parse(remaining);
}

std::string PlaintextParser::parseReply(const std::string& reply)
{
    return reply;
}

YAML::Node YamlParser::parseReply(const std::string& reply)
{
    return YAML::Load(reply);
}