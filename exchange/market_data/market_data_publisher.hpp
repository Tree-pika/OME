#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "market_data/market_update.h"

namespace Exchange {
  class MarketDataPublisher final {
  public:
    // 构造函数接收引擎传出的行情无锁队列，以及组播的 IP 和端口
    MarketDataPublisher(MEMarketUpdateLFQueue* market_updates, 
                        const std::string& multicast_ip, 
                        int port);
    
    ~MarketDataPublisher();

    auto start() -> void;
    auto stop() -> void;

    // 禁用拷贝和移动
    MarketDataPublisher() = delete;
    MarketDataPublisher(const MarketDataPublisher&) = delete;
    MarketDataPublisher(const MarketDataPublisher&&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&&) = delete;

  private:
    auto run() noexcept -> void;
    auto initSocket(const std::string& multicast_ip, int port) noexcept -> void;

  private:
    MEMarketUpdateLFQueue* outgoing_md_updates_ = nullptr;
    
    int udp_socket_ = -1;
    sockaddr_in multicast_addr_{};
    
    alignas(64) std::atomic<bool> running_{false};//
  };
}