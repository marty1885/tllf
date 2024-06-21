add_executable(llm llm.cpp)
target_link_libraries(llm PRIVATE tllf)

add_executable(embed embed.cpp)
target_link_libraries(embed PRIVATE tllf)

add_executable(tool tool.cpp)
target_link_libraries(tool PRIVATE tllf)
