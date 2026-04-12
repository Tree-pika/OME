#include "order_gateway.hpp"

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring>
#include <sstream>

namespace Exchange {

OrderGateway::OrderGateway(int port, 
                           ClientRequestLFQueue* incoming_requests, 
                           ClientResponseLFQueue* outgoing_responses)
    : port_(port), 
      incoming_requests_(incoming_requests), 
      outgoing_responses_(outgoing_responses) {
    
    client_id_to_fd_.fill(-1);
    fd_to_client_id_.fill(ClientId_INVALID);

    // 初始化测试用的 Ticker 映射 (假设 0 是 AAPL, 1 是 TSLA)
    ticker_name_to_id_["AAPL"] = 0;
    ticker_name_to_id_["TSLA"] = 1;

    // 1. 创建 Server Socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(server_fd_ >= 0, "Failed to create socket.");

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//?
    setNonBlockingAndNoDelay(server_fd_);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    ASSERT(bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) >= 0, "Bind failed.");
    ASSERT(listen(server_fd_, SOMAXCONN) >= 0, "Listen failed.");

    // 2. 创建 Epoll 实例
    epoll_fd_ = epoll_create1(0);
    ASSERT(epoll_fd_ >= 0, "Epoll create failed.");

    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLET; // 读事件 + 边缘触发 (ET)
    event.data.fd = server_fd_;
    ASSERT(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &event) >= 0, "Epoll ctl failed.");
}

OrderGateway::~OrderGateway() {
    stop();
    if (server_fd_ >= 0) close(server_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

auto OrderGateway::start() -> void {
    running_ = true;
    // 使用你封装的底层线程工具，将网关死死钉在 Core 0 上
    ASSERT(Common::createAndStartThread(0, "Exchange/OrderGateway", [this]() { run(); }) != nullptr, 
           "Failed to start OrderGateway thread.");
}

auto OrderGateway::stop() -> void {
    running_ = false;
}

auto OrderGateway::setNonBlockingAndNoDelay(int fd) noexcept -> void {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

auto OrderGateway::run() noexcept -> void {
    constexpr int MAX_EVENTS = 256;
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        // ==========================================================
        // 动作 1：无锁轮询引擎的响应队列 (Engine -> Client)
        // ==========================================================
        const auto response = outgoing_responses_->getNextToRead();
        if (LIKELY(response)) {
            int fd = client_id_to_fd_[response->client_id_];
            if (LIKELY(fd >= 0)) {
                // 将结构体直接作为二进制流发送！真实 HFT 中，客户端也用 struct 直接强转接收
                // 为了演示，我们将其格式化为字符串发回，方便 nc 客户端查看
                std::string resp_str = response->toString() + "\n";
                ssize_t res = write(fd, resp_str.c_str(), resp_str.length());
                (void)res;
            }
            outgoing_responses_->updateReadIndex(); // 释放无锁队列槽位
        }

        // ==========================================================
        // 动作 2：非阻塞轮询网卡事件 (Client -> Engine)
        // 注意：这里的 timeout 参数是 0！绝不休眠！
        // ==========================================================
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 0);
        
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server_fd_) {
                handleNewConnection();
            } else {
                handleClientData(events[i].data.fd);
            }
        }
    }
}

auto OrderGateway::handleNewConnection() noexcept -> void {
    while (true) { // ET 模式下必须循环 accept 直到 EAGAIN
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return;
        }

        setNonBlockingAndNoDelay(client_fd);

        struct epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = client_fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);

        // 分配 ClientId 并建立双向极速映射
        ClientId cid = next_client_id_++;
        // if (cid < ME_MAX_NUM_CLIENTS && client_fd < fd_to_client_id_.size()) {
        if (cid < ME_MAX_NUM_CLIENTS && static_cast<size_t>(client_fd) < fd_to_client_id_.size()) {
            client_id_to_fd_[cid] = client_fd;
            fd_to_client_id_[client_fd] = cid;
        } else {
            close(client_fd); // 超过系统最大承载能力
        }
    }
}

auto OrderGateway::handleClientData(int client_fd) noexcept -> void {
    char buffer[1024];

    while (true) { // ET 模式：榨干内核缓冲区
        ssize_t count = read(client_fd, buffer, sizeof(buffer) - 1);

        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读干了
            // 连接错误，清理资源
            ClientId cid = fd_to_client_id_[client_fd];
            if (cid != ClientId_INVALID) client_id_to_fd_[cid] = -1;
            fd_to_client_id_[client_fd] = ClientId_INVALID;
            close(client_fd);
            break;
        } else if (count == 0) {
            // 客户端主动断开
            ClientId cid = fd_to_client_id_[client_fd];
            if (cid != ClientId_INVALID) client_id_to_fd_[cid] = -1;
            fd_to_client_id_[client_fd] = ClientId_INVALID;
            close(client_fd);
            break;
        }

        buffer[count] = '\0'; // 加上字符串结束符，方便文本解析

        // ==========================================================
        // 零拷贝解析核心 (Zero-Copy Parsing)
        // ==========================================================
        // 我们直接向无锁队列索要要写入的内存指针，直接在槽位上原地构造对象！
        auto request_slot = incoming_requests_->getNextToWriteTo();
        if (UNLIKELY(!request_slot)) {
            // 背压 (Backpressure)：撮合引擎消费太慢，队列满了
            // 真实场景中，这里应记录 Drop 日志或将数据暂存。
            continue; 
        }

        // --- 简单文本解析逻辑 (假设输入如 "NEW AAPL 1 BUY 150 100\n") ---
        // 格式：Type Ticker ClientOrderId Side Price Qty
        std::stringstream ss(buffer);
        std::string type_str, ticker_str, side_str;
        OrderId cid_order_id;
        Price price;
        Qty qty;

        ss >> type_str >> ticker_str >> cid_order_id >> side_str >> price >> qty;

        // 1. 原地赋值，没有任何 MEClientRequest 的局部变量和 Copy 操作！
        if (type_str == "NEW") request_slot->type_ = ClientRequestType::NEW;
        else if (type_str == "CANCEL") request_slot->type_ = ClientRequestType::CANCEL;
        else request_slot->type_ = ClientRequestType::INVALID;

        request_slot->client_id_ = fd_to_client_id_[client_fd];
        
        if (ticker_name_to_id_.find(ticker_str) != ticker_name_to_id_.end()) {
            request_slot->ticker_id_ = ticker_name_to_id_[ticker_str];
        } else {
            request_slot->ticker_id_ = TickerId_INVALID;
        }

        request_slot->order_id_ = cid_order_id;
        
        if (side_str == "BUY") request_slot->side_ = Side::BUY;
        else if (side_str == "SELL") request_slot->side_ = Side::SELL;
        else request_slot->side_ = Side::INVALID;

        request_slot->price_ = price;
        request_slot->qty_ = qty;

        // 2. 发布数据：触发 memory_order_release，撮合线程立刻可见！
        incoming_requests_->updateWriteIndex();
    }
}

} // namespace Exchange