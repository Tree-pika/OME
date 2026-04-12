# 新增这一行，让编译器在解析 #include "order_server/..." 时知道去当前目录找
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 收集 exchange 目录下的各个业务模块源文件
file(GLOB_RECURSE EXCHANGE_SOURCES
    "market_data/*.cpp"
    "matcher/*.cpp"
    "order_server/*.cpp"
)

# 将撮合核心逻辑编译为一个静态库
add_library(libexchange STATIC ${EXCHANGE_SOURCES})


# ==========================================
# 编译两套独立的 Benchmark 程序
# ==========================================
# 1. 极限吞吐量测试
# 生成 benchmark 可执行文件
add_executable(benchmark_throughput benchmark_throughput.cpp)
# 链接所需库：exchange 本身的逻辑库，common 基础库，以及系统线程库
target_link_libraries(benchmark_throughput PRIVATE libexchange libcommon Threads::Threads)

# 2. 穿透延迟测试
add_executable(benchmark_latency benchmark_latency.cpp)
target_link_libraries(benchmark_latency PRIVATE libexchange libcommon Threads::Threads)