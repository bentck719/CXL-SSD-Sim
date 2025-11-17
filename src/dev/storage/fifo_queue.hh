#ifndef FIFO_QUEUE_HH_
#define FIFO_QUEUE_HH_

#include <cstddef>   
#include <list>       
#include <unordered_map>
#include <optional> 
#include <stdexcept>

template <typename KeyType, typename ValueType>
class FIFOQueue {
private:
  using ListIterator = typename std::list<ValueType>::iterator;

  size_t max_size_;
  std::list<ValueType> fifo_order_;
  std::unordered_map<KeyType, ListIterator> map_;

public:
  explicit FIFOQueue(size_t max_size) : max_size_(max_size) {
    if (max_size == 0) {
      throw std::invalid_argument("FIFOQueue max_size > 0");
    }
  }

  FIFOQueue(const FIFOQueue&) = delete;
  FIFOQueue& operator = (const FIFOQueue&) = delete;

  size_t Size() const { 
    return map_.size(); 
  }

  bool IsFull() const { 
    return map_.size() >= max_size_; 
  }

  bool IsEmpty() const {
    return map_.empty();
  }

  bool Contains(const KeyType &key) const {
    return map_.count(key);
  }

  ValueType PopFront() {
    ValueType victim = fifo_order_.front();
    map_.erase(victim.logical_frame); 
    fifo_order_.pop_front();
    return victim;
  }

  void Insert(const KeyType& key, const ValueType& value) {
    // 1. 檢查是否已存在 (快取命中) - O(1)
    if (Contains(key)) {
      return std::nullopt;
    }

    // 2. 快取未命中，準備插入
    std::optional<ValueType> victim = std::nullopt;

    // 3. 檢查是否已滿 - O(1)
    if (IsFull()) {
      victim = PopFront();
    }

    // 4. 插入新元素
    fifo_order_.push_back(value);
    map_[key] = std::prev(fifo_order_.end());

    return victim;
  }

  ValueType* Get(const KeyType& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) {
        return nullptr;
    }
    return &(*(map_it->second));
  }

  std::optional<ValueType> Remove(const KeyType& key) {
    auto map_it = map_.find(key);
    if (map_it == map_.end()) {
        return std::nullopt;
    }
    
    ValueType node = *(map_it->second); // 拷貝一份
    fifo_order_.erase(map_it->second);  // 從 list 移除 (O(1))
    map_.erase(map_it);                 // 從 map 移除 (O(1))
    return node;
  }

  void Clear() {
    fifo_order_.clear();
    map_.clear();
  }
}

#endif // FIFO_QUEUE_HH_