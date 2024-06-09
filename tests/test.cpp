#include <drogon/drogon_test.h>
#include <glaze/json/json_t.hpp>
#include "tllf/tllf.hpp"

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
    REQUIRE(parsed["interests"].holds<glz::json_t::array_t>());
    REQUIRE(parsed["interests"].get<glz::json_t::array_t>().size() == 2);
    REQUIRE(parsed["interests"][0].get<std::string>() == "music");
    REQUIRE(parsed["interests"][1].get<std::string>() == "sports");
    REQUIRE(parsed["-"].get<std::string>() == "Tom is a good person");

    parsed = parser.parseReply("Tom is a good person");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "Tom is a good person");

    parsed = parser.parseReply("Task:\n - buy milk");
    REQUIRE(parsed.contains("task"));
    REQUIRE(parsed["task"].holds<glz::json_t::array_t>());
    REQUIRE(parsed["task"].get<glz::json_t::array_t>().size() == 1);

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "I need to wake up 9:00 AM tomorrow.");

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");

    parsed = parser.parseReply("The book \"The Lord of the Rings: The Fellowship of the Ring\" is a good book.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"].get<std::string>() == "The book \"The Lord of the Rings: The Fellowship of the Ring\" is a good book.");
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
    REQUIRE(parsed["distance"].get<std::string>() == "11971");
    REQUIRE(parsed["explanation"].get<std::string>() == "The distance between Taipei and New York is approximately 11,971 kilometers (7,454 miles). This distance is calculated as the straight-line distance between the two cities, and does not take into account the actual travel time or route taken.");


    reply = R"({"test": "This is a test"})";
    parsed = parser.parseReply(reply);
    REQUIRE(parsed.contains("test"));
    REQUIRE(parsed["test"].get<std::string>() == "This is a test");
}

int main(int argc, char** argv)
{
    return drogon::test::run(argc, argv);
}