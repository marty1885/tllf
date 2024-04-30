add_executable(llm llm.cpp)
target_link_libraries(llm PRIVATE tllf)

add_executable(embed embed.cpp)
target_link_libraries(embed PRIVATE tllf)
