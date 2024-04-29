#include <drogon/utils/coroutine.h>
#include <tllf/tllf.hpp>
#include <drogon/HttpAppFramework.h>

using namespace drogon;

Task<> func()
{
    auto llm = std::make_shared<tllf::OpenAIConnector>("meta-llama/Meta-Llama-3-8B-Instruct", "https://api.deepinfra.com", tllf::internal::env("DEEPINFRA_API_KEY"));
    auto config = tllf::TextGenerationConfig();

    tllf::PromptTemplate prompt("Your name is {name} and you are a happy young girl.");
    prompt.setVariable("name", "Lacia");

    std::vector<tllf::ChatEntry> chatlog = {{prompt.render(), "system"}, {"How are you?", "user"}};
    auto result = co_await llm->generate(chatlog, config);
    LOG_INFO << result;

    tllf::MarkdownLikeParser parser;
    auto parsed = parser.parseReply("**interests**:\n - music\n - sports\nTom is a good person");
    std::cout << parsed.dump(4) << std::endl;

    co_return;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}