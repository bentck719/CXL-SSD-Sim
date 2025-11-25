#ifndef FIFO_QUEUE_HH_
#define FIFO_QUEUE_HH_

#include <cstddef>
#include <list>
#include <unordered_map>
#include <optional>
#include <stdexcept>

namespace gem5 {

template <typename KeyType, typename ValueType>
class FIFOQueue {
private:
  using Element = std::pair<KeyType, ValueType>;
  using ListIterator = typename std::list<Element>::iterator;

  size_t max_size_;
  std::list<Element> fifo_order_;
  
  std::unordered_map<KeyType, ListIterator> map_;

public:
  explicit FIFOQueue(size_t max_size) : max_size_(max_size) {
    if (max_size == 0) {
      throw std::invalid_argument("FIFOQueue max_size > 0");
    }
  }

  FIFOQueue(const FIFOQueue&) = delete;
  FIFOQueue& operator = (const FIFOQueue&) = delete;

  size_t Size() const { return map_.size(); }
  bool IsFull() const { return map_.size() >= max_size_; }
  bool IsEmpty() const { return map_.empty(); }
  bool Contains(const KeyType &key) const { return map_.count(key); }

  // 取得 Value 指標 (Element.second)
  ValueType* Get(const KeyType& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) return nullptr;
    // map_it->second 是 list iterator
    // *(map_it->second) 是 Element (Pair)
    // .second 才是 Value
    return &((map_it->second)->second);
  }

  // 插入元素
  void Insert(const KeyType& key, const ValueType& value) {
    if (Contains(key)) return; 
    
    if (IsFull()) {
        PopFront();
    }

    // 這裡會建立一個 pair {key, value} 放入 list
    fifo_order_.push_back({key, value});
    map_[key] = std::prev(fifo_order_.end());
  }

  // 移除並回傳最舊的 Value，同時清理 Map
  ValueType PopFront() {
    if (IsEmpty()) throw std::runtime_error("PopFront on empty queue");
    
    // 1. 取得 List 頭部的 Key 與 Value
    // 這裡 fifo_order_.front() 回傳的是 Element (Pair)
    Element victim_pair = fifo_order_.front();
    KeyType key = victim_pair.first;

    // 2. 使用 Key 從 Map 中移除 Iterator
    map_.erase(key);
    
    // 3. 從 List 中移除
    fifo_order_.pop_front();
    
    return victim_pair.second;
  }

  // 偷看最舊的元素 (Element.second)
  ValueType& PeekFront() {
      if (IsEmpty()) throw std::runtime_error("PeekFront on empty queue");
      return fifo_order_.front().second;
  }

  // 主動移除指定 Key 的元素
  std::optional<ValueType> Remove(const KeyType& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) return std::nullopt;
    
    // map_it->second 是 list iterator，指向 pair
    ValueType val = (map_it->second)->second;
    
    // 清理 List 和 Map
    fifo_order_.erase(map_it->second);
    map_.erase(map_it);
    
    return val;
  }
  
  void Clear() {
      fifo_order_.clear();
      map_.clear();
  }
};

}

#endif // FIFO_QUEUE_HH_
