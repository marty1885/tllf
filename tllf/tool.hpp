#pragma once

#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <stdexcept>
#include <string>
#include <glaze/glaze.hpp>
#include <tuple>
#include <type_traits>
#include <variant>

#include <yaml-cpp/node/node.h>
#include <yaml-cpp/yaml.h>

#define TLLF_DOC(name) if(::tllf::g_local_return_doc) co_return ::tllf::ToolDoc::make(name)
#define BRIEF(x) brief(x)
#define PARAM(x, desc) param(#x, &x, desc)

namespace tllf
{
namespace internal
{

template<typename ... Ts>
struct ExtractPack
{
    template <size_t N>
    using Type = std::tuple_element_t<N, std::tuple<Ts...>>;
};

template<typename T>
struct FunctionTrait {
    constexpr static bool ok = false;
};

template <typename R, typename... Args>
struct FunctionTrait<R(Args...)>
{
    constexpr static bool ok = true;
    using ReturnType = R;
    constexpr static size_t ArgCount = sizeof...(Args);
    template <size_t N>
    using ArgType = typename ExtractPack<Args...>::template Type<N>;
    using ArgTuple = std::tuple<Args...>;
};

struct ParamInfo
{
    std::string desc;
};

template <typename T>
void extract_from_json(T& val, const std::vector<std::string> names, const glz::json_t& json, size_t& idx)
{
    using Type = std::remove_cvref_t<T>;
    if constexpr(std::is_integral_v<T> || std::is_floating_point_v<T>)
        val = json[names[idx]].template get<double>();
    else if constexpr(std::is_same_v<Type, std::string>)
        val = json[names[idx]].template get<std::string>();
    else if constexpr(std::is_same_v<Type, bool>)
        val = json[names[idx]].template get<bool>();
    else if constexpr(std::is_same_v<Type, glz::json_t>)
        val = json[names[idx]];
    else {
        // Yeah.. not the brightest idea to use glz::write_json and glz::read_json
        std::string str = glz::write_json(json[names[idx]]);
        auto err = glz::read_json(val, str);
        if(err)
            throw std::runtime_error("Failed to read json: " + err.message());
    }
    idx++;
}

} // namespace internal
extern thread_local bool g_local_return_doc;

struct ToolDoc
{
    std::string brief_;
    std::vector<std::pair<std::string, internal::ParamInfo>> params;
    std::string name;

    ToolDoc& brief(std::string str) { brief_ = std::move(str); return *this; }
    ToolDoc& param(std::string name, void* ptr, std::string desc) {(void)ptr; params.push_back({name, internal::ParamInfo{desc}}); return *this;}
    
    static ToolDoc make(std::string name) { ToolDoc doc; doc.name = std::move(name); return doc;}

protected:
    // This is a protected constructor to prevent accidental construction of this class
    ToolDoc() = default;
};

using ToolResult = drogon::Task<std::variant<std::string, ToolDoc>>;

template <typename Func>
drogon::Task<ToolDoc> getToolDoc(Func&& func)
{
    using FuncType = std::remove_reference_t<Func>;
    using Trait = typename internal::FunctionTrait<FuncType>;
    static_assert(std::is_same_v<typename Trait::ReturnType, ToolResult>, "Function must return a ToolResult");
    static_assert(Trait::ok, "Failed to extract function traits");

    g_local_return_doc = true;
    auto res = co_await [&] () -> ToolResult {
        if constexpr(Trait::ArgCount != 0) {
            typename Trait::ArgTuple args;
            return std::apply([&func](auto&&... args) { return std::forward<Func>(func)(std::forward<decltype(args)>(args)...); }, args);
        }
        else
            return std::forward<Func>(func)();
    }();
    g_local_return_doc = false;
    if(!std::holds_alternative<ToolDoc>(res))
        throw std::runtime_error("Function did not return a ToolDoc. Did you forget to use TLLF_DOC?");
    co_return std::get<ToolDoc>(res);
}

struct Tool
{
    std::function<drogon::Task<std::string>(glz::json_t)> func;
    ToolDoc doc;

    drogon::Task<std::string> operator()(glz::json_t json) { return func(std::move(json)); }
};

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

struct Toolset : public std::vector<Tool>
{
    std::string generateToolList()
    {
        std::string res;
        for(auto& tool : *this)
            res += "- " + tool.doc.name + ": " + tool.doc.brief_ + "\n";
        return res;
    }

    std::string generateToolDescription()
    {
        std::string res;
        for(auto& tool : *this) {
            res += "- " + tool.doc.name + ": " + tool.doc.brief_ + "\n";
            for(auto& param : tool.doc.params)
                res += "  - " + param.first + ": " + param.second.desc + "\n";
        }
        return res;
    }
};

template <typename Func>
drogon::Task<Tool> toolize(Func&& func)
{
    using FuncType = std::remove_cvref_t<Func>;
    auto doc = co_await getToolDoc(func);

    using Traits = tllf::internal::FunctionTrait<FuncType>;
    static_assert(Traits::ok, "Fail to match function to traits");

    if(Traits::ArgCount != doc.params.size())
        throw std::runtime_error("Argument size does not match");

    std::vector<std::string> param_names;
    param_names.resize(Traits::ArgCount);
    for(size_t i=0;i<Traits::ArgCount;i++) {
        param_names[i] = doc.params[i].first;
    }

    auto functor = [func = std::move(func), param_names = std::move(param_names)](glz::json_t invoke_data) -> drogon::Task<std::string> {
        using FuncType = std::remove_cvref_t<Func>;
        using Traits = tllf::internal::FunctionTrait<FuncType>;
        using InvokeTuple = Traits::ArgTuple;
        constexpr size_t num_args = Traits::ArgCount;
        InvokeTuple tup;

        size_t idx = 0;
        auto apply_func = [&](auto& val) {
            internal::extract_from_json(val, param_names, invoke_data, idx);
        };
        std::apply([&](auto&... args) { (apply_func(args), ...); }, tup);
        auto res = co_await std::apply(func, tup);
        if(std::holds_alternative<ToolDoc>(res))
            throw std::runtime_error("Function returned a ToolDoc. WTF?");
        co_return std::get<std::string>(res);
    };

    co_return Tool{.func = std::move(functor), .doc = std::move(doc)};
}

} // namespace tllf