#include "order_gateway.hpp"

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring> // 为了使用 std::memmove

namespace Exchange {

OrderGateway::OrderGateway(int port, 
                           ClientRequestLFQueue* incoming_requests, 
                           ClientResponseLFQueue* outgoing_responses)
    : port_(port), 
      incoming_requests_(incoming_requests), 
      outgoing_responses_(outgoing_responses) {
    
    client_id_to_fd_.fill(-1);
    
    // 初始化上下文数组：内存预热
    for (auto& ctx : clients_) {
        ctx.fd = -1;
        ctx.client_id = ClientId_INVALID;
        ctx.buf_len = 0;
    }

    // 1. 创建 Server Socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(server_fd_ >= 0, "Failed to create socket.");

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//？
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
    running_.store(true, std::memory_order_release);
    // 绑核 Core 0
    ASSERT(Common::createAndStartThread(0, "Exchange/OrderGateway", [this]() { run(); }) != nullptr, 
           "Failed to start OrderGateway thread.");
}

auto OrderGateway::stop() -> void {
    running_.store(false, std::memory_order_release);
}

auto OrderGateway::setNonBlockingAndNoDelay(int fd) noexcept -> void {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); // 非阻塞socket

    int opt = 1; 
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));// 禁用 Nagle 算法：小包及时发送
}

auto OrderGateway::run() noexcept -> void {
    constexpr int MAX_EVENTS = 256;
    struct epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_acquire)) {
        // ==========================================================
        // 动作 1：无锁轮询引擎的响应队列 (Engine -> Client)
        // ==========================================================
        const auto response = outgoing_responses_->getNextToRead();
        if (LIKELY(response)) {
            int fd = client_id_to_fd_[response->client_id_];
            if (LIKELY(fd >= 0)) {
                // ⭐零序列化开销：直接将 MEClientResponse 结构体的内存块作为二进制流发走！
                ssize_t res = write(fd, reinterpret_cast<const char*>(response), sizeof(MEClientResponse));
                (void)res;
            }
            outgoing_responses_->updateReadIndex();
        }

        // ==========================================================
        // 动作 2：非阻塞轮询网卡事件 (Client -> Engine)
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
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;//数据读干了
            return;
        }

        setNonBlockingAndNoDelay(client_fd);

        struct epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = client_fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);

        ClientId cid = next_client_id_++;//全交易所唯一的client_id_
        if (cid < ME_MAX_NUM_CLIENTS && static_cast<size_t>(client_fd) < clients_.size()) {
            client_id_to_fd_[cid] = client_fd;
            
            // 初始化该连接专属的接收缓冲区上下文
            clients_[client_fd].fd = client_fd;
            clients_[client_fd].client_id = cid;
            clients_[client_fd].buf_len = 0;
        } else {
            close(client_fd);//超过系统承载上限
        }
    }
}

auto OrderGateway::handleClientData(int client_fd) noexcept -> void {
    ClientContext& ctx = clients_[client_fd];

    while (true) { // ET 模式：榨干内核缓冲区
        // 防御性编程：防止被恶意攻击或代码 Bug 导致的缓冲区溢出
        if (UNLIKELY(ctx.buf_len >= BUFFER_SIZE)) {
            ClientId cid = ctx.client_id;
            if (cid != ClientId_INVALID) client_id_to_fd_[cid] = -1;
            close(client_fd);
            ctx.fd = -1;
            ctx.client_id = ClientId_INVALID;
            ctx.buf_len = 0;
            return;
        }

        // 直接读入该连接的专属缓冲区尾部
        ssize_t count = read(client_fd, ctx.buffer + ctx.buf_len, BUFFER_SIZE - ctx.buf_len);

        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 当前内核缓冲区读干了
            
            // 发生网络错误，清理资源
            ClientId cid = ctx.client_id;
            if (cid != ClientId_INVALID) client_id_to_fd_[cid] = -1;
            close(client_fd);
            ctx.fd = -1;
            ctx.client_id = ClientId_INVALID;
            ctx.buf_len = 0;
            break;
        } else if (count == 0) {
            // 客户端主动断开
            ClientId cid = ctx.client_id;
            if (cid != ClientId_INVALID) client_id_to_fd_[cid] = -1;
            close(client_fd);
            ctx.fd = -1;
            ctx.client_id = ClientId_INVALID;
            ctx.buf_len = 0;
            break;
        }

        ctx.buf_len += count;

        // ==========================================================
        // 二进制流粘包处理与极致推送核心
        // ==========================================================
        size_t offset = 0;
        
        // 只要收到的数据长度 >= 一个完整的 MEClientRequest 结构体大小
        while (ctx.buf_len - offset >= CLIENT_REQUEST_SIZE) {
            
            auto request_slot = incoming_requests_->getNextToWriteTo();
            if (UNLIKELY(!request_slot)) {
                // 【背压（Backpressure）机制】
                // 引擎处理拥堵，队列已满。跳出解析循环，已满包和半包都会留在 buffer 里，
                // 等待下一次 epoll 唤醒时继续重试。
                break; 
            }

            // 1. 极速内存拷贝：将二进制流直接强转覆盖到无锁队列的内存上！
            *request_slot = *reinterpret_cast<const MEClientRequest*>(ctx.buffer + offset);

            // 2. 防伪造覆写：绝对不信任网络端传来的 ClientId，强制使用底层系统映射
            request_slot->client_id_ = ctx.client_id;

            // 3. 内存屏障，发布数据给引擎撮合线程
            incoming_requests_->updateWriteIndex();

            offset += CLIENT_REQUEST_SIZE;
        }

        // ==========================================================
        // 二进制流半包 / 背压残留处理
        // ==========================================================
        if (offset > 0) {
            ctx.buf_len -= offset;
            if (ctx.buf_len > 0) {
                // 使用 memmove 将剩下的碎片挪回缓冲区的物理头部，准备下一次拼接
                std::memmove(ctx.buffer, ctx.buffer + offset, ctx.buf_len);
            }
        }
    }
}

} // namespace Exchange