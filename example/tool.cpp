#include "tllf/tool.hpp"
#include "tllf/tllf.hpp"
#include <drogon/utils/coroutine.h>
#include <drogon/drogon.h>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace tllf;
using namespace drogon;


tllf::ToolResult foo(std::string str, int num)
{
    TLLF_DOC("foo")
        .BRIEF("An example tool.")
        .PARAM(str, "a string")
        .PARAM(num, "a number");
    co_return str + std::to_string(num);
}

tllf::ToolResult bar(double n)
{
    TLLF_DOC("bar")
        .BRIEF("Another example tool.")
        .PARAM(n, "a floating point number");
    co_return std::to_string(n * 2);
}

Task<> func()
{
    auto tool = co_await toolize(foo);
    auto tool2 = co_await toolize(bar);
    nlohmann::json json = {
        {"str", "Hello"},
        {"num", 42}
    };
    auto result = co_await tool(json);
    std::cout << result << std::endl;

    Toolset tools;
    tools.push_back(tool);
    tools.push_back(tool2);

    PromptTemplate prompt(R"(
======
Tools
{tools_list}

Description
{tools_description}

Example
{tool_example}
======
)");

    prompt.setVariable("tools_list", tools.generateToolList());
    prompt.setVariable("tools_description", tools.generateToolDescription());
    prompt.setVariable("tool_example", tool.generateInvokeExample("Hello", 42));
    std::cout << prompt.render() << std::endl;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}