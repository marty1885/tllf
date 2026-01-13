#pragma once

#include <cstddef>
#include <cstdint>
#include <drogon/CacheMap.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpTypes.h>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string>
#include <sys/types.h>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <variant>
#include <vector>
#include <unordered_set>
#include <yaml-cpp/node/node.h>

#include <tllf/utils.hpp>
#include <tllf/tool.hpp>

namespace tllf
{

namespace internal
{
drogon::HttpClientPtr getClient(const std::string& hoststr, trantor::EventLoop* loop = nullptr);
std::string env(const std::string& key);
}

struct TextGenerationConfig
{
    std::string prompt;
    std::optional<int> max_tokens;
    std::optional<int> temperature;
    std::optional<int> top_p;
    std::optional<int> frequency_penalty;
    std::optional<int> presence_penalty;
    std::optional<int> stop_sequence;
};

struct ImageUrl
{
    std::string url;
};

struct ImageByUrl
{
    ImageUrl image_url;
};

struct OpenAIToolDesc
{
    std::optional<std::string> type = "function";
    glz::generic function;
};

glz::generic modelBuiltinTool(const std::string& name);

struct ChatEntry
{
    struct ToolCall
    {
        std::string id;
        std::string type;

        struct FunctionInvocation {
            std::string name;
            std::string arguments;
        };
        FunctionInvocation function;
        glz::generic extra_content; // Makes Google happy
    };

    using Part = std::variant<std::string, ImageByUrl>;
    using Parts = std::vector<Part>;
    using Content = std::variant<std::string, Parts>;
    Content content;
    std::string role;
    std::vector<ToolCall> tool_calls;
    std::optional<std::string> tool_call_id;
};

struct OpenAIResponse
{
    struct Choice
    {
        ChatEntry message;
        std::string finish_reason;
        size_t index;
    };
    std::vector<Choice> choices;
};


struct Chatlog : public std::vector<ChatEntry>
{
    Chatlog() = default;
    Chatlog(size_t n) : std::vector<ChatEntry>(n){}
    Chatlog(std::initializer_list<ChatEntry> list)
        : std::vector<ChatEntry>(list)
    {}
    Chatlog operator+ (const Chatlog& rhs)
    {
        Chatlog res = *this;
        res.reserve(size() + rhs.size());
        for(auto& ent : rhs)
            res.push_back(ent);
        return res;
    }

    void push_back(ChatEntry entry)
    {
        std::vector<ChatEntry>::push_back(entry);
    }

    void push_back(std::string content, std::string role)
    {
        push_back(ChatEntry{.content = content, .role = role});
    }

    void push_back(ChatEntry::Parts parts, std::string role)
    {
        push_back(ChatEntry{.content = parts, .role = role});
    }

};

std::string to_string(const Chatlog& chatlog);

struct LLM
{
    struct RateLimitError : public std::exception
    {
        RateLimitError(std::optional<double> until_reset_ms) : until_reset_ms(until_reset_ms) {}
        std::optional<double> until_reset_ms;
    };

    /**
     * Generate a response based on the given chat history.
     * @param history The chat history to generate a response from.
     * @param config The configuration for the generation.
     * @return The generated response.
     * @note This function is a proxy for the real implementation. Which has built-in capablity to handle retry.
     * TODO: Handle rate limiting
    */
    drogon::Task<std::string> generate(Chatlog& history, TextGenerationConfig config = TextGenerationConfig(), const std::vector<Tool>& tools = {});
protected:
    virtual drogon::Task<std::string> generateImpl(Chatlog& history, TextGenerationConfig config, const std::vector<Tool>& tools = {}) = 0;
};

struct TextEmbedder
{
    virtual drogon::Task<std::vector<float>> embed(std::string text) = 0;
    virtual drogon::Task<std::vector<std::vector<float>>> embed(std::vector<std::string> texts)
    {
        std::vector<std::vector<float>> res;
        for(auto& text : texts)
            res.push_back(co_await embed(text));
        co_return res;
    }
};

struct DeepinfraTextEmbedder : public TextEmbedder
{
    DeepinfraTextEmbedder(const std::string& model_name, const std::string& hoststr="https://api.deepinfra.com", const std::string& api_key="")
        : client(internal::getClient(hoststr)), model_name(model_name), api_key(api_key)
    {
    }

    drogon::Task<std::vector<float>> embed(std::string text) override;
    drogon::Task<std::vector<std::vector<float>>> embed(std::vector<std::string> texts) override;

    drogon::HttpClientPtr client;
    std::string model_name;
    std::string api_key;
};

/**
 * Connector for OpenAI-like API endpoints.
 *
 * @note This connector is also the one used for most other services like DeepInfra and Perplexity
 * as they all use the same OpenAI API schema
 * @param model_name The name of the model to use. For example, "text-davinci-003".
 * @param hoststr The host string. Defaults to "https://api.openai.com/".
 * @param api_key The API key to use
*/
struct OpenAIConnector : public LLM
{
    OpenAIConnector(const std::string& model_name, const std::string& baseurl="https://api.openai.com/", const std::string& api_key="", std::vector<glz::generic> builtin_tools = {});

    drogon::Task<std::string> generateImpl(Chatlog& history, TextGenerationConfig config, const std::vector<Tool>& tools = {});

    drogon::HttpClientPtr client;
    std::string base;
    std::string model_name;
    std::string api_key;
    std::vector<glz::generic> builtin_tools;
};

struct PromptTemplate
{
    PromptTemplate() = default;
    PromptTemplate(const std::string& prompt, const std::unordered_map<std::string, std::string>& variables = {})
        : prompt(prompt), variables(variables)
    {
    }

    void setVariable(const std::string& name, const std::string& value)
    {
        variables[name] = value;
    }

    void setVariable(const std::string& name, intmax_t value)
    {
        variables[name] = std::to_string(value);
    }

    void setVariable(const std::string& name, double value)
    {
        variables[name] = std::to_string(value);
    }

    std::string prompt;
    std::unordered_map<std::string, std::string> variables;

    std::string render() const;

    static std::unordered_set<std::string> extractVars(const std::string& prompt);
};

std::string dataUrlfromFile(const std::string& path, std::string mime="");
}
