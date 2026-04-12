#pragma once

#include <sys/epoll.h>
#include <netinet/in.h>
#include <array>
#include <string>
#include <unordered_map>

#include "common/lf_queue.h"
#include "common/thread_utils.h"
#include "common/macros.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

namespace Exchange {
  class OrderGateway final {
  public:
    OrderGateway(int port, 
                 ClientRequestLFQueue* incoming_requests, 
                 ClientResponseLFQueue* outgoing_responses);
    
    ~OrderGateway();

    auto start() -> void;
    auto stop() -> void;

    // 禁用拷贝和移动
    OrderGateway() = delete;
    OrderGateway(const OrderGateway&) = delete;
    OrderGateway(const OrderGateway&&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&&) = delete;

  private:
    auto run() noexcept -> void;
    auto setNonBlockingAndNoDelay(int fd) noexcept -> void;
    auto handleNewConnection() noexcept -> void;
    auto handleClientData(int client_fd) noexcept -> void;

  private:
    int port_;
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    volatile bool running_ = false;

    // 与引擎通信的无锁队列
    ClientRequestLFQueue* incoming_requests_ = nullptr;
    ClientResponseLFQueue* outgoing_responses_ = nullptr;

    // 客户端会话映射管理
    ClientId next_client_id_ = 0;
    
    // ClientId -> FD 的极速映射 (用于发响应)
    std::array<int, ME_MAX_NUM_CLIENTS> client_id_to_fd_;
    // FD -> ClientId 的映射 (用于收请求)，假设系统最大 fd 不会太大
    std::array<ClientId, 10000> fd_to_client_id_; 

    // 为了方便本地 telnet/nc 测试，我们临时使用一个 ticker string 到 TickerId 的映射
    std::unordered_map<std::string, TickerId> ticker_name_to_id_;
  };
}