#include <algorithm>
#include <cstddef>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tllf/tllf.hpp>
#include <tllf/url_parser.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpAppFramework.h>
#include <trantor/utils/Logger.h>
#include <vector>

using namespace tllf;
using namespace drogon;

namespace tllf
{

namespace internal
{
static drogon::CacheMap<std::string, drogon::HttpClientPtr> clientCache(drogon::app().getLoop());
drogon::HttpClientPtr getClient(const std::string& hoststr, trantor::EventLoop* loop)
{
    drogon::HttpClientPtr res;
    bool ok = clientCache.findAndFetch(hoststr, res);
    if(ok)
        return res;
    res = drogon::HttpClient::newHttpClient(hoststr, loop);
    clientCache.insert(hoststr, res, 1200);
    return res;
}

std::string env(const std::string& key)
{
    char* val = std::getenv(key.c_str());
    if(val == nullptr) throw std::runtime_error("Environment variable " + key + " not set");
    return val;
}
}

namespace utils
{
std::string replaceAll(std::string str, const std::string& from, const std::string& to)
{
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

std::string_view trim(const std::string_view str, const std::string_view whitespace)
{
    size_t first = str.find_first_not_of(whitespace);
    if(std::string::npos == first)
        return str;
    size_t last = str.find_last_not_of(whitespace);
    return str.substr(first, (last - first + 1));
}
}

}

struct OpenAIDataBody
{
    std::string model;
    std::vector<ChatEntry> messages;
    std::optional<int> max_tokens;
    std::optional<int> temperature;
    std::optional<int> top_p;
    std::optional<int> frequency_penalty;
    std::optional<int> presence_penalty;
    std::optional<int> stop_sequence;

};

struct OpenAIResponse
{
    struct Choice
    {
        struct Message
        {
            std::string content;
        };
        Message message;
    };
    std::vector<Choice> choices;
};

struct OpenAIError
{
    std::string error;
};

OpenAIConnector::OpenAIConnector(const std::string& model_name, const std::string& hoststr, const std::string& api_key)
{
    Url url(hoststr);
    if(!url.validate())
        throw std::runtime_error("Invalid URL: " + hoststr);
    this->model_name = model_name;
    this->api_key = api_key;
    this->base = url.path();
    client = internal::getClient(url.withFragment("").withParam("").str(), drogon::app().getLoop());
}

drogon::Task<std::string> OpenAIConnector::generate(Chatlog history, TextGenerationConfig config)
{
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    auto p = std::filesystem::path(base) / "chat/completions";

    req->setPath(p.lexically_normal().string());
    req->addHeader("Authorization", "Bearer " + api_key);
    req->addHeader("Accept", "application/json");
    req->setMethod(drogon::HttpMethod::Post);

    OpenAIDataBody body {
        .model = model_name,
        .messages = history,
        .max_tokens = config.max_tokens,
        .temperature = config.temperature,
        .top_p = config.top_p,
        .frequency_penalty = config.frequency_penalty,
        .presence_penalty = config.presence_penalty,
        .stop_sequence = config.stop_sequence
    };

    std::string body_str = glz::write_json(body);
    LOG_DEBUG << "Request: " << body_str;
    req->setBody(body_str);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != drogon::k200OK) {
        OpenAIError error;
        auto err = glz::read<glz::opts{.error_on_unknown_keys=false}>(error, resp->body());
        if(err || error.error.empty())
            throw std::runtime_error("Request failed. status code: " + std::to_string(resp->statusCode()));
        throw std::runtime_error(error.error);
    }

    OpenAIResponse response;
    LOG_DEBUG << "Response: " << resp->body();
    auto error = glz::read<glz::opts{.error_on_unknown_keys=false}>(response, resp->body());
    if(error)
        throw std::runtime_error("Error parsing response: " + std::string(error.includer_error));
    if(response.choices.size() == 0)
        throw std::runtime_error("Server response does not contain any choices");
    auto r = response.choices[0].message.content;
    // HACK: Deepinfra sometimes returns the prompt in the response. This is a workaround.
    if(r.starts_with("assistant\n\n"))
        r = r.substr(11);
    co_return r;
}

struct VertexGenerationConfig
{
    std::optional<int> maxOutputTokens;
    std::optional<float> temperature;
    std::optional<float> topP;
};

struct VertexDataBody
{
    VertexGenerationConfig generationConfig;
    tllf::Chatlog contents;
    std::vector<std::map<std::string, std::string>> safety_settings;
};

struct VertexResponse
{
    struct Candidate
    {
        struct Content
        {
            struct Part
            {
                std::string text;
            };
            std::vector<Part> parts;
        };
        Content content;
    };
    std::vector<Candidate> candidate;
};

struct VertexError
{
    struct Error
    {
        std::string message;
    };
    Error error;
};

Task<std::string> VertexAIConnector::generate(Chatlog history, TextGenerationConfig config)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1beta/models/" + model_name + ":generateContent");
    req->setPathEncode(false);
    req->setParameter("key", api_key);
    req->setMethod(HttpMethod::Post);

    VertexDataBody body;

    Chatlog log;

    // Gemini does not have a "system" role. So we need to merge the system messages into the user messages.
    std::string buffered_sys_message;

    for(auto& entry : history) {
        if(entry.role == "system") {
            buffered_sys_message += entry.content + "\n";
        }
        else {
            if(!buffered_sys_message.empty()) {
                log.push_back({"system", buffered_sys_message});
                buffered_sys_message.clear();
            }
            log.push_back(entry);
        }
    }
    body.contents = log;

    // TOOD: Add more config options
    if(config.max_tokens.has_value()) body.generationConfig.maxOutputTokens = config.max_tokens.value();
    if(config.temperature.has_value()) body.generationConfig.temperature = config.temperature.value();
    if(config.top_p.has_value()) body.generationConfig.topP = config.top_p.value();

    // Force ignore all safety settings
    body.safety_settings = {
        {{"category", "HARM_CATEGORY_HARASSMENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_DANGEROUS_CONTENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_SEXUALLY_EXPLICIT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_HATE_SPEECH"}, {"threshold", "BLOCK_NONE"}}
    };

    std::string body_str = glz::write_json(body);
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != k200OK) {
        VertexError error;
        auto err = glz::read_json(error, resp->body());
        if(err)
            throw std::runtime_error("Request failed. status code: " + std::to_string(resp->statusCode()));
        throw std::runtime_error(error.error.message);
    }

    VertexResponse response;
    auto error = glz::read_json(response, resp->body());
    if(error)
        throw std::runtime_error("Error parsing response: " + std::string(error.includer_error));
    if(response.candidate.size() == 0)
        throw std::runtime_error("Server response does not contain any candidates");
    if(response.candidate[0].content.parts.size() == 0)
        throw std::runtime_error("Server response does not contain any content parts");
    co_return response.candidate[0].content.parts[0].text;
}

glz::json_t to_json(const std::vector<std::string> vec)
{
    glz::json_t::array_t res;
    for(auto& str : vec)
        res.push_back(str);
    return res;
}

glz::json_t MarkdownLikeParser::parseReply(const std::string& reply)
{
    glz::json_t parsed;
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
            std::vector<std::string> items;
            while(remaining.empty() == false) {
                line = utils::trim(peek_next_line());
                if(line.empty()) {
                    consume_line();
                    continue;
                }
                if(!line.starts_with("- "))
                    break;
                items.push_back(line.substr(2));
                consume_line();
            }

            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if(altname_for_plaintext.contains(key))
                key = "-";
            parsed[key] = to_json(items);
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
                parsed["-"] = parsed["-"].get<std::string>() + "\n" + trimmed;
        }
    }
    
    return parsed;
}

glz::json_t MarkdownListParser::parseReply(const std::string& reply)
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

    return to_json(res);
}

glz::json_t JsonParser::parseReply(const std::string& reply)
{
    std::string_view remaining(reply);
    if(remaining.starts_with("```json"))
        remaining = remaining.substr(7);
    if(remaining.starts_with("```"))
        remaining = remaining.substr(3);
    if(remaining.ends_with("```"))
        remaining = remaining.substr(0, remaining.size() - 3);
    remaining = utils::trim(remaining, " \n\r\t");

    glz::json_t parsed;
    auto err = glz::read_json(parsed, remaining);
    if(err)
        throw std::runtime_error("Error parsing JSON: " + std::string(err.includer_error));
    return parsed;
}

glz::json_t PlaintextParser::parseReply(const std::string& reply)
{
    return reply;
}

std::string PromptTemplate::render() const
{
    std::string rendered = prompt;
    size_t n_runs = 0;
    for(auto varset = extractVars(rendered); varset.size() > 0; varset = extractVars(rendered)) {
        for(auto& var : varset) {
            auto it = variables.find(var);
            if(it == variables.end())
                throw std::runtime_error("Variable " + var + " not found in variables map");
            rendered = utils::replaceAll(rendered, "{" + var + "}", it->second);
        }
        n_runs++;
        if(n_runs > 6) {
            throw std::runtime_error("Variable replacements haven't converged after 6 runs. Please check for circular dependencies.");
        }
    }
    return rendered;
}

std::unordered_set<std::string> PromptTemplate::extractVars(const std::string& prompt)
{
    std::unordered_set<std::string> prompt_vars;
    std::string varname;
    bool inVar = false;
    for(size_t i = 0; i < prompt.size(); i++) {
        char ch = prompt[i];
        if(ch == '\\') {
            if(i + 1 >= prompt.size())
                throw std::runtime_error("Escape character at end of prompt");
            i++;
        }
        else if(ch == '{' && inVar == false) {
            inVar = true;
            continue;
        }
        else if(ch == '}' && inVar == true) {
            inVar = false;
            prompt_vars.insert(varname);
            varname.clear();
            continue;
        }
        else if(inVar == true && ch == '\n') {
            inVar = false;
            varname.clear();
            continue;
        }

        if(inVar == true) {
            if(ch == '{')
                throw std::runtime_error("Nested curly braces in prompt");
            varname += ch;
        }
    }
    if(inVar == true) {
        throw std::runtime_error("Unmatched curly brace in prompt");
    }
    return prompt_vars;
}

Task<std::vector<float>> DeepinfraTextEmbedder::embed(std::string text)
{
    std::vector<std::string> texts = {std::move(text)};
    co_return (co_await embed(std::move(texts)))[0];
}

struct DeepinfraEmbedDataBody
{
    std::vector<std::string> inputs;
};

struct DeepinfraEmbedResponse
{
    std::vector<std::vector<float>> embeddings;
};

struct DeepinfraEmbedError
{
    std::string error;
};

template <>
struct glz::meta<DeepinfraEmbedResponse> {
  static constexpr auto value = object("request_id", skip{},
    "inference_status", skip{},
    "input_tokens", skip{},
    &DeepinfraEmbedResponse::embeddings);
};

Task<std::vector<std::vector<float>>> DeepinfraTextEmbedder::embed(std::vector<std::string> texts)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1/inference/" + model_name);
    req->addHeader("Authorization", "Bearer " + api_key);
    req->setMethod(HttpMethod::Post);
    DeepinfraEmbedDataBody body;
    body.inputs = std::move(texts);
    auto body_str = glz::write_json(body);
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    if(resp->statusCode() != k200OK) {
        DeepinfraEmbedError error;
        auto err = glz::read_json(error, resp->body());
        if(err)
            throw std::runtime_error("Error parsing response");
        throw std::runtime_error(error.error);
    }

    DeepinfraEmbedResponse response;
    auto error = glz::read_json(response, resp->body());
    std::cout << "Response: " << resp->body() << std::endl;
    if(error)
        throw std::runtime_error("Error parsing response");
    co_return response.embeddings;
}

std::string tllf::to_string(const Chatlog& chatlog)
{
    std::string res;
    for(auto& entry : chatlog) {
        res += entry.role + ": " + entry.content + "\n";
    }
    return res;
}