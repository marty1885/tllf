#include <drogon/drogon_test.h>
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
    REQUIRE(parsed["interests"].is_array());
    REQUIRE(parsed["interests"].size() == 2);
    REQUIRE(parsed["interests"][0] == "music");
    REQUIRE(parsed["interests"][1] == "sports");
    REQUIRE(parsed["-"] == "Tom is a good person");

    parsed = parser.parseReply("Tom is a good person");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"] == "Tom is a good person");

    parsed = parser.parseReply("Task:\n - buy milk");
    REQUIRE(parsed.contains("task"));
    REQUIRE(parsed["task"].is_array());
    REQUIRE(parsed["task"].size() == 1);

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"] == "I need to wake up 9:00 AM tomorrow.");

    parsed = parser.parseReply("I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"] == "I need to wake up 9:00 AM tomorrow.\nAnd I need to buy milk.");
}

int main(int argc, char** argv)
{
    return drogon::test::run(argc, argv);
}