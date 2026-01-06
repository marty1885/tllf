#include "tllf/utils.hpp"
#include <cstddef>
#include <drogon/HttpTypes.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <glaze/json.hpp>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tllf/tllf.hpp>
#include <tllf/url_parser.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>
#include <type_traits>
#include <variant>
#include <vector>

#include <yaml-cpp/node/node.h>
#include <yaml-cpp/yaml.h>

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

std::string dataUrlfromFile(const std::string& path, std::string mime) {
    // Open the file in binary mode
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    // Determine the file size
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read the file into a buffer
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        throw std::runtime_error("Failed to read file: " + path);
    }
    file.close();

    // Determine MIME type if not provided
    if (mime.empty()) {
        if(buffer.size() < 8)
            throw std::runtime_error("File too small. Cannot automatically decide file type");
        std::string_view magic = std::string_view(buffer.data(), 8);
        if(magic.starts_with("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A"))
            mime = "image/png";
        else if(magic.starts_with("\xFF\xD8\xFF"))
            mime = "image/jpeg";
        else if(magic.starts_with("GIF87a") || magic.starts_with("GIF89a"))
            mime = "image/gif";
        else if(magic.starts_with("BM"))
            mime = "image/bmp";
        else if(magic.starts_with("RIFF") && magic.substr(4, 4) == "WEBP")
            mime = "image/webp";
        else if(magic.starts_with("\x49\x49\x2A\x00") || magic.starts_with("\x4D\x4D\x00\x2A"))
            mime = "image/tiff";
        else if(magic.starts_with("\x00\x00\x01\x00"))
            mime = "image/x-icon";
        else
            throw std::runtime_error("Unsupported file type");
    }

    return "data:" + mime + ";base64," + drogon::utils::base64Encode(buffer.data(), buffer.size());
}
}

template <>
struct glz:: meta<ChatEntry::Part>
{
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"text", "image_url"};
};

namespace glz
{
   template <>
   struct from<JSON, Url>
   {
      template <auto Opts>
      static void op(Url& value, is_context auto&& ctx, auto&& it, auto&& end)
      {
        std::string url_str;
         parse<JSON>::op<Opts>(url_str, ctx, it, end);
         value = Url(url_str);
      }
   };

   template <>
   struct to<JSON, Url>
   {
      template <auto Opts>
      static void op(Url& value, is_context auto&& ctx, auto&& b, auto&& ix) noexcept
      {
         std::string url_str = value.str();
         serialize<JSON>::op<Opts>(url_str, ctx, b, ix);
      }
   };
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
    std::string detail;
};

OpenAIConnector::OpenAIConnector(const std::string& model_name, const std::string& hoststr, const std::string& api_key)
    : model_name(model_name), api_key(api_key)
{
    Url url(hoststr);
    if(!url.validate())
        throw std::runtime_error("Invalid URL: " + hoststr);
    base = url.path();
    client = internal::getClient(url.withFragment("").withParam("").str(), drogon::app().getLoop());
}

Task<std::string> LLM::generate(Chatlog history, TextGenerationConfig config)
{
    constexpr int max_retry = 4;
    for(int retry = 0; retry < max_retry; retry++) {
        bool errored = false;
        // By defaul retry after 500ms
        double retry_delay = 0.5;
        try {
            co_return co_await generateImpl(history, std::move(config));
        }
        catch(const RateLimitError& e) {
            if(e.until_reset_ms.has_value())
                retry_delay = e.until_reset_ms.value() / 1000;
        }
        catch(const std::exception& e) {
            if(retry >= max_retry)
                throw;
            errored = true;
            LOG_WARN << "Request failed. Retrying... " << e.what();
            retry++;
        }

        if(errored) {
            co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), retry_delay);
        }
    }
    throw std::runtime_error("Request failed. Retried " + std::to_string(max_retry) + " times.");
}

drogon::Task<std::string> OpenAIConnector::generateImpl(Chatlog history, TextGenerationConfig config)
{
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    auto p = std::filesystem::path(base) / "chat/completions";

    req->setPath(p.lexically_normal().string());
    req->addHeader("Authorization", "Bearer " + api_key);
    req->addHeader("Accept", "application/json");
    req->setMethod(drogon::HttpMethod::Post);

    OpenAIDataBody body {
        .model = model_name,
        .messages = std::move(history),
        .max_tokens = config.max_tokens,
        .temperature = config.temperature,
        .top_p = config.top_p,
        .frequency_penalty = config.frequency_penalty,
        .presence_penalty = config.presence_penalty,
        .stop_sequence = config.stop_sequence
    };

    std::string body_str = glz::write_json(body).value();
    LOG_DEBUG << "Request: " << body_str;
    req->setBody(body_str);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() == drogon::k429TooManyRequests) {
        double until_reset = 2.;
        if(resp->getHeader("Retry-After") != "")
            until_reset = std::stod(resp->getHeader("Retry-After"));
        else if(resp->getHeader("X-RateLimit-Reset") != "")
            until_reset = std::stod(resp->getHeader("X-RateLimit-Reset"));
        throw RateLimitError(until_reset * 1000);
    }
    else if(resp->statusCode() != drogon::k200OK) {
        OpenAIError error;
        auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(error, resp->body());
        if(ec)
            throw std::runtime_error(error.detail);
    }

    OpenAIResponse response;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(response, resp->body());
    if(ec)
        throw std::runtime_error("Failed to parse response: " + glz::format_error(ec, resp->body()));
    if(response.choices.size() == 0)
        throw std::runtime_error("Server response does not contain any choices");
    auto r = response.choices[0].message.content;
    // HACK: Deepinfra sometimes returns the prompt in the response. This is a workaround.
    if(r.starts_with("assistant\n\n"))
        r = r.substr(11);
    co_return r;
}

struct VertexTextPart
{
    std::string text;
};

struct VertexImagePart
{
    std::string data;
    std::string mime;
};

struct VertexContent
{
    std::string role;
    std::vector<std::variant<VertexTextPart, VertexImagePart>> parts;
};

struct VertexDataBody
{
    std::vector<VertexContent> contents;
    std::vector<std::map<std::string, std::string>> safety_settings;
    std::map<std::string, double> generationConfig;
};

struct VertexResponse
{
    struct Candidate
    {
        VertexContent content;
    };
    std::vector<Candidate> candidates;
};

struct VertexError
{
    struct Error
    {
        std::string message;
    };
    Error error;
};

void oai2vertexContent(VertexContent& v, const ChatEntry& oai)
{
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr(std::is_same_v<T, std::string>)
            v.parts.push_back(VertexTextPart{c});
        else {
            static_assert(std::is_same_v<T, ChatEntry::Parts>);
            for(auto& p : c) {
                std::visit([&](auto&& c) {
                    using T = std::decay_t<decltype(c)>;
                    if constexpr(std::is_same_v<T, std::string>)
                        v.parts.push_back(VertexTextPart{c});
                    else if constexpr(std::is_same_v<T, ImageByUrl>)
                        abort();
                    else
                        throw std::runtime_error("Unsupported type for VertexAI");
                }, p);
            }
        }
    }, oai.content);
}

Task<std::string> VertexAIConnector::generateImpl(Chatlog history, TextGenerationConfig config)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1beta/models/" + model_name + ":generateContent");
    req->setPathEncode(false);
    req->setParameter("key", api_key);
    req->setMethod(HttpMethod::Post);

    VertexDataBody body;

    std::vector<VertexContent> log;

    // Gemini does not have a "system" role. So we need to merge the system messages into the user messages.
    // Also Gemini MUST have alternating user and model messages. Else the API breaks
    VertexContent buffered;
    std::string last_role;
    bool first = true;

    for(auto& entry : history) {
        if(entry.role == "system") {
            if(!first)
                throw std::runtime_error("System message must be the first message in the chatlog");
            // if(!std::holds_alternative<std::string>(entry.content))
            //     throw std::runtime_error("System message MUST be a string");

            buffered.parts.push_back(VertexTextPart{std::get<std::string>(entry.content)});
            buffered.role = "user";
            last_role = "user";
        }
        first = false;

        std::string role = (entry.role == "user" ? "user" : "model");

        if(last_role != entry.role) {
            log.push_back(buffered);
            buffered = VertexContent{.role = role};
        }

        VertexContent v;
        oai2vertexContent(v, entry);
        for(auto& part : v.parts) {
            buffered.parts.push_back(part);
        }
        last_role = role;
    }
    body.contents = std::move(log);

    if(buffered.parts.size() > 0)
        body.contents.push_back(buffered);

    // TOOD: Add more config options
    if(config.max_tokens.has_value()) body.generationConfig["maxOutputTokens"] = config.max_tokens.value();
    if(config.temperature.has_value()) body.generationConfig["temperature"] = config.temperature.value();
    if(config.top_p.has_value()) body.generationConfig["topP"] = config.top_p.value();

    // Force ignore all safety settings
    body.safety_settings = {
        {{"category", "HARM_CATEGORY_HARASSMENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_DANGEROUS_CONTENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_SEXUALLY_EXPLICIT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_HATE_SPEECH"}, {"threshold", "BLOCK_NONE"}}
    };

    std::string body_str = glz::write_json(body).value();
    LOG_DEBUG << "Request: " << body_str;
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != k200OK) {
        VertexError error;
        auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(error, resp->body());
        if(ec)
            throw std::runtime_error("Failed to parse error response: " + glz::format_error(ec, resp->body()));
        throw std::runtime_error(error.error.message);
    }

    VertexResponse response;
    if(auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(response, resp->body()); ec)
        throw std::runtime_error("Failed to parse response: " + glz::format_error(ec, resp->body()));
    if(response.candidates.size() == 0)
        throw std::runtime_error("Server response does not contain any candidates");
    if(response.candidates[0].content.parts.size() == 0)
        throw std::runtime_error("Server response does not contain any content parts");

    if(!std::holds_alternative<VertexTextPart>(response.candidates[0].content.parts[0]))
        throw std::runtime_error("only supports text responses now");
    co_return std::get<VertexTextPart>(response.candidates[0].content.parts[0]).text;
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
            rendered = tllf::utils::replaceAll(rendered, "{" + var + "}", it->second);
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


Task<std::vector<std::vector<float>>> DeepinfraTextEmbedder::embed(std::vector<std::string> texts)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1/inference/" + model_name);
    req->addHeader("Authorization", "Bearer " + api_key);
    req->setMethod(HttpMethod::Post);
    DeepinfraEmbedDataBody body;
    body.inputs = std::move(texts);
    auto body_str = glz::write_json(body).value();
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    if(resp->statusCode() != k200OK) {
        DeepinfraEmbedError error;
        auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(error, resp->body());
        if(ec)
            throw std::runtime_error("Failed to parse error response: " + glz::format_error(ec, resp->body()));
        throw std::runtime_error(error.error);
    }

    DeepinfraEmbedResponse response;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys=false}>(response, resp->body());
    if(ec)
        throw std::runtime_error("Failed to parse response: " + glz::format_error(ec, resp->body()));
    co_return response.embeddings;
}

std::string tllf::to_string(const Chatlog& chatlog)
{
    std::string res;
    for(auto& entry : chatlog) {
        if(std::holds_alternative<std::string>(entry.content))
            res += entry.role + ": " + std::get<std::string>(entry.content) + "\n";
        else
            throw std::runtime_error("Chatlog entry is not a string");
    }
    return res;
}
