#include "tllf/utils.hpp"
#include <cstddef>
#include <drogon/HttpTypes.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>
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
    res = drogon::HttpClient::newHttpClient(hoststr, loop, false, true);
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

ImageBlob ImageBlob::fromFile(const std::string& path, std::string mime) {
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

    // Create and return the ImageBlob
    ImageBlob blob;
    blob.data = std::move(buffer);
    blob.mime = mime;
    return blob;
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

void to_json(nlohmann::json& j, const OpenAIError& d)
{
    j["error"] = d.error;
}

void from_json(const nlohmann::json& j, OpenAIError& d)
{
    j.at("error").get_to(d.error);
}

void to_json(nlohmann::json& j, const OpenAIResponse::Choice::Message& d)
{
    j["content"] = d.content;
}

void from_json(const nlohmann::json& j, OpenAIResponse::Choice::Message& d)
{
    j.at("content").get_to(d.content);
}

void to_json(nlohmann::json& j, const OpenAIResponse::Choice& d)
{
    j["message"] = d.message;
}

void from_json(const nlohmann::json& j, OpenAIResponse::Choice& d)
{
    j.at("message").get_to(d.message);
}

void to_json(nlohmann::json& j, const OpenAIResponse& d)
{
    j["choices"] = d.choices;
}

void from_json(const nlohmann::json& j, OpenAIResponse& d)
{
    j.at("choices").get_to(d.choices);
}

namespace tllf
{

void to_json(nlohmann::json& j, const ChatEntry& d)
{
    j["role"] = d.role;
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr(std::is_same_v<T, std::string>)
            j["content"] = c;
        else {
            static_assert(std::is_same_v<T, ChatEntry::ListOfParts>);
            // j["content"] = c; // TODO: Implement this
            abort();
        }
    }, d.content);
}

void from_json(const nlohmann::json& j, ChatEntry& d)
{
    j.at("role").get_to(d.role);
    if(j.contains("content")) {
        if(j["content"].is_string())
            d.content = j["content"].get<std::string>();
        else {
            // j.at("content").get_to(d.content); // TODO: Implement this
            abort();
        }
    }
}

void to_json(nlohmann::json& j, const std::vector<ChatEntry>& d)
{
    j = nlohmann::json::array();
    for(auto& entry : d) {
        nlohmann::json e;
        to_json(e, entry);
        j.push_back(e);
    }

}

}

void to_json(nlohmann::json& j, const OpenAIDataBody& d)
{
    j["model"] = d.model;
    // j["messages"] = d.messages;
    tllf::to_json(j["messages"], d.messages);
    if(d.max_tokens.has_value()) j["max_tokens"] = d.max_tokens.value();
    if(d.temperature.has_value()) j["temperature"] = d.temperature.value();
    if(d.top_p.has_value()) j["top_p"] = d.top_p.value();
    if(d.frequency_penalty.has_value()) j["frequency_penalty"] = d.frequency_penalty.value();
    if(d.presence_penalty.has_value()) j["presence_penalty"] = d.presence_penalty.value();
    if(d.stop_sequence.has_value()) j["stop_sequence"] = d.stop_sequence.value();
}

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
            co_return co_await generateImpl(std::move(history), std::move(config));
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

    std::string body_str = nlohmann::json(body).dump();
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
        auto err = nlohmann::json::parse(resp->body()).template get<OpenAIError>();
        throw std::runtime_error(error.error);
    }

    OpenAIResponse response = nlohmann::json::parse(resp->body()).template get<OpenAIResponse>();
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
    std::vector<char> data;
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

void to_json(nlohmann::json& j, const VertexError::Error& d)
{
    j["message"] = d.message;
}

void from_json(const nlohmann::json& j, VertexError::Error& d)
{
    j.at("message").get_to(d.message);
}

void to_json(nlohmann::json& j, const VertexError& d)
{
    j["error"] = d.error;
}

void from_json(const nlohmann::json& j, VertexError& d)
{
    j.at("error").get_to(d.error);
}

void to_json(nlohmann::json& j, const VertexTextPart& d)
{
    j["text"] = d.text;
}

void from_json(const nlohmann::json& j, VertexTextPart& d)
{
    j.at("text").get_to(d.text);
}

void to_json(nlohmann::json& j, const VertexImagePart& d)
{
    j["data"] = drogon::utils::base64Encode(std::string_view(d.data.data(), d.data.size()));
    j["mime"] = d.mime;
}

void from_json(const nlohmann::json& j, VertexImagePart& d)
{
    std::string data;
    j.at("data").get_to(data);
    d.data = drogon::utils::base64DecodeToVector(data);
    j.at("mime").get_to(d.mime);
}

void to_json(nlohmann::json& j, const std::vector<std::variant<VertexTextPart, VertexImagePart>>& d)
{
    j = nlohmann::json::array();
    for(auto& entry : d) {
        if(std::holds_alternative<VertexTextPart>(entry)) {
            nlohmann::json e;
            to_json(e, std::get<VertexTextPart>(entry));
            j.push_back(e);
        }
        else {
            nlohmann::json e;
            to_json(e, std::get<VertexImagePart>(entry));
            j.push_back(e);
        }
    }
}

void from_json(const nlohmann::json& j, std::vector<std::variant<VertexTextPart, VertexImagePart>>& d)
{
    for(auto& entry : j) {
        if(entry.contains("text")) {
            VertexTextPart e;
            e = entry;
            d.push_back(e);
        }
        else {
            VertexImagePart e;
            e = entry;
            d.push_back(e);
        }
    }
}

void to_json(nlohmann::json& j, const VertexContent& d)
{
    j["role"] = d.role;
    j["parts"] = d.parts;
}

void from_json(const nlohmann::json& j, VertexContent& d)
{
    j.at("role").get_to(d.role);
    j.at("parts").get_to(d.parts);
}

void to_json(nlohmann::json& j, const std::vector<VertexContent>& d)
{
    j = nlohmann::json::array();
    for(auto& entry : d) {
        nlohmann::json e;
        to_json(e, entry);
        j.push_back(e);
    }
}

void from_json(const nlohmann::json& j, std::vector<VertexContent>& d)
{
    for(auto& entry : j) {
        VertexContent e;
        e = entry;
        d.push_back(e);
    }
}

void to_json(nlohmann::json& j, const VertexDataBody& d)
{
    j["contents"] = d.contents;
    j["safety_settings"] = d.safety_settings;
    j["generationConfig"] = d.generationConfig;
}

void from_json(const nlohmann::json& j, VertexDataBody& d)
{
    j.at("contents").get_to(d.contents);
    j.at("safety_settings").get_to(d.safety_settings);
    j.at("generationConfig").get_to(d.generationConfig);
}

void to_json(nlohmann::json& j, const VertexResponse::Candidate& d)
{
    j["content"] = d.content;
}

void from_json(const nlohmann::json& j, VertexResponse::Candidate& d)
{
    j.at("content").get_to(d.content);
}

void to_json(nlohmann::json& j, const VertexResponse& d)
{
    j["candidates"] = d.candidates;
}

void from_json(const nlohmann::json& j, VertexResponse& d)
{
    j.at("candidates").get_to(d.candidates);
}

void oai2vertexContent(VertexContent& v, const ChatEntry& oai)
{
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr(std::is_same_v<T, std::string>)
            v.parts.push_back(VertexTextPart{c});
        else {
            static_assert(std::is_same_v<T, ChatEntry::ListOfParts>);
            for(auto& p : c) {
                std::visit([&](auto&& c) {
                    using T = std::decay_t<decltype(c)>;
                    if constexpr(std::is_same_v<T, std::string>)
                        v.parts.push_back(VertexTextPart{c});
                    else if constexpr(std::is_same_v<T, ImageBlob>)
                        v.parts.push_back(VertexImagePart{.data = c.data, .mime = c.mime});
                    else if constexpr(std::is_same_v<T, Url>)
                        throw std::runtime_error("VertexAI does not support fetching from URL");
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
    std::string buffered_sys_message;

    for(auto& entry : history) {
        if(entry.role == "system") {
            if(!std::holds_alternative<std::string>(entry.content))
                throw std::runtime_error("System message MUST be a string");
            buffered_sys_message += std::get<std::string>(entry.content) + "\n";
        }
        else {
            VertexContent content;
            // Vertex AI has different name vs OpenAI
            content.role = entry.role == "user" ? "user" : "model";
            if(!buffered_sys_message.empty()) {
                content.parts.push_back(VertexTextPart{buffered_sys_message});
                buffered_sys_message.clear();
            }
            
            oai2vertexContent(content, entry);
            log.push_back(content);
        }
    }
    body.contents = std::move(log);

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

    std::string body_str = nlohmann::json(body).dump();
    LOG_DEBUG << "Request: " << body_str;
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != k200OK) {
        VertexError error = nlohmann::json::parse(resp->body()).get<VertexError>();
        throw std::runtime_error(error.error.message);
    }

    VertexResponse response = nlohmann::json::parse(resp->body()).get<VertexResponse>();
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

void to_json(nlohmann::json& j, const DeepinfraEmbedDataBody& d)
{
    j["inputs"] = d.inputs;
}

void from_json(const nlohmann::json& j, DeepinfraEmbedDataBody& d)
{
    j.at("inputs").get_to(d.inputs);
}

void to_json(nlohmann::json& j, const DeepinfraEmbedResponse& d)
{
    j["embeddings"] = d.embeddings;
}

void from_json(const nlohmann::json& j, DeepinfraEmbedResponse& d)
{
    j.at("embeddings").get_to(d.embeddings);
}

void to_json(nlohmann::json& j, const DeepinfraEmbedError& d)
{
    j["error"] = d.error;
}

void from_json(const nlohmann::json& j, DeepinfraEmbedError& d)
{
    j.at("error").get_to(d.error);
}

Task<std::vector<std::vector<float>>> DeepinfraTextEmbedder::embed(std::vector<std::string> texts)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1/inference/" + model_name);
    req->addHeader("Authorization", "Bearer " + api_key);
    req->setMethod(HttpMethod::Post);
    DeepinfraEmbedDataBody body;
    body.inputs = std::move(texts);
    auto body_str = nlohmann::json(body).dump();
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    if(resp->statusCode() != k200OK) {
        DeepinfraEmbedError error = nlohmann::json::parse(resp->body()).get<DeepinfraEmbedError>();
        throw std::runtime_error(error.error);
    }

    DeepinfraEmbedResponse response = nlohmann::json::parse(resp->body()).get<DeepinfraEmbedResponse>();
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