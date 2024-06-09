#include <drogon/utils/coroutine.h>
#include <glaze/core/opts.hpp>
#include <tllf/tllf.hpp>
#include <drogon/HttpAppFramework.h>

using namespace drogon;

Task<> func()
{
    auto llm = std::make_shared<tllf::OpenAIConnector>("meta-llama/Meta-Llama-3-8B-Instruct", "https://api.deepinfra.com/v1/openai", tllf::internal::env("DEEPINFRA_API_KEY"));
    auto config = tllf::TextGenerationConfig();
    config.temperature = 0;

    tllf::PromptTemplate sysprompt("Your name is {name}, and you are {character_desc}. {task_desc}");
    sysprompt.setVariable("name", "Lacia");
    sysprompt.setVariable("character_desc", "a happy, young girl with and would help when possible");
    sysprompt.setVariable("task_desc", "Reply in the following format:\n\nneed_action: <true|false if more action then replying the message is needed>\nreply: <your reply>");

    std::cout << "System Prompt:\n=====\n" << sysprompt.render() << "\n=====\n";

    tllf::PromptTemplate userprompt("What is your name?");

    tllf::Chatlog chatlog = {{sysprompt.render(), "system"}, {userprompt.render(), "user"}};
    auto result = co_await llm->generate(chatlog, config);
    std::cout << "LLM Generated:\n=====\n" << result << "\n=====\n";

    tllf::PlaintextParser parser;
    auto parsed = parser.parseReply(result);
    std::cout << "Parsed:\n=====\n" << glz::write<glz::opts{.indentation_width=4}>(parsed) << "\n=====\n";

    co_return;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}