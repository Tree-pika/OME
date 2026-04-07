#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <thread>
#include <iomanip>

#include "matcher/matching_engine.h"

using namespace Exchange;
using namespace Common;

// ==========================================
// 1. 高精度计时器辅助函数 (基于 std::chrono)
// ==========================================
inline long long get_current_nanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ==========================================
// 2. 模拟数据生成器 (冷路径)
// ==========================================
// 预先在内存中生成订单，避免测试时产生任何生成开销
std::vector<MEClientRequest> generate_dummy_orders(size_t num_orders) {
    std::vector<MEClientRequest> orders;
    orders.reserve(num_orders);

    for (size_t i = 0; i < num_orders; ++i) {
        MEClientRequest req;
        req.type_ = ClientRequestType::NEW;
        req.client_id_ = 1; // 假设都来自客户 1
        req.ticker_id_ = 0; // 假设都在交易标的 0 (例如 AAPL)
        req.order_id_ = i;  // client_order_id_ 用作索引追踪
        req.qty_ = 100;

        // 制造买卖交替的订单，并刻意制造价格交叉以触发真实的撮合(TRADE)
        if (i % 2 == 0) {
            req.side_ = Side::BUY;
            req.price_ = 1000 + (i % 10); // 买价 1000 ~ 1009
        } else {
            req.side_ = Side::SELL;
            req.price_ = 1000 + (i % 10); // 卖价 1000 ~ 1009，大概率交叉成交
        }
        orders.push_back(req);
    }
    return orders;
}

// ==========================================
// 3. 统计打印函数 (计算 P50, P99 等长尾延迟)
// ==========================================
void print_latency_stats(const std::string& phase_name, std::vector<long long>& latencies, long long total_time_ns) {
    if (latencies.empty()) return;

    // 排序以计算百分位数
    std::sort(latencies.begin(), latencies.end());

    size_t n = latencies.size();
    long long p50 = latencies[n * 0.50];
    long long p90 = latencies[n * 0.90];
    long long p99 = latencies[n * 0.99];
    long long p99_9 = latencies[n * 0.999];
    long long max_lat = latencies.back();
    long long min_lat = latencies.front();
    
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0LL) / (double)n;
    double throughput = (n * 1e9) / total_time_ns;

    std::cout << "\n===================================================\n";
    std::cout << " [ " << phase_name << " ] Performance Report\n";
    std::cout << "===================================================\n";
    std::cout << " Total Orders Processed : " << n << "\n";
    std::cout << " Throughput (Ops/sec)   : " << std::fixed << std::setprecision(2) << throughput << "\n\n";
    std::cout << " --- Tick-to-Trade Latency (Nanoseconds) ---\n";
    std::cout << " Average  : " << avg << " ns\n";
    std::cout << " Min      : " << min_lat << " ns\n";
    std::cout << " P50 (Med): " << p50 << " ns\n";
    std::cout << " P90      : " << p90 << " ns\n";
    std::cout << " P99      : " << p99 << " ns\n";
    std::cout << " P99.9    : " << p99_9 << " ns\n";
    std::cout << " Max      : " << max_lat << " ns\n";
    std::cout << "===================================================\n";
}

// ==========================================
// 4. 核心压测执行器
// ==========================================
void run_benchmark(size_t num_orders, bool is_warmup, 
                   ClientRequestLFQueue& req_q, 
                   ClientResponseLFQueue& resp_q, 
                   MEMarketUpdateLFQueue& md_q) {
    
    std::cout << (is_warmup ? "Starting Warm-up..." : "Starting Actual Benchmark...") << "\n";

    auto orders = generate_dummy_orders(num_orders);
    
    // 记录每笔订单的发送时间和最终延迟
    std::vector<long long> send_times(num_orders, 0);
    std::vector<long long> latencies;
    latencies.reserve(num_orders);

    // 记录是否已经收到该订单的响应，防止一单多响（如 ACCEPTED 后紧接着 FILLED）重复计算
    std::vector<bool> response_received(num_orders, false);

    size_t send_idx = 0;
    size_t recv_count = 0;

    auto start_time = get_current_nanos();

    // 核心的 Busy-Polling 注入与接收循环
    while (recv_count < num_orders) {
        
        // 1. 抽干市场数据队列 (非常重要！否则撮合引擎写满队列会被阻塞)
        while (auto md = md_q.getNextToRead()) {
            md_q.updateReadIndex();
        }

        // 2. 极速注入请求 (T_0)
        if (send_idx < num_orders) {
            auto req = req_q.getNextToWriteTo();
            if (req) {
                *req = orders[send_idx];
                send_times[send_idx] = get_current_nanos(); // 打点 T_0
                req_q.updateWriteIndex();
                send_idx++;
            }
        }

        // 3. 极速接收响应 (T_1)
        auto resp = resp_q.getNextToRead();
        if (resp) {
            size_t oid = resp->client_order_id_; // 使用 client_order_id_ 定位原订单
            if (oid < num_orders && !response_received[oid]) {
                long long t_1 = get_current_nanos(); // 打点 T_1
                latencies.push_back(t_1 - send_times[oid]);
                response_received[oid] = true;
                recv_count++;
            }
            resp_q.updateReadIndex();
        }
    }

    auto end_time = get_current_nanos();

    if (!is_warmup) {
        print_latency_stats("Official Benchmark", latencies, (end_time - start_time));
    } else {
        std::cout << "Warm-up completed.\n";
    }
}


// ==========================================
// 5. 主函数
// ==========================================
int main() {
    // 禁用标准输出的同步以防拖慢速度
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    // 实例化无锁队列
    ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
    ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
    MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

    // 实例化并启动撮合引擎
    std::cout << "Initializing Matching Engine...\n";
    MatchingEngine engine(&client_requests, &client_responses, &market_updates);
    engine.start();

    // 等待引擎线程完全启动
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 测试参数配置
    constexpr size_t WARMUP_ORDERS = 100'000;
    constexpr size_t BENCHMARK_ORDERS = 1'000'000;

    // 1. 预热阶段 (将 CPU L1/L2 缓存填满，激活分支预测器)
    run_benchmark(WARMUP_ORDERS, true, client_requests, client_responses, market_updates);

    // 2. 休息片刻，让引擎处理完可能的残余异步数据
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 3. 正式压测阶段 (记录真实数据的阶段)
    run_benchmark(BENCHMARK_ORDERS, false, client_requests, client_responses, market_updates);

    // 优雅退出
    engine.stop();
    std::cout << "Benchmark Finished. Engine stopped.\n";

    return 0;
}