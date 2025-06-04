# CMakeLists.txt

cmake_minimum_required(VERSION 3.5)
project(monoWeb)

# 指定 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 如果你有多个子目录，比如 http，lock，threadpool 等，
# 可以在这里分别把它们添加到 include 路径中
include_directories(
    ${CMAKE_SOURCE_DIR}        # 根目录（用于包含 main.cpp、config.h 等）
    ${CMAKE_SOURCE_DIR}/http/   # 包含 http_conn.h
    ${CMAKE_SOURCE_DIR}/epoller/
    # 如果后续有其它子模块，也可以按需添加：
    # ${CMAKE_SOURCE_DIR}/threadpool
    # ${CMAKE_SOURCE_DIR}/lock
)

# 把你需要编译的源文件全部列在这里
set(SOURCES
    main.cpp
    http/http_conn.cpp
    epoller/epoller.cpp
    # 如果后续有 WebServer、线程池等，记得在这里追加：
    # webserver.cpp
    # threadpool/threadpool.cpp
    # lock/locker.cpp
    # log/log.cpp
    # config.cpp
)

# 生成可执行文件
add_executable(server ${SOURCES})

# 链接 pthread（用于多线程）
find_package(Threads REQUIRED)
target_link_libraries(server PRIVATE Threads::Threads)

# 如果你需要额外链接其他库，比如 MySQL、OpenSSL 等，可以在这里加：
# find_package(MySQL REQUIRED)
# target_link_libraries(server PRIVATE MySQL::Client)

# （可选）将可执行文件放到 bin 目录下
set_target_properties(server
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/
)
