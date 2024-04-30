#pragma once

#include <drogon/CacheMap.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpTypes.h>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_set>

namespace tllf
{

namespace internal
{
drogon::HttpClientPtr getClient(const std::string& hoststr, trantor::EventLoop* loop = nullptr);
std::string env(const std::string& key);
}

namespace utils
{
std::string replaceAll(std::string str, const std::string& from, const std::string& to);
std::string_view trim(const std::string_view str, const std::string_view whitespace = " \t\n");
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

struct ChatEntry
{
    std::string content;
    std::string role;
};

struct LLM
{
    virtual drogon::Task<std::string> generate(std::vector<ChatEntry> history, TextGenerationConfig config, const nlohmann::json& function = nlohmann::json{}) = 0;
};

struct OpenAIConnector : public LLM
{
    OpenAIConnector(const std::string& model_name, const std::string& hoststr="https://api.openai.com", const std::string& api_key="")
        : client(internal::getClient(hoststr)), model_name(model_name), api_key(api_key)
    {
    }

    drogon::Task<std::string> generate(std::vector<ChatEntry> history, TextGenerationConfig config, const nlohmann::json& function = nlohmann::json{}) override;

    drogon::HttpClientPtr client;
    std::string model_name;
    std::string api_key;
};

struct LanguageParser
{
    virtual nlohmann::json parseReply(const std::string& reply) = 0;
};

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
 * {
 *     "interest": ["music", "sports"],
 *     "-": "Other interests are not important."
 * }
 *
 * This is good enough for most simple use cases.
 * TODO: Make a formal specification for this format.
*/
struct MarkdownLikeParser : public LanguageParser
{
    MarkdownLikeParser() = default;
    MarkdownLikeParser(const std::set<std::string>& altname_for_plaintext) : altname_for_plaintext(altname_for_plaintext) {}

    nlohmann::json parseReply(const std::string& reply);
    std::set<std::string> altname_for_plaintext;
};

struct JsonParser : public LanguageParser
{
    nlohmann::json parseReply(const std::string& reply) override;
};

struct PlaintextParser : public LanguageParser
{
    nlohmann::json parseReply(const std::string& reply) override;
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

    std::string prompt;
    std::unordered_map<std::string, std::string> variables;

    std::string render() const;

    static std::unordered_set<std::string> extractVars(const std::string& prompt);
};

struct Chain
{
    std::vector<tllf::ChatEntry> chatlog;
    std::shared_ptr<tllf::LLM> llm;
    std::shared_ptr<tllf::LanguageParser> parser;
    std::shared_ptr<PromptTemplate> prompt;

    Chain(std::shared_ptr<tllf::LLM> llm, std::shared_ptr<tllf::LanguageParser> parser, std::shared_ptr<PromptTemplate> prompt)
        : llm(llm), parser(parser), prompt(prompt)
    {
        chatlog.push_back({prompt->render(), "system"});
    }

    drogon::Task<std::string> generate(std::string user_input, TextGenerationConfig config)
    {
        chatlog.push_back({user_input, "user"});
        auto result = co_await llm->generate(chatlog, config);
        chatlog.push_back({result, "bot"});
        co_return result;
    }
};

}