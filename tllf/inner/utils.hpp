#include <glaze/json.hpp>
#include <optional>
#include <yaml-cpp/yaml.h>

namespace tllf::internal
{
inline void json2yaml_internal(YAML::Node& node, const glz::json_t& json)
{
    if(json.holds<glz::json_t::object_t>()) {
        for(auto& [key, val] : json.get<glz::json_t::object_t>()) {
            YAML::Node child;
            json2yaml_internal(child, val);
            node[key] = child;
        }
    }
    else if(json.holds<glz::json_t::array_t>()) {
        for(auto& val : json.get<glz::json_t::array_t>()) {
            YAML::Node child;
            json2yaml_internal(child, val);
            node.push_back(child);
        }
    }
    else if(json.holds<std::string>())
        node = json.get<std::string>();
    else if(json.holds<double>())
        node = json.get<double>();
    else if(json.holds<bool>())
        node = json.get<bool>();
    else
        throw std::runtime_error("Unknown json type");
}

inline YAML::Node json2yaml(const glz::json_t& json)
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

inline void yaml2json_internal(glz::json_t& json, const YAML::Node& node)
{
    if(node.IsNull())
        json.data = glz::json_t::null_t{};
    else if(node.IsScalar()) {
        if(auto val = node.as<std::string>(); val == "true" || val == "false")
            json.data = (val == "true");
        else if(auto val = try_stod(node.as<std::string>()))
            json.data = *val;
        else
            json.data = node.as<std::string>();
    }
    else if(node.IsSequence()) {
        json.data = glz::json_t::array_t{};
        for(auto& child : node)
        {
            glz::json_t child_json;
            yaml2json_internal(child_json, child);
            json.get<glz::json_t::array_t>().push_back(child_json);
        }
    }
    else if(node.IsMap()) {
        json.data = glz::json_t::object_t{};
        for(auto& n : node)
        {
            std::string key = n.first.as<std::string>();
            glz::json_t child_json;
            yaml2json_internal(child_json, n.second);
            json.get<glz::json_t::object_t>()[key] = child_json;
        }
    }
    else {
        throw std::runtime_error("Unknown yaml type");
    }

}

inline glz::json_t yaml2json(const YAML::Node& node)
{
    glz::json_t json;
    yaml2json_internal(json, node);
    return json;
}
}