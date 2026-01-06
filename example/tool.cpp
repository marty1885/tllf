#include "tllf/tool.hpp"
#include "tllf/tllf.hpp"
#include <drogon/utils/coroutine.h>
#include <drogon/drogon.h>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace tllf;
using namespace drogon;


tllf::ToolResult foo(const tllf::Chatlog& log, std::string str, int num)
{
    TLLF_DOC("foo")
        .BRIEF("An example tool.")
        .PARAM(str, "a string")
        .PARAM(num, "a number");
    co_return str + std::to_string(num);
}

tllf::ToolResult bar(const tllf::Chatlog& log, double n)
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
    auto result = co_await tool(json.dump());
    std::cout << result << std::endl;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}
