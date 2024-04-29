#include <stdexcept>
#include <string_view>
#include <tllf/tllf.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpAppFramework.h>

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

    while(!remaining.empty()) {
        if(remaining.size() == last_remaining_size)
            throw std::runtime_error("Parser stuck in infinite loop. THIS IS A BUG.");
        last_remaining_size = remaining.size();

        // skip leading whitespace
        size_t pos = remaining.find_first_not_of(" \t\n");
        if(pos == std::string::npos)
            break;
        remaining = remaining.substr(pos);
        if(remaining.empty())
            break;

        size_t colon_pos = remaining.find(':');
        if(colon_pos == std::string::npos) {
            auto it = parsed.find("-");
            if(it == parsed.end())
                parsed["-"] = std::string(remaining);
            else
                parsed["-"].get<std::string>() += std::string(remaining);
            break;
        }

        // extract variable name and get rid of leading/trailing whitespace/decorators
        std::string varname = utils::trim(remaining.substr(0, colon_pos), " \t*_");

        // peek ahead to see if there's a list
        remaining = remaining.substr(colon_pos + 1);
        bool is_list = false;
        auto next_line_pos = remaining.find('\n');
        std::string_view next_line = remaining;
        if(next_line_pos != std::string::npos) {
            std::string_view next_line = remaining.substr(next_line_pos + 1);
            size_t non_whitespace = next_line.find_first_not_of(" \t\n");
            is_list = non_whitespace != std::string::npos && (next_line[non_whitespace] == '-' || next_line[non_whitespace] == '*');
            if(is_list) {
                remaining = remaining.substr(next_line_pos + 1);
            }
        }
        else {
            is_list = remaining.find('-') != std::string::npos || remaining.find('*') != std::string::npos;
        }

        if(is_list) {
            std::vector<std::string> list;
            while(!remaining.empty()) {
                size_t newline_pos = remaining.find('\n');
                std::string_view line = remaining;
                if(newline_pos != std::string::npos) {
                    line = remaining.substr(0, newline_pos);
                    remaining = remaining.substr(newline_pos + 1);
                }
                size_t non_whitespace = line.find_first_not_of(" \t\n");
                if(non_whitespace != std::string::npos && (line[non_whitespace] == '-' || line[non_whitespace] == '*')) {
                    list.push_back(utils::trim(line.substr(non_whitespace + 1)));
                }
                else {
                    break;
                }

                if(newline_pos == std::string::npos) {
                    remaining = "";
                    break;
                }
            }
            parsed[std::string(varname)] = std::move(list);
        }
        else {
            // string
            size_t newline_pos = remaining.find('\n');
            if(newline_pos == std::string::npos) {
                parsed[std::string(varname)] = utils::trim(remaining);
                break;
            }
            parsed[std::string(varname)] = utils::trim(remaining.substr(0, newline_pos));
            remaining = remaining.substr(newline_pos + 1);
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