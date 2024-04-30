#include <drogon/utils/coroutine.h>
#include <tllf/tllf.hpp>
#include <drogon/HttpAppFramework.h>

using namespace drogon;

Task<> func()
{
    auto llm = std::make_shared<tllf::OpenAIConnector>("meta-llama/Meta-Llama-3-8B-Instruct", "https://api.deepinfra.com", tllf::internal::env("DEEPINFRA_API_KEY"));
    auto config = tllf::TextGenerationConfig();
    config.temperature = 0;

    tllf::PromptTemplate sysprompt("Your name is {name}, adn you are {character_desc}.");
    sysprompt.setVariable("name", "Lacia");
    sysprompt.setVariable("character_desc", "a happy, young girl with a lot of energy");

    tllf::PromptTemplate userprompt("What is the distance between earth and the sun?");

    std::vector<tllf::ChatEntry> chatlog = {{sysprompt.render(), "system"}, {userprompt.render(), "user"}};
    auto result = co_await llm->generate(chatlog, config);
    std::cout << "LLM Generated:\n=====\n" << result << "\n=====\n";

    tllf::PlaintextParser parser;
    auto parsed = parser.parseReply(result);
    std::cout << "Parsed:\n=====\n" << parsed.dump(4) << "\n=====\n";

    co_return;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}