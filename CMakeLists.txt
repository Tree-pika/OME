cmake_minimum_required(VERSION 3.10)
project(LightningMatch CXX)

# 全局编译选项，开启 -O3 优化并启用多线程支持
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -Werror -Wpedantic -pthread")

# 包含项目根目录，以便于寻找 common/... 和 exchange/... 的头文件
include_directories(${PROJECT_SOURCE_DIR})

# 查找系统的线程库（为底层的无锁队列和核心绑定提供 pthread 支持）
find_package(Threads REQUIRED)

# 添加子目录
add_subdirectory(common)
add_subdirectory(exchange)