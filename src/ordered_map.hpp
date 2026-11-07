#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace csy {

template <class V>
class OrderedMap {
    struct Entry {
        std::pair<std::string, V> kv;
        bool dead = false;
        Entry(const std::string& k, V v) : kv(k, std::move(v)) {}
    };

    template <bool Const>
    class Iter {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<std::string, V>;
        using difference_type = std::ptrdiff_t;
        using pointer = typename std::conditional<Const, const value_type*, value_type*>::type;
        using reference = typename std::conditional<Const, const value_type&, value_type&>::type;
        using EntryPtr = typename std::conditional<Const, const Entry*, Entry*>::type;

        Iter() = default;
        Iter(EntryPtr cur, EntryPtr end) : cur_(cur), end_(end) { skip(); }
        template <bool C = Const, class = typename std::enable_if<C>::type>
        Iter(const Iter<false>& other) : cur_(other.cur_), end_(other.end_) {}

        reference operator*() const { return cur_->kv; }
        pointer operator->() const { return &cur_->kv; }
        Iter& operator++() { ++cur_; skip(); return *this; }
        Iter operator++(int) { Iter t = *this; ++*this; return t; }
        bool operator==(const Iter& o) const { return cur_ == o.cur_; }
        bool operator!=(const Iter& o) const { return cur_ != o.cur_; }

    private:
        friend class OrderedMap;
        friend class Iter<!Const>;
        void skip() { while (cur_ != end_ && cur_->dead) ++cur_; }
        EntryPtr cur_ = nullptr;
        EntryPtr end_ = nullptr;
    };

public:
    using value_type = std::pair<std::string, V>;
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    iterator begin() { return iterator(entries_.data(), entries_.data() + entries_.size()); }
    iterator end() { return iterator(entries_.data() + entries_.size(), entries_.data() + entries_.size()); }
    const_iterator begin() const { return const_iterator(entries_.data(), entries_.data() + entries_.size()); }
    const_iterator end() const { return const_iterator(entries_.data() + entries_.size(), entries_.data() + entries_.size()); }

    size_t size() const { return entries_.size() - dead_; }
    bool empty() const { return size() == 0; }
    void reserve(size_t n) {
        entries_.reserve(n);
        size_t want = 16;
        while (want < n * 2 + 2) want *= 2;
        if (want > slots_.size()) rebuild_index(want);
    }
    void clear() { entries_.clear(); slots_.clear(); slots_.shrink_to_fit(); used_ = 0; dead_ = 0; }

    size_t count(const std::string& key) const { return locate(key, hash_key(key)) != kNone ? 1 : 0; }
    bool contains(const std::string& key) const { return locate(key, hash_key(key)) != kNone; }

    iterator find(const std::string& key) {
        const size_t at = locate(key, hash_key(key));
        if (at == kNone) return end();
        return iterator(entries_.data() + at, entries_.data() + entries_.size());
    }
    const_iterator find(const std::string& key) const {
        const size_t at = locate(key, hash_key(key));
        if (at == kNone) return end();
        return const_iterator(entries_.data() + at, entries_.data() + entries_.size());
    }

    V& operator[](const std::string& key) {
        const uint32_t h = hash_key(key);
        const size_t at = locate(key, h);
        if (at != kNone) return entries_[at].kv.second;
        add_slot(h, entries_.size());
        entries_.emplace_back(key, V());
        return entries_.back().kv.second;
    }

    const V& at(const std::string& key) const {
        const size_t at = locate(key, hash_key(key));
        if (at == kNone) throw std::out_of_range("key not found");
        return entries_[at].kv.second;
    }

    std::pair<iterator, bool> emplace(const std::string& key, V value) {
        const uint32_t h = hash_key(key);
        const size_t at = locate(key, h);
        if (at != kNone) {
            return {iterator(entries_.data() + at, entries_.data() + entries_.size()), false};
        }
        add_slot(h, entries_.size());
        entries_.emplace_back(key, std::move(value));
        return {iterator(entries_.data() + entries_.size() - 1, entries_.data() + entries_.size()), true};
    }

    bool erase(const std::string& key) {
        const uint32_t h = hash_key(key);
        const size_t slot = locate_slot(key, h);
        if (slot == kNone) return false;
        const size_t pos = slots_[slot].index - 1;
        slots_[slot].index = kTomb;
        if (pos + 1 == entries_.size()) {
            entries_.pop_back();
            while (!entries_.empty() && entries_.back().dead) {
                entries_.pop_back();
                --dead_;
            }
            return true;
        }
        Entry& e = entries_[pos];
        e.dead = true;
        e.kv.second = V();
        std::string().swap(e.kv.first);
        ++dead_;
        if (dead_ >= 32 && dead_ * 2 > entries_.size()) compact();
        return true;
    }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);
    static constexpr uint32_t kTomb = 0xFFFFFFFFu;

    struct Slot {
        uint32_t hash = 0;
        uint32_t index = 0;
    };

    static uint32_t hash_key(const std::string& key) {
        const size_t h = std::hash<std::string>{}(key);
        return static_cast<uint32_t>(h ^ (h >> 32));
    }

    size_t locate_slot(const std::string& key, uint32_t h) const {
        if (slots_.empty()) return kNone;
        const size_t mask = slots_.size() - 1;
        for (size_t at = h & mask;; at = (at + 1) & mask) {
            const Slot& slot = slots_[at];
            if (slot.index == 0) return kNone;
            if (slot.index != kTomb && slot.hash == h && entries_[slot.index - 1].kv.first == key) return at;
        }
    }

    size_t locate(const std::string& key, uint32_t h) const {
        const size_t slot = locate_slot(key, h);
        return slot == kNone ? kNone : static_cast<size_t>(slots_[slot].index - 1);
    }

    void add_slot(uint32_t h, size_t entry_index) {
        if ((used_ + 1) * 2 > slots_.size()) {
            size_t want = 16;
            while (want < (size() + 1) * 4) want *= 2;
            rebuild_index(want);
        }
        const size_t mask = slots_.size() - 1;
        size_t at = h & mask;
        while (slots_[at].index != 0 && slots_[at].index != kTomb) at = (at + 1) & mask;
        if (slots_[at].index == 0) ++used_;
        slots_[at].hash = h;
        slots_[at].index = static_cast<uint32_t>(entry_index + 1);
    }

    void rebuild_index(size_t capacity) {
        std::vector<Slot> fresh(capacity);
        const size_t mask = capacity - 1;
        used_ = 0;
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].dead) continue;
            const uint32_t h = hash_key(entries_[i].kv.first);
            size_t at = h & mask;
            while (fresh[at].index != 0) at = (at + 1) & mask;
            fresh[at].hash = h;
            fresh[at].index = static_cast<uint32_t>(i + 1);
            ++used_;
        }
        slots_.swap(fresh);
    }

    void compact() {
        size_t out = 0;
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].dead) continue;
            if (out != i) entries_[out] = std::move(entries_[i]);
            out++;
        }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(out), entries_.end());
        dead_ = 0;
        size_t want = 16;
        while (want < (entries_.size() + 1) * 4) want *= 2;
        rebuild_index(want);
    }

    std::vector<Entry> entries_;
    std::vector<Slot> slots_;
    size_t used_ = 0;
    size_t dead_ = 0;
};

}
