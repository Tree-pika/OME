#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

namespace Common {
  template<typename T>
  class MemPool final {
  private:
    union Block {
      T object_;
      uint32_t next_free_index_;

      // 【核心魔法】：因为 T 可能是非平凡类型（有自定义构造/析构）
      // 编译器会默认 delete 掉 Block 的构造和析构。
      // 我们必须显式提供空的实现，告诉编译器：“内存分配时什么都不用做，生命周期由我手动管理”。
      Block() {}  
      ~Block() {} 
    };

    std::vector<Block> store_;
    uint32_t next_free_index_ = 0;

  public:
    explicit MemPool(std::size_t num_elems) : 
        store_(num_elems) /* 触发 Block() 空构造，仅分配虚拟内存 */ {
      
      // 初始化空闲索引单链表，分配物理内存
      for (std::size_t i = 0; i < num_elems - 1; ++i) {
          // 随着 i 的增长，每跨越 4KB，就会触发一次缺页中断。
        store_[i].next_free_index_ = i + 1;
      }
      // 最后一个节点的 next 指向一个无效值代表链表尾部
      store_[num_elems - 1].next_free_index_ = 0xFFFFFFFF;
    }

    template<typename... Args>
    T *allocate(Args&&... args) noexcept {
      ASSERT(next_free_index_ != 0xFFFFFFFF, "Memory Pool out of space.");
      
      // O(1) 弹栈
      uint32_t index = next_free_index_;
      next_free_index_ = store_[index].next_free_index_;

      // 在准确的内存位置上原地构造对象
      T *ret = &store_[index].object_;
      return new(ret) T(std::forward<Args>(args)...); 
    }

    // 去掉 const，语义更严谨
    auto deallocate(T *elem) noexcept {
      // 1. O(1) 反向寻址，算出 index
      const auto elem_index = (reinterpret_cast<const Block *>(elem) - &store_[0]);
      ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store_.size(), 
             "Element being deallocated does not belong to this Memory pool.");

      // 2. 显式调用析构函数，安全清理 T 内部可能存在的资源：防御性编程
      elem->~T(); 

      // 3. O(1) 压栈：将该内存块的身份从 object_ 切换回 next_free_index_
      store_[elem_index].next_free_index_ = next_free_index_;
      next_free_index_ = static_cast<uint32_t>(elem_index);
    }

    MemPool() = delete;
    MemPool(const MemPool &) = delete;
    MemPool(const MemPool &&) = delete;
    MemPool &operator=(const MemPool &) = delete;
    MemPool &operator=(const MemPool &&) = delete;
  };
}