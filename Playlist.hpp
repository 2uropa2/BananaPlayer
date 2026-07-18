#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <random>
#include <algorithm>

class Playlist {
public:
    enum class RepeatMode { None, One, All };

    void add(const std::string& path) {
        items_.push_back(path);
        if (items_.size() == 1) index_ = 0;
        if (shuffle_) rebuildShuffleOrder();
    }
    // Bulk-add without rebuilding the shuffle order on every item. Callers
    // that add many paths at once (Open Folder, M3U load, CLI multi-file)
    // should prefer this over add()-in-a-loop to keep folder scans O(N).
    void addBulk(const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        bool was_empty = items_.empty();
        items_.insert(items_.end(), paths.begin(), paths.end());
        if (was_empty) index_ = 0;
        if (shuffle_) rebuildShuffleOrder();
    }
    void clear() {
        items_.clear();
        shuffle_order_.clear();
        index_ = 0;
    }
    void setCurrent(size_t i) {
        if (i < items_.size()) index_ = i;
    }
    bool next() {
        if (items_.empty()) return false;
        if (shuffle_) {
            // Walk shuffle_order_ from the position of index_ forward.
            size_t k = shufflePosOf(index_);
            if (k + 1 < shuffle_order_.size()) {
                index_ = shuffle_order_[k + 1];
                return true;
            }
            return false;
        }
        if (index_ + 1 < items_.size()) { ++index_; return true; }
        return false;
    }
    bool prev() {
        if (items_.empty()) return false;
        if (shuffle_) {
            size_t k = shufflePosOf(index_);
            if (k > 0) { index_ = shuffle_order_[k - 1]; return true; }
            return false;
        }
        if (index_ > 0) { --index_; return true; }
        return false;
    }
    bool hasNext() const {
        if (items_.empty()) return false;
        if (shuffle_) return shufflePosOf(index_) + 1 < shuffle_order_.size();
        return index_ + 1 < items_.size();
    }
    bool hasPrev() const {
        if (items_.empty()) return false;
        if (shuffle_) return shufflePosOf(index_) > 0;
        return index_ > 0;
    }
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    size_t index() const { return index_; }
    const std::string& current() const {
        static const std::string kEmpty;
        return items_.empty() ? kEmpty : items_[index_];
    }
    const std::vector<std::string>& items() const { return items_; }
    void remove(size_t i) {
        if (i >= items_.size()) return;
        // Track whether the removed item was the currently-playing one so
        // callers can react (e.g. advance the player, or leave the current
        // file playing after the playlist entry disappears).
        bool was_current = (i == index_);
        items_.erase(items_.begin() + i);
        if (items_.empty()) {
            index_ = 0;
        } else {
            // Keep `index_` pointing at the same logical item when possible.
            if (i < index_)       --index_;
            else if (was_current && index_ >= items_.size())
                                  index_ = items_.size() - 1;
        }
        if (shuffle_) rebuildShuffleOrder();
    }
    // Swap adjacent entries. Adjusts `index_` so the currently-playing
    // item stays highlighted after the move. No-op at boundaries.
    bool moveUp(size_t i) {
        if (i == 0 || i >= items_.size()) return false;
        std::swap(items_[i], items_[i - 1]);
        if (index_ == i)          index_ = i - 1;
        else if (index_ == i - 1) index_ = i;
        if (shuffle_) rebuildShuffleOrder();
        return true;
    }
    bool moveDown(size_t i) {
        if (i + 1 >= items_.size()) return false;
        std::swap(items_[i], items_[i + 1]);
        if (index_ == i)          index_ = i + 1;
        else if (index_ == i + 1) index_ = i;
        if (shuffle_) rebuildShuffleOrder();
        return true;
    }
    // Move the item at `i` to the front / back of the list. Adjusts
    // `index_` so the currently-playing item keeps pointing at the same
    // file across the reorder.
    bool moveToTop(size_t i) {
        if (i == 0 || i >= items_.size()) return false;
        std::string item = std::move(items_[i]);
        items_.erase(items_.begin() + i);
        items_.insert(items_.begin(), std::move(item));
        if (index_ == i)      index_ = 0;
        else if (index_ < i)  ++index_;   // shifted down by the insert
        if (shuffle_) rebuildShuffleOrder();
        return true;
    }
    bool moveToBottom(size_t i) {
        if (i + 1 >= items_.size()) return false;
        std::string item = std::move(items_[i]);
        items_.erase(items_.begin() + i);
        items_.push_back(std::move(item));
        if (index_ == i)      index_ = items_.size() - 1;
        else if (index_ > i)  --index_;   // shifted up by the erase
        if (shuffle_) rebuildShuffleOrder();
        return true;
    }

    RepeatMode repeatMode() const { return repeat_; }
    void setRepeatMode(RepeatMode m) { repeat_ = m; }
    void cycleRepeatMode() {
        switch (repeat_) {
            case RepeatMode::None: repeat_ = RepeatMode::One; break;
            case RepeatMode::One:  repeat_ = RepeatMode::All; break;
            case RepeatMode::All:  repeat_ = RepeatMode::None; break;
        }
    }

    bool shuffle() const { return shuffle_; }
    void setShuffle(bool v) {
        if (shuffle_ == v) return;
        shuffle_ = v;
        if (shuffle_) rebuildShuffleOrder();
        else          shuffle_order_.clear();
    }

    // Force a new shuffle order (used when the user re-enables shuffle or
    // finishes an "All" cycle and needs to reshuffle for the next round).
    void reshuffle() {
        if (shuffle_) rebuildShuffleOrder();
    }

    // For "Repeat All": jump index to first item in the current traversal
    // order (either linear or shuffled). Returns true if we moved.
    bool wrapToStart() {
        if (items_.empty()) return false;
        if (shuffle_) {
            if (!shuffle_order_.empty()) { index_ = shuffle_order_.front(); return true; }
            return false;
        }
        index_ = 0;
        return true;
    }

private:
    void rebuildShuffleOrder() {
        shuffle_order_.resize(items_.size());
        for (size_t i = 0; i < items_.size(); ++i) shuffle_order_[i] = i;
        std::mt19937 rng{ std::random_device{}() };
        std::shuffle(shuffle_order_.begin(), shuffle_order_.end(), rng);
        // Move current index_ to the front so playback continues with it,
        // then shuffle the rest.
        auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), index_);
        if (it != shuffle_order_.end() && it != shuffle_order_.begin()) {
            std::iter_swap(shuffle_order_.begin(), it);
        }
    }
    size_t shufflePosOf(size_t items_idx) const {
        for (size_t k = 0; k < shuffle_order_.size(); ++k) {
            if (shuffle_order_[k] == items_idx) return k;
        }
        return 0;
    }

    std::vector<std::string> items_;
    size_t index_ = 0;
    RepeatMode repeat_ = RepeatMode::None;
    bool shuffle_ = false;
    std::vector<size_t> shuffle_order_;  // permutation of items_ indices
};
