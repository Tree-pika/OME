// exchange/benchmark_latency.cpp
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <thread>

#include "matcher/matching_engine.h"
#include "common/time_utils.h"

using namespace Exchange;
using namespace Common;

constexpr size_t NUM_ORDERS = 1000000;
constexpr size_t QUEUE_SIZE = 1048576; // 2^20

// ==========================================
// 核心改进：设定目标 QPS，控制发包速率
// ==========================================
constexpr size_t TARGET_QPS = 100000; // 将发包率控制在 10 万笔/秒
constexpr int64_t NANOS_PER_ORDER = 1000000000LL / TARGET_QPS;

// 全局数组记录提交时间戳，用于计算端到端延迟
std::vector<int64_t> submit_times(NUM_ORDERS, 0);
std::vector<int64_t> latencies;

int main() {
    std::cout << "=== V2 Latency Benchmark (Fixed-Rate Injection) ===\n";
    
    // 1. 初始化无锁队列和引擎
    auto* incoming_requests = new ClientRequestLFQueue(QUEUE_SIZE);
    auto* outgoing_responses = new ClientResponseLFQueue(QUEUE_SIZE);
    auto* outgoing_md_updates = new MEMarketUpdateLFQueue(QUEUE_SIZE);

    MatchingEngine engine(incoming_requests, outgoing_responses, outgoing_md_updates);
    
    // 2. 预生成订单 (Pre-generation)
    std::cout << "Pre-generating " << NUM_ORDERS << " orders...\n";
    std::vector<MEClientRequest> pre_generated(NUM_ORDERS);
    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        pre_generated[i].type_ = ClientRequestType::NEW;
        pre_generated[i].client_id_ = 1;
        pre_generated[i].ticker_id_ = 0;
        pre_generated[i].order_id_ = i;
        // 疯狂对敲：偶数买，奇数卖，价格相同，必定成交
        pre_generated[i].side_ = (i % 2 == 0) ? Side::BUY : Side::SELL;
        pre_generated[i].price_ = 10000;
        pre_generated[i].qty_ = 10;
    }

    latencies.reserve(NUM_ORDERS);

    // 3. 启动黑洞消费者线程 (Drainer & Latency Tracker)
    auto sink_thread = Common::createAndStartThread(3, "Benchmark/Sink", [&]() {
        size_t responses_handled = 0;
        while (responses_handled < NUM_ORDERS) {
            // 不断抽干 outgoing_responses 和 outgoing_md_updates 队列
            auto resp = outgoing_responses->getNextToRead();
            if (resp) {
                // 如果是接收确认，代表该订单在引擎中已经处理完毕一轮
                if (resp->type_ == ClientResponseType::ACCEPTED) {
                    int64_t now = getCurrentNanos();
                    int64_t submit_time = submit_times[resp->client_order_id_];
                    if (submit_time > 0) {
                        latencies.push_back(now - submit_time);
                    }
                    responses_handled++;
                }
                outgoing_responses->updateReadIndex();
            }

            auto md = outgoing_md_updates->getNextToRead();
            if (md) {
                outgoing_md_updates->updateReadIndex();
            }
        }
    });

    // 4. 启动撮合引擎
    engine.start();

    // 休眠 1 秒，确保线程都已绑核完毕且操作系统完成了所有缺页中断的分配
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    std::cout << "Starting LATENCY test at fixed rate of " << TARGET_QPS << " QPS...\n";
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 初始化第一笔订单的发包时间
    int64_t next_send_time = getCurrentNanos();

    // 5. 匀速压入订单 (Fixed-Rate Producer Loop)
    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        
        // ==========================================
        // 核心改进：极速自旋等待，直到触发下一个发车时间
        // ==========================================
        while (getCurrentNanos() < next_send_time) {
            // CPU 空转 (Busy-wait) 以获得纳秒级的唤醒精度
        }

        auto slot = incoming_requests->getNextToWriteTo();
        while (!slot) { 
            slot = incoming_requests->getNextToWriteTo(); 
        }

        *slot = pre_generated[i]; // Zero-copy 赋值
        
        // 记录真实的入队瞬间时间！
        submit_times[i] = getCurrentNanos();
        incoming_requests->updateWriteIndex();

        // 计划下一单的发车时间
        next_send_time += NANOS_PER_ORDER;
    }

    // 6. 等待消费完毕
    sink_thread->join();
    auto end_time = std::chrono::steady_clock::now();

    engine.stop();

    // 7. 计算统计指标
    std::chrono::duration<double> diff = end_time - start_time;
    double seconds = diff.count();
    
    // 这里的实际 QPS 应该非常接近你设置的 TARGET_QPS
    long long actual_qps = static_cast<long long>(NUM_ORDERS / seconds); 

    std::sort(latencies.begin(), latencies.end());
    
    int64_t avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0LL) / latencies.size();
    int64_t p50 = latencies[latencies.size() * 0.50];
    int64_t p90 = latencies[latencies.size() * 0.90];
    int64_t p99 = latencies[latencies.size() * 0.99];
    int64_t p999 = latencies[latencies.size() * 0.999];

    std::cout << "\n=== Version 2 Performance Results ===\n";
    std::cout << "Target Injection Rate  : " << TARGET_QPS << " QPS\n";
    std::cout << "Actual Throughput      : " << actual_qps << " orders/sec\n";
    std::cout << "\n=== Latency Percentiles (End-to-End) ===\n";
    std::cout << "Average Latency  : " << avg_lat << " ns\n";
    std::cout << "P50 (Median)     : " << p50 << " ns\n";
    std::cout << "P90 Latency      : " << p90 << " ns\n";
    std::cout << "P99 Latency      : " << p99 << " ns\n";
    std::cout << "P99.9 Latency    : " << p999 << " ns\n";

    delete incoming_requests;
    delete outgoing_responses;
    delete outgoing_md_updates;

    return 0;
}