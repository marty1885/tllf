#include <nlohmann/json.hpp>
#include <optional>
#include <yaml-cpp/yaml.h>

namespace tllf::internal
{
inline void json2yaml_internal(YAML::Node& node, const nlohmann::json& json)
{
    if(json.is_null())
        node = YAML::Node();
    else if(json.is_boolean())
        node = json.get<bool>();
    else if(json.is_number_integer())
        node = json.get<int>();
    else if(json.is_number_float())
        node = json.get<double>();
    else if(json.is_string())
        node = json.get<std::string>();
    else if(json.is_array()) {
        node = YAML::Node(YAML::NodeType::Sequence);
        for(auto& child : json)
        {
            YAML::Node child_node;
            json2yaml_internal(child_node, child);
            node.push_back(child_node);
        }
    }
    else if(json.is_object()) {
        node = YAML::Node(YAML::NodeType::Map);
        for(auto& [key, val] : json.items())
        {
            YAML::Node child_node;
            json2yaml_internal(child_node, val);
            node[key] = child_node;
        }
    }
    else {
        throw std::runtime_error("Unknown json type");
    }
}

inline YAML::Node json2yaml(const nlohmann::json& json)
{
    YAML::Node node;
    json2yaml_internal(node, json);
    return node;
}

static std::optional<double> try_stod(const std::string& str)
{
    try {
        size_t pos;
        double val = std::stod(str, &pos);
        if(pos == str.size())
            return val;
    }
    catch(...) {}
    return std::nullopt;
}

static std::optional<int> try_stoi(const std::string& str)
{
    try {
        size_t pos;
        int val = std::stoi(str, &pos);
        if(pos == str.size())
            return val;
    }
    catch(...) {}
    return std::nullopt;
}

static std::optional<bool> try_stob(const std::string& str)
{
    if(str == "true")
        return true;
    if(str == "false")
        return false;
    return std::nullopt;
}

inline void yaml2json_internal(nlohmann::json& json, const YAML::Node& node)
{
    if(node.IsScalar()) {
        if(auto val = try_stob(node.as<std::string>()))
            json = *val;
        else if(auto val = try_stod(node.as<std::string>()))
            json = *val;
        else if(auto val = try_stoi(node.as<std::string>()))
            json = *val;
        else
            json = node.as<std::string>();
    }
    else if(node.IsSequence()) {
        json = nlohmann::json::array();
        for(auto& child : node)
        {
            nlohmann::json child_json;
            yaml2json_internal(child_json, child);
            json.push_back(child_json);
        }
    }
    else if(node.IsMap()) {
        json = nlohmann::json::object();
        for(auto& n : node)
        {
            std::string key = n.first.as<std::string>();
            nlohmann::json child_json;
            yaml2json_internal(child_json, n.second);
            json[key] = child_json;
        }
    }
    else {
        throw std::runtime_error("Unknown yaml type");
    }
}

inline nlohmann::json yaml2json(const YAML::Node& node)
{
    nlohmann::json json;
    yaml2json_internal(json, node);
    return json;
}
}