#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <tllf/tllf.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpAppFramework.h>
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

std::string trim(const std::string_view str, const std::string_view whitespace)
{
    size_t first = str.find_first_not_of(whitespace);
    if(std::string::npos == first)
        return std::string(str);
    size_t last = str.find_last_not_of(whitespace);
    return std::string(str.substr(first, (last - first + 1)));
}
}

}

drogon::Task<std::string> OpenAIConnector::generate(std::vector<ChatEntry> history, TextGenerationConfig config, const nlohmann::json& function)
{
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/openai/chat/completions");
    req->addHeader("Authorization", "Bearer " + api_key);
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
    req->setBody(body.dump());
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    auto resp             = co_await client->sendRequestCoro(req);
    // std::cerr << resp->body() << std::endl;
    auto json = nlohmann::json::parse(resp->body());
    co_return json["choices"][0]["message"]["content"].get<std::string>();
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

        if(line.empty()) {
            continue;
        }

        std::string trimmed = utils::trim(line);
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
            parsed[key] = items;
        }
        else if(trimmed.find(": ") != std::string::npos) {
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
    for(auto ch : prompt) {
        if(ch == '{') {
            inVar = true;
            continue;
        }
        else if(ch == '}') {
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