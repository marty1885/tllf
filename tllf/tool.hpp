#pragma once
#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>

#include <tllf/inner/utils.hpp>

#include <yaml-cpp/emittermanip.h>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/yaml.h>

#include <nlohmann/json.hpp>

#include <tllf/tllf.hpp>

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
struct FunctionTrait;

template <typename R, typename... Args>
struct FunctionTrait<R(Args...)>
{
    constexpr static bool ok = true;
    using ReturnType = R;
    constexpr static size_t ArgCount = sizeof...(Args);
    template <size_t N>
    using ArgType = typename ExtractPack<Args...>::template Type<N>;
    using ArgTuple = std::tuple<std::remove_cvref_t<Args>...>; // TODOL: Remove cvref so large objects are not copied
    using Signature = R(Args...);
};

template <typename R, typename... Args>
struct FunctionTrait<R(*)(Args...)> : public FunctionTrait<R(Args...)>
{
};

template <typename Class, typename R, typename... Args>
struct FunctionTrait<R (Class::*)(Args...)> : public FunctionTrait<R(Args...)>
{
};

template <typename Class, typename R, typename... Args>
struct FunctionTrait<R (Class::*)(Args...) const> : public FunctionTrait<R(Args...)>
{
};

template <typename Function>
struct FunctionTrait : public FunctionTrait<decltype(&Function::operator())>
{
};

struct ParamInfo
{
    std::string desc;
    std::function<bool(const std::string&)> validator;
};

template <typename T>
void extract_from_json(T& val, const std::vector<std::string> names, const nlohmann::json& json, size_t idx)
{
    if(!json.contains(names[idx]))
        throw std::runtime_error("Missing parameter " + names[idx] + " in JSON object");
    using Type = std::remove_cvref_t<T>;
    if constexpr(std::is_integral_v<T> || std::is_floating_point_v<T>)
        val = json[names[idx]].template get<double>();
    else if constexpr(std::is_same_v<Type, float>)
        val = json[names[idx]].template get<float>();
    else if constexpr(std::is_same_v<Type, std::string>)
        val = json[names[idx]].template get<std::string>();
    else if constexpr(std::is_same_v<Type, bool>)
        val = json[names[idx]].template get<bool>();
    else if constexpr(std::is_same_v<Type, int>)
        val = json[names[idx]].template get<int>();
    else if constexpr(std::is_same_v<Type, size_t>)
        val = json[names[idx]].template get<size_t>();
    else if constexpr(std::is_same_v<Type, nlohmann::json>)
        val = json[names[idx]];
    else if constexpr(std::is_same_v<Type, nlohmann::json>)
        val = json[names[idx]];
    else {
        // TODO: Handle this
    }
}

} // namespace internal
extern thread_local bool g_local_return_doc;

template <template <typename...> class T, typename U>
struct is_specialization_of: std::false_type {};

template <template <typename...> class T, typename... Us>
struct is_specialization_of<T, T<Us...>>: std::true_type {};

struct ToolDoc
{
    std::string brief_;
    std::vector<std::pair<std::string, internal::ParamInfo>> params;
    std::string name;

    ToolDoc& brief(std::string str) { brief_ = std::move(str); return *this; }
    template <typename T>
    ToolDoc& param(std::string name, const T* ptr, std::string desc) {
        (void)ptr;
        using Type = std::remove_cvref_t<T>;
        auto validator = [](const std::string& str) -> bool {
            try {
                if constexpr(is_specialization_of<std::optional, Type>::value) {
                    using InnerType = std::remove_cvref_t<typename Type::value_type>;
                    std::string lower = str;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if(lower == "null" || lower == "nil" || lower == "none" || lower == "" || lower == "n/a")
                        return true;
                    nlohmann::json json = nlohmann::json::parse(str);
                    json.get<InnerType>();
                }
                else {
                    nlohmann::json json = nlohmann::json::parse(str);
                    json.get<Type>();
                }
            }
            catch(...) {
                return false;
            }
            return true;
        };
        params.push_back({name, internal::ParamInfo{desc, validator}});
        return *this;
    }
    
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
    std::string name;
    std::function<drogon::Task<std::string>(const tllf::Chatlog&, nlohmann::json)> func;
    ToolDoc doc;

    template <typename ... Args>
    nlohmann::json make_json(Args&&... args)
    {
        constexpr size_t num_args = sizeof...(Args);
        if(num_args != doc.params.size() + 1)
            throw std::runtime_error("Argument size does not match tool params");

        nlohmann::json json;
        size_t idx = 0;

        auto apply_func = [&](auto& val) {
            std::string str = nlohmann::json(val).dump();
            if(!doc.params[idx].second.validator(str))
                // TODO: This error message is confusing. We should provide a better error message
                throw std::runtime_error("Parameter " + doc.params[idx].first + " does not seem to be serializable from example");
            json[doc.params[idx].first] = val;
            idx++;
        };

        std::apply([&](auto&... args) { (apply_func(args), ...); }, std::forward_as_tuple(args...));
        return json;
    }

    template <typename ... Args>
    drogon::Task<std::string> operator()(const tllf::Chatlog& log, Args&&... args)
    {
        constexpr size_t num_args = sizeof...(Args);
        if constexpr(num_args == 1 && std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, std::tuple<Args...>>>, nlohmann::json>) {
            static_assert(num_args == 1, "Only one argument is allowed when passing a JSON object");
            return func(log, std::forward<nlohmann::json>(args)...);
        }
        else
            return func(log, make_json(std::forward<Args>(args)...));
    }

    template <typename ... Args>
    std::string generateInvokeExample(Args&&... args)
    {
        auto json = make_json(std::forward<Args>(args)...);
        YAML::Node yaml;
        yaml[name] = internal::json2yaml(json);
        YAML::Emitter emitter;
        emitter << yaml;

        std::stringstream ss;
        // HACK: add a - to the beginning of each line to turn it into a markdown list
        ss << emitter.c_str();
        std::string line;
        std::string result;
        while(std::getline(ss, line)) {
            auto pos = line.find_first_not_of(' ');
            std::string new_line;
            if(pos != std::string::npos) {
                for(size_t i=0;i<pos;i++)
                    new_line += ' ';
                new_line += "- " + line.substr(pos);
            }
            else
                new_line = "- " + line;
            result += new_line + "\n";
        }
        if(result.size() != 0 && result.back() == '\n')
            result.pop_back();
        return result;
    }
};

struct Toolset : public std::vector<Tool>
{
    Toolset(std::initializer_list<Tool> tools) : std::vector<Tool>(tools) {}
    Toolset() = default;

    std::string generateToolList() const
    {
        std::string str;
        for(auto& tool : *this) {
            str += "- " + tool.name + "\n";
        }
        if(str.size() != 0 && str.back() == '\n')
            str.pop_back(); 
        return str;
    }

    std::string generateToolListWithBrief() const
    {
        std::string str;
        for(auto& tool : *this) {
            str += "- " + tool.name + ": " + tool.doc.brief_ + "\n";
        }
        if(str.size() != 0 && str.back() == '\n')
            str.pop_back();
        return str;
    }

    std::string generateToolDescription() const
    {
        // Can't use YAML here because we want syntax closer to Markdown
        std::string str;
        for(auto& tool : *this) {
            str += "- " + tool.name + ": " + tool.doc.brief_ + "\n";
            for(auto& param : tool.doc.params) {
                str += "  - " + param.first + ": <" + param.second.desc + ">\n";
            }
        }
        if(str.size() != 0 && str.back() == '\n')
            str.pop_back();
        return str;
    }

    bool contains(const std::string& name) const
    {
        return std::any_of(begin(), end(), [&](const Tool& tool) { return tool.name == name; });
    }

    Tool& operator[](const std::string& name)
    {
        auto it = std::find_if(begin(), end(), [&](const Tool& tool) { return tool.name == name; });
        if(it == end())
            throw std::runtime_error("Tool " + name + " not found");
        return *it;
    }

    const Tool& operator[](const std::string& name) const
    {
        return const_cast<const Tool&>(static_cast<const Toolset&>(*this)[name]);
    }
};

template <typename Func>
drogon::Task<Tool> toolize(Func&& func)
{
    using FuncType = std::remove_cvref_t<Func>;
    auto doc = co_await getToolDoc(func);

    using Traits = tllf::internal::FunctionTrait<FuncType>;
    static_assert(Traits::ok, "Fail to match function to traits");

    if(Traits::ArgCount != doc.params.size() + 1)
        throw std::runtime_error("Argument size does not match");

    std::vector<std::string> param_names;
    param_names.resize(doc.params.size());
    for(size_t i=0;i<doc.params.size();i++) {
        param_names[i] = doc.params[i].first;
    }

    auto functor = [func = std::move(func), param_names = std::move(param_names)](const tllf::Chatlog& log, nlohmann::json invoke_data) -> drogon::Task<std::string> {
        using FuncType = std::remove_cvref_t<Func>;
        using Traits = tllf::internal::FunctionTrait<FuncType>;
        using InvokeTuple = Traits::ArgTuple;
        constexpr size_t num_args = Traits::ArgCount;
        if(invoke_data.is_object() == false)
            throw std::runtime_error("Invoke data must be an ordered, JSON object of the parameters");

        InvokeTuple tup;
        size_t idx = 0;
        auto apply_func = [&](auto& val) {
            // HACK: 1st argument is log
            if(idx == 0)
                std::get<0>(tup) = log;
            else
                internal::extract_from_json(val, param_names, invoke_data, idx-1);
            idx++;
            
        };
        std::apply([&](auto&... args) { (apply_func(args), ...); }, tup);
        if(idx != num_args)
            throw std::runtime_error("Expecting " + std::to_string(num_args) + " arguments, but got " + std::to_string(idx) + ", including the chatlog");
        auto res = co_await std::apply(func, tup);
        if(std::holds_alternative<ToolDoc>(res))
            throw std::runtime_error("Function returned a ToolDoc. WTF?");
        co_return std::get<std::string>(res);
    };

    co_return Tool{.name = doc.name,.func = std::move(functor), .doc = std::move(doc)};
}

} // namespace tllf