#pragma once

#include <vector>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>

namespace Common {
  template<typename T>
  class LFQueue final {
  public:
    explicit LFQueue(std::size_t num_elems) {
      // 强制要求容量为 2 的幂，为后续的极速位运算取模打下基础
      assert(num_elems > 0 && !(num_elems & (num_elems - 1)) && "Capacity must be a power of 2");
      
      capacity_ = num_elems;
      mask_ = capacity_ - 1;
      
      // 预分配并默认构造所有元素。配合二段式 API，业务层获取指针后直接覆盖赋值，免去内部 placement new 开销
      store_.resize(capacity_, T()); 
    }

    // ========== 生产者接口 (Producer) ==========

    // 阶段一：获取要写入的槽位指针 (Zero-copy)
    auto getNextToWriteTo() noexcept -> T* {
      const size_t w = next_write_index_.load(std::memory_order_relaxed);
      const size_t next_w = (w + 1) & mask_; // 用 & 替代耗时的 % 取模

      // acquire 语义确保准确读取消费者最新的 read_index
      if (next_w == next_read_index_.load(std::memory_order_acquire)) {
        return nullptr; // 队列已满，直接 reject 或由上层处理
      }
      return &store_[w];
    }

    // 阶段二：数据写完后，发布写入索引
    auto updateWriteIndex() noexcept -> void {
      const size_t w = next_write_index_.load(std::memory_order_relaxed);
      // release 语义：确保在索引增加前，T 内部数据的写入已完全刷新到主存
      next_write_index_.store((w + 1) & mask_, std::memory_order_release);
    }


    // ========== 消费者接口 (Consumer) ==========

    // 阶段一：获取要读取的槽位指针
    auto getNextToRead() const noexcept -> const T* {
      const size_t r = next_read_index_.load(std::memory_order_relaxed);
      
      // acquire 语义确保准确读取生产者最新的 write_index，且保证读到的数据不会是旧数据
      if (r == next_write_index_.load(std::memory_order_acquire)) {
        return nullptr; // 队列为空
      }
      return &store_[r];
    }

    // 阶段二：数据读完后，推进读取索引
    auto updateReadIndex() noexcept -> void {
      const size_t r = next_read_index_.load(std::memory_order_relaxed);
      // release 语义确保在“宣告这个槽位空闲”之前，数据已经被彻底读完，防止生产者提前覆盖
      next_read_index_.store((r + 1) & mask_, std::memory_order_release);
    }


    // ========== 辅助接口 ==========

    // 通过头尾指针差值动态计算
    auto size() const noexcept -> std::size_t {
      const size_t r = next_read_index_.load(std::memory_order_acquire);
      const size_t w = next_write_index_.load(std::memory_order_acquire);
      return (w >= r) ? (w - r) : (capacity_ - r + w);
    }


    // 禁用拷贝和移动，防止队列被意外复制导致灾难性的多线程状态错乱
    LFQueue() = delete;
    LFQueue(const LFQueue &) = delete;
    LFQueue(const LFQueue &&) = delete;
    LFQueue &operator=(const LFQueue &) = delete;
    LFQueue &operator=(const LFQueue &&) = delete;

  private:
    std::size_t capacity_;
    std::size_t mask_;
    std::vector<T> store_;

    // 缓存行隔离 (Cache Line Padding)
    // 强制 read 和 write 索引分配在不同的 CPU 64字节缓存行上。
    // 这样网关线程(写)和撮合线程(读)的 CPU 核心在修改各自的索引时，绝对不会触发缓存行失效 (MESI 协议冲突)。
    alignas(64) std::atomic<std::size_t> next_write_index_{0};
    alignas(64) std::atomic<std::size_t> next_read_index_{0};
  };
}