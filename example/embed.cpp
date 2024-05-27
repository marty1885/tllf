#include <drogon/utils/coroutine.h>
#include <tllf/tllf.hpp>
#include <drogon/HttpAppFramework.h>

using namespace drogon;

Task<> func()
{
    auto embedder = std::make_shared<tllf::DeepinfraTextEmbedder>("BAAI/bge-large-en-v1.5", "https://api.deepinfra.com", tllf::internal::env("DEEPINFRA_API_KEY"));
    auto res = co_await embedder->embed("Hello, world!");
    std::cout << "Embedding: [";
    for(auto& val : res)
        std::cout << val << ", ";
    std::cout << "]\n";
    std::cout << "Embedding size: " << res.size() << "\n";

    co_return;
}

int main()
{
    app().getLoop()->queueInLoop(async_func(func));
    app().run();
}