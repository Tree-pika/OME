#pragma once

#include "common/types.h"
#include "common/mem_pool.h"
#include "common/logging.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order.h"

using namespace Common;

namespace Exchange {
  class MatchingEngine;

  class MEOrderBook final {//final
  public:
    explicit MEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine);

    ~MEOrderBook();

    auto add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void;

    auto cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void;

    auto toString(bool detailed, bool validity_check) const -> std::string;

    // Deleted default, copy & move constructors and assignment-operators.以防止意外复制和赋值MEOrderBook对象
    MEOrderBook() = delete;

    MEOrderBook(const MEOrderBook &) = delete;

    MEOrderBook(const MEOrderBook &&) = delete;

    MEOrderBook &operator=(const MEOrderBook &) = delete;

    MEOrderBook &operator=(const MEOrderBook &&) = delete;

  private:
    TickerId ticker_id_ = TickerId_INVALID;//该订单簿所对应的交易工具的交易代码

    MatchingEngine *matching_engine_ = nullptr;

    ClientOrderHashMap cid_oid_to_order_;//用于通过客户ID（ClientId）键来追踪OrderHashMap对象。再通过订单ID（OrderId order_id_ = client_order_id_）键来追踪MEOrder对象。

    MemPool<MEOrdersAtPrice> orders_at_price_pool_;//用于创建新对象，并将不再使用的对象返还到内存池。
    MEOrdersAtPrice *bids_by_price_ = nullptr;
    MEOrdersAtPrice *asks_by_price_ = nullptr;

    OrdersAtPriceHashMap price_orders_at_price_;//使用价格水平作为键，来追踪对应价格水平的MEOrdersAtPrice对象。

    MemPool<MEOrder> order_pool_;//在这个内存池中创建和回收MEOrder对象，无需进行动态内存分配。

    MEClientResponse client_response_;
    MEMarketUpdate market_update_;

    OrderId next_market_order_id_ = 1;//

    std::string time_str_;
    Logger *logger_ = nullptr;

  private:
    auto generateNewMarketOrderId() noexcept -> OrderId {//生成新的市场订单ID
      return next_market_order_id_++;
    }

    auto priceToIndex(Price price) const noexcept {//将价格（Price）转换为OrdersAtPriceHashMap中的索引
      return (price % ME_MAX_PRICE_LEVELS);//将Price参数转换为一个介于0到ME_MAX_PRICE_LEVELS - 1之间的索引
    }

    auto getOrdersAtPrice(Price price) const noexcept -> MEOrdersAtPrice * {
      return price_orders_at_price_.at(priceToIndex(price));//给定价格时访问OrdersAtPriceHashMap类型的price_orders_at_price_映射
    }
    /*3.向订单簿添加新的价格水平。*/
    auto addOrdersAtPrice(MEOrdersAtPrice *new_orders_at_price) noexcept {
      //1.在OrdersAtPriceHashMap中添加新的MEOrdersAtPrice条目 = new_orders_at_price
      price_orders_at_price_.at(priceToIndex(new_orders_at_price->price_)) = new_orders_at_price;

      //2、获取按价格排序的买盘或卖盘的开头
      const auto best_orders_by_price = (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);
      if (UNLIKELY(!best_orders_by_price)) {//2.1边界情况：没有买盘或没有卖盘，订单簿一侧为空
        // 如果当前买盘或卖盘确实没有任何价格层级，直接将链表头指向这个新层级
        (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;
        // 因为是双向循环链表，所以它的前驱（prev_entry_）和后继（next_entry_）都指向它自己
        new_orders_at_price->prev_entry_ = new_orders_at_price->next_entry_ = new_orders_at_price;
      } else {//遍历寻找正确的插入位置
        auto target = best_orders_by_price;//从最优价格 target（链表头）开始往后遍历
        // 只要add_after为真，说明还没找到合适的位置
        bool add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                          (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        if (add_after) {
          target = target->next_entry_;
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        }
        while (add_after && target != best_orders_by_price) {//找到插入位置或遍历一圈停下来
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
          if (add_after)
            target = target->next_entry_;
        }

        if (add_after) { // add new_orders_at_price after target.
          // if (target == best_orders_by_price) {
            target = best_orders_by_price->prev_entry_;//进入add_after必然是转了一圈才停下来
          // }
          new_orders_at_price->prev_entry_ = target;
          target->next_entry_->prev_entry_ = new_orders_at_price;
          new_orders_at_price->next_entry_ = target->next_entry_;
          target->next_entry_ = new_orders_at_price;
        } else { // add new_orders_at_price before target.
          new_orders_at_price->prev_entry_ = target->prev_entry_;
          new_orders_at_price->next_entry_ = target;
          target->prev_entry_->next_entry_ = new_orders_at_price;
          target->prev_entry_ = new_orders_at_price;

          //新价格成为全盘最优价格
          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ > best_orders_by_price->price_) ||
              (new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ < best_orders_by_price->price_)) {
            // target->next_entry_ = (target->next_entry_ == best_orders_by_price ? new_orders_at_price : target->next_entry_);//单节点指针闭环，防御性编程
            (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;//更新最优价格
          }
        }
      }
    }
    /*4.订单簿纵向上删除一个价格水平*/
    auto removeOrdersAtPrice(Side side, Price price) noexcept {
      const auto best_orders_by_price = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
      auto orders_at_price = getOrdersAtPrice(price);

      if (UNLIKELY(orders_at_price->next_entry_ == orders_at_price)) { // empty side of book.
        (side == Side::BUY ? bids_by_price_ : asks_by_price_) = nullptr;
      } else {
        orders_at_price->prev_entry_->next_entry_ = orders_at_price->next_entry_;
        orders_at_price->next_entry_->prev_entry_ = orders_at_price->prev_entry_;

        if (orders_at_price == best_orders_by_price) {
          (side == Side::BUY ? bids_by_price_ : asks_by_price_) = orders_at_price->next_entry_;
        }

        orders_at_price->prev_entry_ = orders_at_price->next_entry_ = nullptr;//只是置空而非delete ptr
      }

      price_orders_at_price_.at(priceToIndex(price)) = nullptr;//移除哈希映射

      orders_at_price_pool_.deallocate(orders_at_price);//价格水平条还给内存池
    }

    /*0.获得价格在对应的价格水平链表中的优先级*/
    auto getNextPriority(Price price) noexcept {
      const auto orders_at_price = getOrdersAtPrice(price);
      if (!orders_at_price)
        return 1lu;

      return orders_at_price->first_me_order_->prev_order_->priority_ + 1;//链表最后一个node的优先级+1
    }
    /*6.继续匹配*/
    auto match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id, 
               OrderId new_market_order_id, MEOrder* bid_itr, Qty* leaves_qty) noexcept;
    /*5.开始匹配新的主动订单*/
    auto checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, 
                       Side side, Price price, Qty qty, Qty new_market_order_id) noexcept;
    
    /*从orderbook中删除已有订单*/
    auto removeOrder(MEOrder *order) noexcept {
      auto orders_at_price = getOrdersAtPrice(order->price_);

      if (order->prev_order_ == order) { // only one element.
        removeOrdersAtPrice(order->side_, order->price_);
      } else { // 水平方向的同价格链表中remove the link.
        const auto order_before = order->prev_order_;
        const auto order_after = order->next_order_;
        order_before->next_order_ = order_after;
        order_after->prev_order_ = order_before;

        if (orders_at_price->first_me_order_ == order) {//更新链表头节点信息 = 第1个订单
          orders_at_price->first_me_order_ = order_after;
        }

        order->prev_order_ = order->next_order_ = nullptr;//只是置空，而非delete ptr
      }

      cid_oid_to_order_.at(order->client_id_).at(order->client_order_id_) = nullptr;//从cid_oid_to_order_哈希表中删除该MEOrder的条目
      order_pool_.deallocate(order);//将order还回对象池
    }

    /*1.往orderbook中添加新订单*/
    auto addOrder(MEOrder *order) noexcept {
      const auto orders_at_price = getOrdersAtPrice(order->price_);

      if (!orders_at_price) {//若价格水平不存在
        order->next_order_ = order->prev_order_ = order;//循环doubly list的第1个node，只能自己指自己

        auto new_orders_at_price = orders_at_price_pool_.allocate(order->side_, order->price_, order, nullptr, nullptr);
        addOrdersAtPrice(new_orders_at_price);
      } else {//存在有效的价格水平
        auto first_order = (orders_at_price ? orders_at_price->first_me_order_ : nullptr);

        first_order->prev_order_->next_order_ = order;
        order->prev_order_ = first_order->prev_order_;
        order->next_order_ = first_order;
        first_order->prev_order_ = order;
      }

      cid_oid_to_order_.at(order->client_id_).at(order->client_order_id_) = order;
    }
  };

  typedef std::array<MEOrderBook *, ME_MAX_TICKERS> OrderBookHashMap;
}
