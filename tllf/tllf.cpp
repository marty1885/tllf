#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tllf/tllf.hpp>

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

drogon::Task<std::string> OpenAIConnector::generate(Chatlog history, TextGenerationConfig config, const nlohmann::json& function)
{
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    auto p = std::filesystem::path(base) / "chat/completions";

    req->setPath(p.lexically_normal().string());
    req->addHeader("Authorization", "Bearer " + api_key);
    req->addHeader("Accept", "application/json");
    req->setMethod(drogon::HttpMethod::Post);
    nlohmann::json body;
    body["model"] = model_name;
    if(config.max_tokens.has_value()) body["max_tokens"] = config.max_tokens.value();
    if(config.temperature.has_value()) body["temperature"] = config.temperature.value();
    if(config.top_p.has_value()) body["top_p"] = config.top_p.value();
    if(config.frequency_penalty.has_value()) body["frequency_penalty"] = config.frequency_penalty.value();
    if(config.presence_penalty.has_value()) body["presence_penalty"] = config.presence_penalty.value();
    if(config.stop_sequence.has_value()) body["stop_sequence"] = config.stop_sequence.value();
    if(!function.is_null()) body["functions"] = function;
    nlohmann::json historyJson;
    for(auto& entry : history) {
        nlohmann::json entryJson;
        entryJson["content"] = entry.content;
        entryJson["role"] = entry.role;
        historyJson.push_back(entryJson);
    }
    body["messages"] = historyJson;
    LOG_DEBUG << "Request: " << body.dump();
    req->setBody(body.dump());
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != drogon::k200OK) {
        try {
            nlohmann::json json = nlohmann::json::parse(resp->body());
            if(json.contains("error"))
                throw std::runtime_error(json["error"].get<std::string>());
            throw std::runtime_error("API error: " + json.dump());
        }
        catch(const std::exception& e) {
            throw std::runtime_error("Unknown error. status code: " + std::to_string(resp->statusCode()));
        }
    }
    auto json = nlohmann::json::parse(resp->body());
    if(json["choices"].size() == 0)
        throw std::runtime_error("Error: " + json.dump());
    auto r = json["choices"][0]["message"]["content"].get<std::string>();
    // HACK: Deepinfra sometimes returns the prompt in the response. This is a workaround.
    if(r.starts_with("assistant\n\n"))
        r = r.substr(11);
    co_return r;
}

Task<std::string> VertexAIConnector::generate(Chatlog history, TextGenerationConfig config, const nlohmann::json& function)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1beta/models/" + model_name + ":generateContent");
    // /v1beta/models/gemini-1.5-flash:generateContent
    req->setPathEncode(false);
    req->setParameter("key", api_key);
    req->setMethod(HttpMethod::Post);
    nlohmann::json body;
    nlohmann::json contents;

    // Gemini does not have a "system" role. So we need to merge the system messages into the user messages.
    std::string buffered_sys_message;

    for(auto& entry : history) {
        if(entry.role == "system" && buffered_sys_message.empty()) {
            buffered_sys_message += entry.content + "\n";
            continue;
        }
        else if(entry.role == "system" && buffered_sys_message.empty() == false) {
            throw std::runtime_error("Multiple system messages");
        }

        nlohmann::json entryJson;
        entryJson["role"] = entry.role;
        entryJson["parts"]["text"] = buffered_sys_message + "\n" + entry.content;
        buffered_sys_message.clear();
        contents.push_back(entryJson);
    }
    body["contents"] = contents;
    body["tools"] = function;

    nlohmann::json generation_config;
    // TOOD: Add more config options
    if(config.max_tokens.has_value()) generation_config["maxOutputTokens"] = config.max_tokens.value();
    if(config.temperature.has_value()) generation_config["temperature"] = config.temperature.value();
    if(config.top_p.has_value()) generation_config["topP"] = config.top_p.value();

    if(generation_config.is_null() == false)
        body["generationConfig"] = generation_config;

    // Force ignore all safety settings
    body["safety_settings"] = {
        {{"category", "HARM_CATEGORY_HARASSMENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_DANGEROUS_CONTENT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_SEXUALLY_EXPLICIT"}, {"threshold", "BLOCK_NONE"}},
        {{"category", "HARM_CATEGORY_HATE_SPEECH"}, {"threshold", "BLOCK_NONE"}}
    };

    req->setBody(body.dump());
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != k200OK) {
        nlohmann::json json = nlohmann::json::parse(resp->body());
        if(json.contains("error"))
            throw std::runtime_error(json["error"]["message"].get<std::string>());
        throw std::runtime_error("Unknown error");
    }

    auto resp_body = nlohmann::json::parse(resp->body());
    auto res = resp_body.at("candidates").at(0).at("content").at("parts").at(0).at("text").get<std::string>();
    co_return res;
}

nlohmann::json MarkdownLikeParser::parseReply(const std::string& reply)
{
    nlohmann::json parsed;
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
            parsed[key] = items;
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
            auto it = parsed.find("-");
            if(it == parsed.end())
                parsed["-"] = trimmed;
            else
                parsed["-"] = it->get<std::string>() + "\n" + trimmed;
        }
    }
    
    return parsed;
}

nlohmann::json MarkdownListParser::parseReply(const std::string& reply)
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

    return res;
}

nlohmann::json JsonParser::parseReply(const std::string& reply)
{
    std::string_view remaining(reply);
    if(remaining.starts_with("```json"))
        remaining = remaining.substr(7);
    if(remaining.starts_with("```"))
        remaining = remaining.substr(3);
    if(remaining.ends_with("```"))
        remaining = remaining.substr(0, remaining.size() - 3);
    remaining = utils::trim(remaining, " \n\r\t");

    return nlohmann::json::parse(remaining);
}

nlohmann::json PlaintextParser::parseReply(const std::string& reply)
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

Task<std::vector<std::vector<float>>> DeepinfraTextEmbedder::embed(std::vector<std::string> texts)
{
    HttpRequestPtr req = HttpRequest::newHttpRequest();
    req->setPath("/v1/inference/" + model_name);
    req->addHeader("Authorization", "Bearer " + api_key);
    req->setMethod(HttpMethod::Post);
    nlohmann::json body;
    body["inputs"] = texts;
    req->setBody(body.dump());
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    if(resp->statusCode() != k200OK) {
        nlohmann::json json = nlohmann::json::parse(resp->body());
        if(json.contains("error"))
            throw std::runtime_error(json["error"].get<std::string>());
        throw std::runtime_error("Unknown error");
    }
    auto json = nlohmann::json::parse(resp->body());
    co_return json["embeddings"].get<std::vector<std::vector<float>>>();
}

std::string tllf::to_string(const Chatlog& chatlog)
{
    std::string res;
    for(auto& entry : chatlog) {
        res += entry.role + ": " + entry.content + "\n";
    }
    return res;
}