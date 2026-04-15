#pragma once

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order_book.h"

namespace Exchange {
  class MatchingEngine final {//final
  public:
    MatchingEngine(ClientRequestLFQueue *client_requests,
                   ClientResponseLFQueue *client_responses,
                   MEMarketUpdateLFQueue *market_updates);

    ~MatchingEngine();

    auto start() -> void;//启动和停止主撮合引擎循环的执行

    auto stop() -> void;
    /*1.处理客户请求*/
    auto processClientRequest(const MEClientRequest *client_request) noexcept {
      //1.检查MEClientRequest的类型，并将其转发到相应交易工具的限价订单簿
      auto order_book = ticker_order_book_[client_request->ticker_id_];
      //2.处理不同类型的客户请求
      switch (client_request->type_) {
        case ClientRequestType::NEW: {//2.1.添加新订单
          order_book->add(client_request->client_id_, client_request->order_id_, client_request->ticker_id_,
                           client_request->side_, client_request->price_, client_request->qty_);
        }
          break;

        case ClientRequestType::CANCEL: {//2.2取消订单
          order_book->cancel(client_request->client_id_, client_request->order_id_, client_request->ticker_id_);
        }
          break;

        default: {
          FATAL("Received invalid client-request-type:" + clientRequestTypeToString(client_request->type_));
        }
          break;
      }
    }
    /*2.发布订单响应*/
    auto sendClientResponse(const MEClientResponse *client_response) noexcept {
      // logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), client_response->toString());
      auto next_write = outgoing_ogw_responses_->getNextToWriteTo();
      *next_write = std::move(*client_response);//
      outgoing_ogw_responses_->updateWriteIndex();
    }
    /*3.发布市场数据更新*/
    auto sendMarketUpdate(const MEMarketUpdate *market_update) noexcept {
      // logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), market_update->toString());
      auto next_write = outgoing_md_updates_->getNextToWriteTo();
      *next_write = std::move(*market_update);//
      outgoing_md_updates_->updateWriteIndex();
    }
    /*撮合线程执行的主体循环*/
    auto run() noexcept {
      // logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      //工作线程读取，使用 acquire 语义
      while (running_.load(std::memory_order_acquire)) {
        const auto me_client_request = incoming_requests_->getNextToRead();//1.
        if (LIKELY(me_client_request)) {
          // logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
          //             me_client_request->toString());
          processClientRequest(me_client_request);//2.
          incoming_requests_->updateReadIndex();//3.
        }
      }
    }

    // Deleted default, copy & move constructors and assignment-operators.
    MatchingEngine() = delete;

    MatchingEngine(const MatchingEngine &) = delete;

    MatchingEngine(const MatchingEngine &&) = delete;

    MatchingEngine &operator=(const MatchingEngine &) = delete;

    MatchingEngine &operator=(const MatchingEngine &&) = delete;

  private:
    /*跟踪每个交易工具的限价订单簿MEOrderBook:ticker_id_--MEOrderBook*(new MEOrderBook)*/
    OrderBookHashMap ticker_order_book_;

    /*与其他线程进行通信*/
    ClientRequestLFQueue *incoming_requests_ = nullptr;//处理客户端请求
    ClientResponseLFQueue *outgoing_ogw_responses_ = nullptr;//订单响应
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;//更新市场

    alignas(64) std::atomic<bool> running_{false};//

    std::string time_str_;
    Logger logger_;
  };
}
