#pragma once
#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <glaze/json/generic.hpp>
#include <glaze/json/write.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>

#include <tllf/inner/utils.hpp>

#include <yaml-cpp/emittermanip.h>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/yaml.h>

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
    std::string type;
    bool is_mandatory;
};

} // namespace internal
extern thread_local bool g_local_return_doc;

template <template <typename...> class T, typename U>
struct is_specialization_of: std::false_type {};

template <template <typename...> class T, typename... Us>
struct is_specialization_of<T, T<Us...>>: std::true_type {};

template<typename T>
struct remove_optional {
    using type = T;
};

template<typename T>
struct remove_optional<std::optional<T>> {
    using type = T;
};

template<typename T>
using remove_optional_t = typename remove_optional<T>::type;

struct ToolDoc
{
    std::string brief_;
    std::vector<std::pair<std::string, internal::ParamInfo>> params;
    std::string name;

    ToolDoc& brief(std::string str) { brief_ = std::move(str); return *this; }
    template <typename T>
    ToolDoc& param(std::string name, const T* ptr, std::string desc) {
        (void)ptr;
        using Type = remove_optional_t<T>;
        std::string type;
        if constexpr(is_specialization_of<std::vector, Type>::value) {
            type = "array";
        }
        else if constexpr(is_specialization_of<std::map, Type>::value) {
            type = "object";
        }
        else if constexpr(std::is_same_v<Type, bool>) {
            type = "boolean";
        }
        else if constexpr(std::is_same_v<Type, std::string>) {
            type = "string";
        }
        else if constexpr(std::is_same_v<Type, double> || std::is_same_v<Type, int> || std::is_same_v<Type, float>) {
            type = "number";
        }
        else {
            throw std::invalid_argument("Unsupported parameter type");
        }
        params.push_back({name, internal::ParamInfo{desc, type, is_specialization_of<std::optional, T>::value}});
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
    std::function<drogon::Task<std::string>(const std::string&)> func;
    ToolDoc doc;

    template <typename ... Args>
    drogon::Task<std::string> operator()(Args&&... args)
    {
        return func(std::forward<Args>(args)...);
    }

    // FIXME: Use static schemas
    // TODO: Migrate to use of Glaze's JSON scheme generation
    glz::generic makeOpenAIToolObject() const
    {
        glz::generic data;
        data["name"] = name;
        data["description"] = doc.brief_;
        glz::generic parameters;
        parameters["type"] = "object";
        parameters["properties"] = glz::generic{};
        for(const auto& param : doc.params) {
            glz::generic prop;
            prop["type"] = param.second.type;
            parameters["properties"][param.first] = prop;
        }
        data["parameters"] = parameters;
        std::vector<std::string> required;
        for(const auto& param : doc.params) {
            if(param.second.is_mandatory)
                required.push_back(param.first);
        }
        data["parameters"]["required"] = required;
        return data;
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
    param_names.resize(doc.params.size());
    for(size_t i=0;i<doc.params.size();i++) {
        param_names[i] = doc.params[i].first;
    }

    auto functor = [func = std::move(func), param_names = std::move(param_names)](const std::string& invoke_data) -> drogon::Task<std::string> {
        using FuncType = std::remove_cvref_t<Func>;
        using Traits = tllf::internal::FunctionTrait<FuncType>;
        using InvokeTuple = Traits::ArgTuple;
        constexpr size_t num_args = Traits::ArgCount;

        glz::generic json_data;
        auto ec = glz::read_json(json_data, invoke_data);
        if(ec)
            throw std::runtime_error("Failed to parse JSON during tool invocation");

        InvokeTuple tup;
        size_t idx = 0;
        auto apply_func = [&](auto& val) {
            using Type = std::remove_cvref_t<decltype(val)>;

            // Not the fastest way
            const std::string& name = param_names[idx++];
            if(!json_data.contains(name)) {
                if(is_specialization_of<std::optional, Type>::value)
                    return;
                else
                    throw std::runtime_error("Missing required parameter for tool: " + name);
            }
            auto str = glz::write_json(json_data[name]).value();
            auto ec = glz::read<glz::opts{.error_on_missing_keys=true}>(val, str);
            if(ec)
                throw std::runtime_error("Failed to parse JSON parameters during tool invocation");
        };
        std::apply([&](auto&... args) { (apply_func(args), ...); }, tup);
        auto res = co_await std::apply(func, tup);
        if(std::holds_alternative<ToolDoc>(res))
            throw std::runtime_error("Function returned a ToolDoc. WTF?");
        co_return std::get<std::string>(res);
    };

    co_return Tool{.name = doc.name,.func = std::move(functor), .doc = std::move(doc)};
}

} // namespace tllf
