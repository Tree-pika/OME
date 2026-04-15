#pragma once

#include <sys/epoll.h>
#include <netinet/in.h>
#include <array>
#include <atomic>

#include "common/lf_queue.h"
#include "common/thread_utils.h"
#include "common/macros.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

namespace Exchange {

  // 与引擎通信的定长结构体大小
  constexpr size_t CLIENT_REQUEST_SIZE = sizeof(MEClientRequest);
  // 缓存大小：足以容纳 16 个粘包订单，保持在 1个 4KB 内存页内，对 Cache 极度友好
  constexpr size_t BUFFER_SIZE = CLIENT_REQUEST_SIZE * 16; 

  // 预分配的客户端上下文，避免连接建立时的 malloc/new
  struct ClientContext {
      int fd = -1;
      ClientId client_id = ClientId_INVALID; // 记录底层分配的真实身份
      char buffer[BUFFER_SIZE];
      size_t buf_len = 0;//缓冲区当前的字节数
  };

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
    alignas(64) std::atomic<bool> running_{false}; // 完跨线程缓存行隔离

    // 与引擎通信的无锁队列
    ClientRequestLFQueue* incoming_requests_ = nullptr;
    ClientResponseLFQueue* outgoing_responses_ = nullptr;

    // 客户端会话映射管理
    ClientId next_client_id_ = 0;
    
    // ClientId -> FD 的极速映射 (用于发响应路由)
    std::array<int, ME_MAX_NUM_CLIENTS> client_id_to_fd_;
    
    // 预分配的客户端接收上下文数组，通过 FD 直接索引 (处理粘包/半包)
    std::array<ClientContext, 10000> clients_; 
  };
}