add_executable(ive_poc main.cpp)
target_link_libraries(ive_poc PRIVATE ivevsg::ivevsg)
target_compile_definitions(ive_poc PRIVATE
    "IVEVSG_DEFAULT_TILES=\"${IVEVSG_DEFAULT_TILES}\""
    "IVEVSG_DEFAULT_GLOBE_TILES=\"${IVEVSG_DEFAULT_GLOBE_TILES}\"")
