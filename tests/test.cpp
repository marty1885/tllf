#include <drogon/drogon_test.h>
#include <drogon/utils/coroutine.h>
#include <functional>
#include "tllf/parsers.hpp"
#include "tllf/tllf.hpp"
#include "tllf/tool.hpp"
#include <optional>

using namespace tllf;

DROGON_TEST(PromptTemplate)
{
    PromptTemplate prompt("Your name is {name} and you are a happy", {{"name", "Tom"}});
    REQUIRE(prompt.render() == "Your name is Tom and you are a happy");

    prompt = PromptTemplate("Your name is {name} and you are a happy");
    REQUIRE_THROWS(prompt.render());

    prompt = PromptTemplate(R"(Escaped \{variables\} must not be rendered)");
    REQUIRE(prompt.render() == R"(Escaped \{variables\} must not be rendered)");

    prompt = PromptTemplate("Nested {replacment {is}} not allowed");
    REQUIRE_THROWS(prompt.render());

    prompt = PromptTemplate(R"(Invalid {escape\} should throw)");
    REQUIRE_THROWS(prompt.render());

    prompt = PromptTemplate("No variables");
    REQUIRE(prompt.render() == "No variables");

    prompt = PromptTemplate("Recurrent replacment is Ok {var}", {{"var", "{var2}"}, {"var2", "value"}});
    REQUIRE(prompt.render() == "Recurrent replacment is Ok value");

    prompt = PromptTemplate("But be careful with cyclic replacment {var}", {{"var", "{var2}"}, {"var2", "{var}"}});
    REQUIRE_THROWS(prompt.render());
}

DROGON_TEST(MarkdownLikeParser)
{
    MarkdownLikeParser parser;
    auto parsed = parser.parseReply("**interests**:\n - music\n - sports\nTom is a good person");
    REQUIRE(parsed.contains("interests"));
    auto node = std::get<MarkDownListNodes>(parsed["interests"]);
    REQUIRE(node.size() == 2);
    REQUIRE(node[0].value == "music");
    REQUIRE(node[1].value == "sports");
    REQUIRE(node[0].children.empty());
    REQUIRE(node[1].children.empty());
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "Tom is a good person");

    parsed = parser.parseReply("Tom is a good person");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "Tom is a good person");

    parsed = parser.parseReply("Task:\n - buy milk\n - buy bread");
    REQUIRE(parsed.contains("task"));
    node = std::get<MarkDownListNodes>(parsed["task"]);
    REQUIRE(node.size() == 2);
    REQUIRE(node[0].value == "buy milk");
    REQUIRE(node[1].value == "buy bread");
    REQUIRE(node[0].children.empty());
    REQUIRE(node[1].children.empty());

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "I need to wake up 9:00 AM tomorrow.");

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");

    parsed = parser.parseReply("The book \"The Lord of the Rings: The Fellowship of the Ring\" is a good book.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "The book \"The Lord of the Rings: The Fellowship of the Ring\" is a good book.");

    parsed = parser.parseReply(R"(
steps:
- Step 1:
  - be careful
  - be patient
- Step 2:
  - Profit
)");
    REQUIRE(parsed.contains("steps"));
    node = std::get<MarkDownListNodes>(parsed["steps"]);
    REQUIRE(node.size() == 2);
    REQUIRE(node[0].value == "Step 1:");
    REQUIRE(node[1].value == "Step 2:");
    REQUIRE(node[0].children.size() == 2);
    REQUIRE(node[1].children.size() == 1);
    REQUIRE(node[0].children[0].value == "be careful");
    REQUIRE(node[0].children[1].value == "be patient");
    REQUIRE(node[1].children[0].value == "Profit");

}

DROGON_TEST(JsonParser)
{
    std::string reply = R"(```json
{"distance": 11971, "explanation": "The distance between Taipei and New York is approximately 11,971 kilometers (7,454 miles). This distance is calculated as the straight-line distance between the two cities, and does not take into account the actual travel time or route taken."}
```)";
    JsonParser parser;
    auto parsed = parser.parseReply(reply);
    REQUIRE(parsed.contains("distance"));
    REQUIRE(parsed.contains("explanation"));
    REQUIRE(parsed["distance"].get<double>() == 11971);
    REQUIRE(parsed["explanation"].get<std::string>() == "The distance between Taipei and New York is approximately 11,971 kilometers (7,454 miles). This distance is calculated as the straight-line distance between the two cities, and does not take into account the actual travel time or route taken.");


    reply = R"({"test": "This is a test"})";
    parsed = parser.parseReply(reply);
    REQUIRE(parsed.contains("test"));
    REQUIRE(parsed["test"].get<std::string>() == "This is a test");
}

DROGON_TEST(json2yaml)
{
    std::string str = R"({"test": "This is a test"})";
    nlohmann::json json = nlohmann::json::parse(str);
    YAML::Node node = internal::json2yaml(json);
    REQUIRE(node["test"].as<std::string>() == "This is a test");

    str = R"({"int": 42, "string": "Hello", "bool": true, "array": [1, 2, 3], "object": {"key": "value"}})";
    json = nlohmann::json::parse(str);
    node = internal::json2yaml(json);
    REQUIRE(node["int"].as<int>() == 42);
    REQUIRE(node["string"].as<std::string>() == "Hello");
    REQUIRE(node["bool"].as<bool>() == true);
    REQUIRE(node["array"][0].as<int>() == 1);
    REQUIRE(node["array"][1].as<int>() == 2);
    REQUIRE(node["array"][2].as<int>() == 3);
    REQUIRE(node["object"]["key"].as<std::string>() == "value");
}

DROGON_TEST(yaml2json)
{
    YAML::Node node;
    node["test"] = "This is a test";
    nlohmann::json json = internal::yaml2json(node);
    REQUIRE(json["test"].get<std::string>() == "This is a test");

    node = YAML::Node();
    node["int"] = 42;
    node["string"] = "Hello";
    node["bool"] = true;
    node["array"].push_back(1);
    node["array"].push_back(2);
    node["array"].push_back(3);
    node["object"]["key"] = "value";

    json = internal::yaml2json(node);
    REQUIRE(json["int"].get<double>() == 42);
    REQUIRE(json["string"].get<std::string>() == "Hello");
    REQUIRE(json["bool"].get<bool>() == true);
    REQUIRE(json["array"].is_array());
    REQUIRE(json["array"].size() == 3);
    REQUIRE(json["array"][0].get<double>() == 1);
    REQUIRE(json["array"][1].get<double>() == 2);
    REQUIRE(json["array"][2].get<double>() == 3);
    REQUIRE(json["object"]["key"].get<std::string>() == "value");

}

DROGON_TEST(mdlist2json)
{
    std::string str = R"(
steps:
- online_search:
   - query: cat
   - page: 2
)";

    auto parsed = MarkdownLikeParser().parseReply(str);
    auto json = tllf::to_json(parsed["steps"].get<MarkDownListNodes>()[0]);
    REQUIRE(nlohmann::json(json).dump() == R"({"online_search":{"page":2,"query":"cat"}})");

    str = R"(
steps:
- online_search
   - query: cat
   - page: 2
)";
    parsed = MarkdownLikeParser().parseReply(str);
    json = tllf::to_json(parsed["steps"].get<MarkDownListNodes>()[0]);
    REQUIRE(nlohmann::json(json).dump() == R"({"online_search":{"page":2,"query":"cat"}})");
}

// DROGON_TEST(ChatlogSerDes)
// {
//     Chatlog log;
//     log.push_back("Hello", "user");
//     log.push_back("Hi", "assistant");
//     log.push_back("How are you?", "user");
//     log.push_back("I'm fine", "assistant");

//     std::string serialized = log.to_json_string();
//     std::cout << serialized << std::endl;
//     Chatlog deserialized;
//     REQUIRE_NOTHROW(deserialized = Chatlog::from_json_string(serialized));
//     REQUIRE(deserialized.size() == 4);
// }

tllf::ToolResult noop_tool(const tllf::Chatlog& log, std::string s)
{
    TLLF_DOC("noop")
        .BRIEF("Returns the same thing in it is given.")
        .PARAM(s, "The string to be returned");
    co_return s;
}


tllf::ToolResult optional_tool(const tllf::Chatlog& log, std::optional<std::string> s)
{
    TLLF_DOC("noop")
        .BRIEF("Returns the same thing in it is given.")
        .PARAM(s, "The string to be returned");
    co_return s.value_or("The string is not given");
}

DROGON_TEST(tool)
{
    auto n = [TEST_CTX]()->drogon::AsyncTask {
        auto f = co_await toolize(noop_tool);
        auto doc = co_await getToolDoc(noop_tool);
        CO_REQUIRE(doc.name == "noop");
        CO_REQUIRE(doc.brief_ == "Returns the same thing in it is given.");
        CO_REQUIRE(doc.params.size() == 1);
        CO_REQUIRE(doc.params[0].first == "s");
        CO_REQUIRE(doc.params[0].second.desc == "The string to be returned");

        nlohmann::json invoke_data;
        invoke_data["s"] = "Hello!";
        auto res = co_await f(tllf::Chatlog{}, invoke_data);
        CO_REQUIRE(res == "Hello!");

        // Make sure these also compiles
        // std::function
        co_await toolize(std::function(noop_tool));
        // lambda
        co_await toolize([](const tllf::Chatlog& log, std::string s) { return noop_tool(log, s); });
        // function pointer
        co_await toolize(&noop_tool);
    };

    auto m = [TEST_CTX]()->drogon::AsyncTask {
        auto f = co_await toolize(optional_tool);
        auto doc = co_await getToolDoc(optional_tool);

        auto res = co_await f(tllf::Chatlog{}, nlohmann::json());
        CO_REQUIRE(res == "The string is not given");
        res = co_await f(tllf::Chatlog{}, "Hello");
        CO_REQUIRE(res == "Hello");
        res = co_await f(tllf::Chatlog{}, nlohmann::json({{"s", "Hello"}}));
        CO_REQUIRE(res == "Hello");
    };
}

int main(int argc, char** argv)
{
    return drogon::test::run(argc, argv);
}