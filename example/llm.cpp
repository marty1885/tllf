#include "tllf/tool.hpp"
#include <drogon/utils/coroutine.h>
#include <glaze/core/opts.hpp>
#include <tllf/tllf.hpp>
#include <drogon/HttpAppFramework.h>

using namespace drogon;

tllf::ToolResult reply(std::string command)
{
    TLLF_DOC("execute_bash")
        .BRIEF("Run a bash command")
        .PARAM(command, "command");
    std::cout << "running: " << command << std::endl;
    co_return std::string("Hello, world\n\n[PROCESS ENDED. STATUS=0]\n");
}

Task<> func()
{
    auto llm = std::make_shared<tllf::OpenAIConnector>("Qwen/Qwen3-235B-A22B-Instruct-2507", "https://api.deepinfra.com/v1/openai", tllf::internal::env("DEEPINFRA_API_KEY"));
    auto config = tllf::TextGenerationConfig();
    config.temperature = 0;

    auto tool = co_await tllf::toolize(reply);

    tllf::PromptTemplate sysprompt("Your name is {name}, and you are {character_desc}. {task_desc}");
    sysprompt.setVariable("name", "Lacia");
    sysprompt.setVariable("character_desc", "a happy, young girl with and would help when possible");
    sysprompt.setVariable("task_desc", "");

    std::cout << "System Prompt:\n=====\n" << sysprompt.render() << "\n=====\n";

    tllf::Chatlog chatlog = {{sysprompt.render(), "system"}, {"Use the execute_bash tool to show me conent of file.txt in CWD", "user"}};
    auto result = co_await llm->generate(chatlog, config, {tool});
    std::cout << "LLM Generated:\n=====\n" << result << "\n=====\n";

    // tllf::PlaintextParser parser;
    // auto parsed = parser.parseReply(result);
    // std::cout << "Parsed:\n=====\n" << parsed << "\n=====\n";

    co_return;
}

int main()
{
    // tllf::debug();
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}
