#include <drogon/drogon_test.h>
#include "tllf/tllf.hpp"

using namespace tllf;

DROGON_TEST(PromptTemplate)
{
    PromptTemplate prompt("Your name is {name} and you are a happy", {{"name", "Tom"}});
    REQUIRE(prompt.render() == "Your name is Tom and you are a happy");

    prompt = PromptTemplate("Your name is {name} and you are a happy");
    REQUIRE_THROWS(prompt.render());
}

DROGON_TEST(MarkdownLikeParser)
{
    MarkdownLikeParser parser;
    auto parsed = parser.parseReply("**interests**:\n - music\n - sports\nTom is a good person");
    std::cout << parsed.dump(4) << std::endl;
    REQUIRE(parsed.contains("interests"));
    REQUIRE(parsed["interests"].is_array());
    REQUIRE(parsed["interests"].size() == 2);
    REQUIRE(parsed["interests"][0] == "music");
    REQUIRE(parsed["interests"][1] == "sports");
    REQUIRE(parsed["-"] == "Tom is a good person");

    parsed = parser.parseReply("Tom is a good person");
    std::cout << parsed.dump(4) << std::endl;
    REQUIRE(parsed.contains("-"));
    REQUIRE(parsed["-"] == "Tom is a good person");

    parsed = parser.parseReply("Task:\n - buy milk");
    std::cout << parsed.dump(4) << std::endl;
    REQUIRE(parsed.contains("Task"));
    REQUIRE(parsed["Task"].is_array());
    REQUIRE(parsed["Task"].size() == 1);
}

int main(int argc, char** argv)
{
    return drogon::test::run(argc, argv);
}