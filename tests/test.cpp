#include <drogon/drogon_test.h>
#include <drogon/utils/coroutine.h>
#include <functional>
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

tllf::ToolResult noop_tool(std::string s)
{
    TLLF_DOC("noop")
        .BRIEF("Returns the same thing in it is given.")
        .PARAM(s, "The string to be returned");
    co_return s;
}


tllf::ToolResult optional_tool(std::optional<std::string> s)
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
        auto res = co_await f(invoke_data.dump());
        CO_REQUIRE(res == "Hello!");

        // Make sure these also compiles
        // std::function
        co_await toolize(std::function(noop_tool));
        // lambda
        co_await toolize([](std::string s) { return noop_tool(s); });
        // function pointer
        co_await toolize(&noop_tool);
    };

    auto m = [TEST_CTX]()->drogon::AsyncTask {
        auto f = co_await toolize(optional_tool);
        auto doc = co_await getToolDoc(optional_tool);

        auto res = co_await f(nlohmann::json().dump());
        CO_REQUIRE(res == "The string is not given");
        res = co_await f("Hello");
        CO_REQUIRE(res == "Hello");
        res = co_await f(nlohmann::json({{"s", "Hello"}}).dump());
        CO_REQUIRE(res == "Hello");
    };
}

int main(int argc, char** argv)
{
    return drogon::test::run(argc, argv);
}
