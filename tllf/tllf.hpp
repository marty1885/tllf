#pragma once

#include "tllf/url_parser.hpp"
#include <cstddef>
#include <cstdint>
#include <drogon/CacheMap.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpTypes.h>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <glaze/glaze.hpp>
#include <variant>
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

struct ImageBlob
{
    std::vector<char> data;
    std::string mime;

    // Functions for Glaze to serialize the data structure
    // into formats accepts by OpenAI
    std::string write_data();
    void read_data(const std::string& data);
};

struct ImageUrl
{
    ImageUrl() = default;
    ImageUrl(const std::string& url) : url(url) {}
    ImageUrl(const tllf::Url& url) : url(url) {}
    ImageUrl(const ImageBlob& blob) : blob(blob) {}

    std::optional<tllf::Url> url;
    std::optional<ImageBlob> blob;

    std::string write_data();
    void read_data(const std::string& data);
};

struct Image
{
    // Don't ask. The OpenAI API expects us to feed it an image as base64 encoded string.
    ImageUrl image_url;
};

struct Text
{
    std::string text;
};

struct ChatEntry
{
    using Part = std::variant<Text, Image>;
    using ListOfParts = std::vector<Part>;
    std::variant<std::string, ListOfParts> content;
    std::string role;
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
    drogon::Task<std::string> generate(Chatlog history, TextGenerationConfig config = TextGenerationConfig());
    virtual drogon::Task<std::string> generateImpl(Chatlog history, TextGenerationConfig config) = 0;
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
    OpenAIConnector(const std::string& model_name, const std::string& baseurl="https://api.openai.com/", const std::string& api_key="");

    drogon::Task<std::string> generateImpl(Chatlog history, TextGenerationConfig config) override;

    drogon::HttpClientPtr client;
    std::string base;
    std::string model_name;
    std::string api_key;
};

/**
 * Connector for Vertex AI (ie. Google Gemini) API endpoints.
 *
 * @param model_name The name of the model to use. For example, "gemini-1.5-flash".
 * @param hoststr The host string. Defaults to "https://generativelanguage.googleapis.com/".
 * @param api_key The API key to use
*/
struct VertexAIConnector : public LLM
{
    VertexAIConnector(const std::string& model_name, const std::string& hoststr="https://generativelanguage.googleapis.com/", const std::string& api_key="")
        : client(internal::getClient(hoststr)), model_name(model_name), api_key(api_key)
    {
    }

    drogon::Task<std::string> generateImpl(Chatlog history, TextGenerationConfig config) override;

    drogon::HttpClientPtr client;
    std::string model_name;
    std::string api_key;
};

struct LanguageParser
{
    virtual glz::json_t parseReply(const std::string& reply) = 0;
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

    glz::json_t parseReply(const std::string& reply);
    std::set<std::string> altname_for_plaintext;
};

struct MarkdownListParser : public LanguageParser
{
    glz::json_t parseReply(const std::string& reply) override;
};

struct JsonParser : public LanguageParser
{
    glz::json_t parseReply(const std::string& reply) override;
};

struct PlaintextParser : public LanguageParser
{
    glz::json_t parseReply(const std::string& reply) override;
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

struct Chain
{
    Chatlog chatlog;
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