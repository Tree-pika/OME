#include "market_data_publisher.hpp"

#include <unistd.h>
#include <iostream>
#include <cstring>

namespace Exchange {

MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue* market_updates, 
                                         const std::string& multicast_ip, 
                                         int port)
    : outgoing_md_updates_(market_updates) {
    initSocket(multicast_ip, port);
}

MarketDataPublisher::~MarketDataPublisher() {
    stop();
    if (udp_socket_ >= 0) {
        close(udp_socket_);
    }
}

auto MarketDataPublisher::initSocket(const std::string& multicast_ip, int port) noexcept -> void {
    // 1. 创建 UDP Socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT(udp_socket_ >= 0, "Failed to create UDP socket for MarketDataPublisher.");

    // 2. 配置目标组播地址
    std::memset(&multicast_addr_, 0, sizeof(multicast_addr_));
    multicast_addr_.sin_family = AF_INET;
    multicast_addr_.sin_addr.s_addr = inet_addr(multicast_ip.c_str());
    multicast_addr_.sin_port = htons(port);

    // 注意：作为发送端，我们不需要 bind()，也不需要加入组播组。
    // 我们只需要把包往这个特殊的 IP 地址 sendto() 即可，交换机会帮我们复制和路由。
}

auto MarketDataPublisher::start() -> void {
    //主线程写入，使用 release 语义
    running_.store(true, std::memory_order_release);
    ASSERT(Common::createAndStartThread(2, "Exchange/MarketDataPublisher", [this]() { run(); }) != nullptr, 
           "Failed to start MarketDataPublisher thread.");
}

auto MarketDataPublisher::stop() -> void {
    //主线程写入，使用 release 语义
    running_.store(false, std::memory_order_release);
}

auto MarketDataPublisher::run() noexcept -> void {
    //工作线程读取，使用 acquire 语义
    while (running_.load(std::memory_order_acquire)) {
        // ==========================================================
        // 动作 1：轮询引擎吐出的行情更新队列 (Engine -> Market)
        // ==========================================================
        const auto market_update = outgoing_md_updates_->getNextToRead();
        
        if (LIKELY(market_update)) {
            // ==========================================================
            // 动作 2：零序列化暴力组播 (Fire and Forget)
            // ==========================================================
            // 因为 market_update.h 中使用了 #pragma pack(push, 1)
            // 结构体在内存中是绝对紧凑的，直接把它当作一块 byte 数组发出去！
            ssize_t sent_bytes = sendto(udp_socket_, 
                                        reinterpret_cast<const char*>(market_update), 
                                        sizeof(MEMarketUpdate), 
                                        0, // Flags = 0，因为 UDP 发送几乎不阻塞
                                        (struct sockaddr*)&multicast_addr_, 
                                        sizeof(multicast_addr_));
            
            // 在真实高频环境中，UDP 发送哪怕失败了我们也不重传（实时性大于可靠性）
            if (UNLIKELY(sent_bytes < 0)) {
                // 可以加个计数器统计丢包，但绝对不在这里打日志拖慢循环
            }

            // ==========================================================
            // 动作 3：释放无锁队列槽位，触发 memory_order_release
            // ==========================================================
            outgoing_md_updates_->updateReadIndex();
        }
    }
}

} // namespace Exchange