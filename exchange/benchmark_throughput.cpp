// exchange/benchmark_throughput.cpp
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>

#include "matcher/matching_engine.h"
#include "common/time_utils.h"

using namespace Exchange;
using namespace Common;

constexpr size_t NUM_ORDERS = 1000000;
constexpr size_t QUEUE_SIZE = 1048576; // 2^20

// 全局数组记录提交时间戳，用于计算端到端延迟
std::vector<int64_t> submit_times(NUM_ORDERS, 0);
std::vector<int64_t> latencies;

int main() {
    Common::setThreadCore(4);//main线程绑定到Core 4
    std::cout << "=== V2 Throughput Benchmark (Max Injection) ===\n";
    
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

    // 3. 启动黑洞消费者线程 (Drainer & Latency Tracker):绑定到Core3
    // 必须有消费者读走引擎输出，否则引擎会被无锁队列堵死
    auto sink_thread = Common::createAndStartThread(3, "Benchmark/Sink", [&]() {
        size_t responses_handled = 0;
        while (responses_handled < NUM_ORDERS) {
            // 处理客户端响应并计算延迟
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

            // 必须抽干行情队列，否则引擎写不进去
            auto md = outgoing_md_updates->getNextToRead();
            if (md) {
                outgoing_md_updates->updateReadIndex();
            }
        }
    });

    // 4. 启动撮合引擎 (它会绑定到 Core 1)
    engine.start();

    std::cout << "Starting throughput and latency test...\n";
    
    auto start_time = std::chrono::steady_clock::now();

    // 5. 极限压入订单 (Producer Loop)
    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        auto slot = incoming_requests->getNextToWriteTo();
        while (!slot) { // 队列满了，自旋等待
            slot = incoming_requests->getNextToWriteTo();
        }

        *slot = pre_generated[i]; // Zero-copy 赋值
        
        // 记录发车时间！
        submit_times[i] = getCurrentNanos();
        
        // 释放锁存，推给引擎
        incoming_requests->updateWriteIndex();
    }

    // 6. 等待消费完毕
    sink_thread->join();
    auto end_time = std::chrono::steady_clock::now();

    engine.stop();

    // 7. 计算统计指标
    std::chrono::duration<double> diff = end_time - start_time;
    double seconds = diff.count();
    long long qps = static_cast<long long>(NUM_ORDERS / seconds);

    std::sort(latencies.begin(), latencies.end());
    
    int64_t avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0LL) / latencies.size();
    int64_t p50 = latencies[latencies.size() * 0.50];
    int64_t p90 = latencies[latencies.size() * 0.90];
    int64_t p99 = latencies[latencies.size() * 0.99];
    int64_t p999 = latencies[latencies.size() * 0.999];

    std::cout << "\n=== Version 2 Performance Results ===\n";
    std::cout << "Total Orders Processed : " << NUM_ORDERS << "\n";
    std::cout << "Total Time Elapsed     : " << seconds << " seconds\n";
    std::cout << "Throughput (QPS)       : " << qps << " orders/sec\n";
    std::cout << "\n=== Latency Percentiles (End-to-End Core) ===\n";
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