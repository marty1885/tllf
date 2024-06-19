#include <algorithm>
#include <cstddef>
#include <drogon/HttpTypes.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/core/opts.hpp>
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
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>
#include <type_traits>
#include <variant>
#include <vector>

using namespace tllf;
using namespace drogon;

static constexpr glz::opts laxed_opt = {.error_on_unknown_keys=false}; 

template<>
struct glz::meta<ImageBlob>
{
    using T = ImageBlob;
    static constexpr auto value = object(
        "url", custom<&T::read_data, &T::write_data>
    );
};

template<>
struct glz::meta<Url>
{
    using T = Url;
    static constexpr auto value = object(
        "url", custom<&T::from, &T::str>
    );
};

template <>
struct glz::meta<Text>
{
    static constexpr auto value = object(
        "text", &Text::text
    );
};

glz::json_t to_json(const ChatEntry::Part& data)
{
    glz::json_t json;
    std::visit([&](auto&& arg) -> void {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, std::string>) {
            json["type"] = "text";
            json["text"] = arg;
        }
        else if constexpr(std::is_same_v<T, Url>) {
            json["type"] = "image_url";
            json["image_url"]["url"] = arg.str();
        }
        else if constexpr(std::is_same_v<T, ImageBlob>) {
            json["type"] = "image_url";
            json["image_url"]["url"] = arg.write_data();
        }
        else if constexpr(std::is_same_v<T, ImageBlob>) {
            return arg.write_data();
        }
    }, data);
    return json;
}

glz::json_t ChatEntry::Content::write_data() const
{
    auto& data = *this;
    glz::json_t json;
    std::visit([&](auto&& arg) -> void {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, std::string>) {
            json = arg;
        }
        else if constexpr(std::is_same_v<T, ListOfParts>) {
            glz::json_t::array_t arr;
            arr.reserve(arg.size());
            for(auto& part : arg) {
                arr.push_back(to_json(part));
            }
            json = arr;
        }
    }, data);
    return json;
}

void ChatEntry::Content::read_data(const std::string& data)
{
    throw std::runtime_error("ChatEntry::Content is read-only");
}

template <>
struct glz::meta<ChatEntry::Content>
{
   using T = ChatEntry::Content;
    static constexpr auto value = object(
        "content", custom<&T::read_data, &T::write_data>
    );
};

glz::json_t ChatEntry::write_content() const
{
    glz::json_t json;
    glz::json_t c = content.write_data();
    if(c.holds<std::string>())
        json = c.get<std::string>();
    else if(c.holds<glz::json_t::array_t>())
        json = c.get<glz::json_t::array_t>();
    else
        throw std::runtime_error("Unexpected data while serializing conversaion content");
    return json;
}

void ChatEntry::read_data(const std::string& data)
{
    throw std::runtime_error("ChatEntry is read-only");
}

template <>
struct glz::meta<ChatEntry>
{
   using T = ChatEntry;
    static constexpr auto value = object(
        "content", custom<&T::read_data, &T::write_content>,
        "role", &T::role
    );
};

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

std::string ImageBlob::write_data() const
{
    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
    return "data:" + mime + ";base64," + drogon::utils::base64Encode(sv);
}

void ImageBlob::read_data(const std::string& value)
{
    std::string_view remaining = value;
    if(remaining.starts_with("data:")) {
        remaining = remaining.substr(5);
    }
    
    size_t mime_end = remaining.find(';');
    if(mime_end == std::string::npos)
        throw std::runtime_error("Invalid data URL: " + value);
    mime = std::string(remaining.substr(0, mime_end));

    size_t base64_start = remaining.find(',');
    if(base64_start == std::string::npos)
        throw std::runtime_error("Invalid data URL: " + value);
    data = drogon::utils::base64DecodeToVector(remaining.substr(base64_start + 1));
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
        return std::string_view{};
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

    std::string body_str = glz::write_json(body);
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
        auto err = glz::read<laxed_opt>(error, resp->body());
        if(err || error.error.empty())
            throw std::runtime_error("Request failed. status code: " + std::to_string(resp->statusCode()));
        throw std::runtime_error(error.error);
    }

    OpenAIResponse response;
    auto error = glz::read<laxed_opt>(response, resp->body());
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

struct VertexTextPart
{
    std::string text;
};

struct VertexImagePart
{
    std::vector<char> data;
    std::string mime;

    void deserialize(const std::string& str) {throw std::runtime_error("Not implemented");}
    glz::json_t serialize() const
    {
        glz::json_t res;
        res["mime_type"] = mime;
        res["data"] = drogon::utils::base64Encode(std::string_view(data.data(), data.size()));
        return res;
    }
};

template<>
struct glz::meta<VertexImagePart>
{
    using T = VertexImagePart;
    static constexpr auto value = object("inline_data", custom<&T::deserialize, &T::serialize>);
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
    std::map<std::string, double> generationConfig; // HACK: Workaround Glaze bug. This should be a struct.
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

    std::string body_str = glz::write_json(body);
    LOG_DEBUG << "Request: " << body_str;
    req->setBody(body_str);
    req->setContentTypeCode(CT_APPLICATION_JSON);
    auto resp = co_await client->sendRequestCoro(req);
    LOG_DEBUG << "Response: " << resp->body();
    if(resp->statusCode() != k200OK) {
        VertexError error;
        auto err = glz::read<laxed_opt>(error, resp->body());
        if(err)
            throw std::runtime_error("Request failed. status code: " + std::to_string(resp->statusCode()));
        throw std::runtime_error(error.error.message);
    }

    VertexResponse response;
    auto error = glz::read<laxed_opt>(response, resp->body());
    if(error)
        throw std::runtime_error("Error parsing response: " + std::string(error.includer_error));
    if(response.candidates.size() == 0)
        throw std::runtime_error("Server response does not contain any candidates");
    if(response.candidates[0].content.parts.size() == 0)
        throw std::runtime_error("Server response does not contain any content parts");

    if(!std::holds_alternative<VertexTextPart>(response.candidates[0].content.parts[0]))
        throw std::runtime_error("only supports text responses now");
    co_return std::get<VertexTextPart>(response.candidates[0].content.parts[0]).text;
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
    auto error = glz::read<laxed_opt>(response, resp->body());
    if(error)
        throw std::runtime_error("Error parsing response");
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