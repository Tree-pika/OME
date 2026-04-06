#include "me_order_book.h"

#include "matcher/matching_engine.h"

namespace Exchange {
  MEOrderBook::MEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine)
      : ticker_id_(ticker_id), matching_engine_(matching_engine), orders_at_price_pool_(ME_MAX_PRICE_LEVELS), order_pool_(ME_MAX_ORDER_IDS),
        logger_(logger) {
  }

  MEOrderBook::~MEOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                toString(false, true));

    matching_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    for (auto &itr: cid_oid_to_order_) {//遍历所有客户端对应的订单映射表
      itr.fill(nullptr);//将每个映射表中的值(MEOrder*)都变为nullptr
    }
  }

  /*4.撮合订单*/
  auto MEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id, OrderId new_market_order_id, MEOrder* itr, Qty* leaves_qty) noexcept {
    const auto order = itr;
    const auto order_qty = order->qty_;
    const auto fill_qty = std::min(*leaves_qty, order_qty);

    *leaves_qty -= fill_qty;
    order->qty_ -= fill_qty;

    client_response_ = {ClientResponseType::FILLED, client_id, ticker_id, client_order_id,
                        new_market_order_id, side, itr->price_, fill_qty, *leaves_qty};
    matching_engine_->sendClientResponse(&client_response_);

    client_response_ = {ClientResponseType::FILLED, order->client_id_, ticker_id, order->client_order_id_,
                        order->market_order_id_, order->side_, itr->price_, fill_qty, order->qty_};
    matching_engine_->sendClientResponse(&client_response_);

    market_update_ = {MarketUpdateType::TRADE, OrderId_INVALID, ticker_id, side, itr->price_, fill_qty, Priority_INVALID};
    matching_engine_->sendMarketUpdate(&market_update_);

    if (!order->qty_) {//完全执行
      market_update_ = {MarketUpdateType::CANCEL, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order_qty, Priority_INVALID};
      matching_engine_->sendMarketUpdate(&market_update_);

      removeOrder(order);//订单已被移除CANCEL
    } else {//部分匹配：订单已修改MODIFY
      market_update_ = {MarketUpdateType::MODIFY, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order->qty_, order->priority_};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }
  /*3.检查主动订单是否与被动订单交叉*/
  auto MEOrderBook::checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty, Qty new_market_order_id) noexcept {
    auto leaves_qty = qty;

    if (side == Side::BUY) {//新的买单找卖盘asks
      while (leaves_qty && asks_by_price_) {//从head node开始检查价格水平
        const auto ask_itr = asks_by_price_->first_me_order_;
        if (LIKELY(price < ask_itr->price_)) {
          break;
        }

        match(ticker_id, client_id, side, client_order_id, new_market_order_id, ask_itr, &leaves_qty);
      }
    }
    if (side == Side::SELL) {//新的卖单找买盘asks
      while (leaves_qty && bids_by_price_) {
        const auto bid_itr = bids_by_price_->first_me_order_;
        if (LIKELY(price > bid_itr->price_)) {
          break;
        }

        match(ticker_id, client_id, side, client_order_id, new_market_order_id, bid_itr, &leaves_qty);
      }
    }

    return leaves_qty;
  }

  /*1.添加新订单*/
  auto MEOrderBook::add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void {
    /*0.生成new_market_order_id，用于MEClientResponse和MEMarketUpdate*/
    const auto new_market_order_id = generateNewMarketOrderId();
    //1.生成并发送MEClientResponse，表示接受新订单，并将其发送回撮合引擎
    client_response_ = {ClientResponseType::ACCEPTED, client_id, ticker_id, client_order_id, new_market_order_id, side, price, 0, qty};
    matching_engine_->sendClientResponse(&client_response_);//1.1再由撮合引擎发送给下达新订单的客户

    //2.检查这个新订单是否与另一边已有的被动订单交叉，以及是否完全匹配或部分匹配。
    const auto leaves_qty = checkForMatch(client_id, client_order_id, ticker_id, side, price, qty, new_market_order_id);

    //2.1没有完全匹配(包括完全不匹配)
    if (LIKELY(leaves_qty)) {
      const auto priority = getNextPriority(price);//找出此订单的正确优先级值

      auto order = order_pool_.allocate(ticker_id, client_id, client_order_id, new_market_order_id, side, price, leaves_qty, priority, nullptr,
                                        nullptr);//从order_pool_内存池中分配一个新的MEOrder对象
      addOrder(order);
      
      //为公开市场数据馈送生成MEMarketUpdate，并将其发送回撮合引擎
      market_update_ = {MarketUpdateType::ADD, new_market_order_id, ticker_id, side, price, leaves_qty, priority};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }

  /*2.取消订单*/
  auto MEOrderBook::cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void {
    //1.检查取消请求是否有效
    //1.1clientID有效
    auto is_cancelable = (client_id < cid_oid_to_order_.size());
    MEOrder *exchange_order = nullptr;
    //1.1 取消请求中的OrderId有订单簿中的活跃订单与之相对应
    if (LIKELY(is_cancelable)) {
      auto &co_itr = cid_oid_to_order_.at(client_id);
      exchange_order = co_itr.at(order_id);
      is_cancelable = (exchange_order != nullptr);
    }

    if (UNLIKELY(!is_cancelable)) {//2.1订单无法取消
      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id, ticker_id, order_id, OrderId_INVALID,
                          Side::INVALID, Price_INVALID, Qty_INVALID, Qty_INVALID};
    } else {//2.2订单可以取消
      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id, order_id, exchange_order->market_order_id_,
                          exchange_order->side_, exchange_order->price_, Qty_INVALID, exchange_order->qty_};
      market_update_ = {MarketUpdateType::CANCEL, exchange_order->market_order_id_, ticker_id, exchange_order->side_, exchange_order->price_, 0,
                        exchange_order->priority_};

      removeOrder(exchange_order);

      matching_engine_->sendMarketUpdate(&market_update_);
    }

    matching_engine_->sendClientResponse(&client_response_);
  }

  auto MEOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;
    std::string time_str;

    auto printer = [&](std::stringstream &ss, MEOrdersAtPrice *itr, Side side, Price &last_price, bool sanity_check) {
      char buf[4096];
      Qty qty = 0;
      size_t num_orders = 0;

      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        qty += o_itr->qty_;
        ++num_orders;
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }
      sprintf(buf, " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
              priceToString(itr->price_).c_str(), priceToString(itr->prev_entry_->price_).c_str(), priceToString(itr->next_entry_->price_).c_str(),
              priceToString(itr->price_).c_str(), qtyToString(qty).c_str(), std::to_string(num_orders).c_str());
      ss << buf;
      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        if (detailed) {
          sprintf(buf, "[oid:%s q:%s p:%s n:%s] ",
                  orderIdToString(o_itr->market_order_id_).c_str(), qtyToString(o_itr->qty_).c_str(),
                  orderIdToString(o_itr->prev_order_ ? o_itr->prev_order_->market_order_id_ : OrderId_INVALID).c_str(),
                  orderIdToString(o_itr->next_order_ ? o_itr->next_order_->market_order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }

      ss << std::endl;

      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) || (side == Side::BUY && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted by ascending/descending prices last:" + priceToString(last_price) + " itr:" + itr->toString());
        }
        last_price = itr->price_;
      }
    };

    ss << "Ticker:" << tickerIdToString(ticker_id_) << std::endl;
    {
      auto ask_itr = asks_by_price_;
      auto last_ask_price = std::numeric_limits<Price>::min();
      for (size_t count = 0; ask_itr; ++count) {
        ss << "ASKS L:" << count << " => ";
        auto next_ask_itr = (ask_itr->next_entry_ == asks_by_price_ ? nullptr : ask_itr->next_entry_);
        printer(ss, ask_itr, Side::SELL, last_ask_price, validity_check);
        ask_itr = next_ask_itr;
      }
    }

    ss << std::endl << "                          X" << std::endl << std::endl;

    {
      auto bid_itr = bids_by_price_;
      auto last_bid_price = std::numeric_limits<Price>::max();
      for (size_t count = 0; bid_itr; ++count) {
        ss << "BIDS L:" << count << " => ";
        auto next_bid_itr = (bid_itr->next_entry_ == bids_by_price_ ? nullptr : bid_itr->next_entry_);
        printer(ss, bid_itr, Side::BUY, last_bid_price, validity_check);
        bid_itr = next_bid_itr;
      }
    }

    return ss.str();
  }
}
