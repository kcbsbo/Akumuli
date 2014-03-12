/*
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <thread>
#include "akumuli_def.h"
#include "cache.h"
#include "util.h"

namespace Akumuli {

// Generation ---------------------------------

Sequence::Sequence(size_t max_size) noexcept
    : capacity_(max_size)
{
}

Sequence::Sequence(Sequence const& other)
    : capacity_(other.capacity_)
    , data_(other.data_)
{
}

Sequence& Sequence::operator = (Sequence const& other) {
    capacity_ = other.capacity_.load();
    data_ = other.data_;
}

// This method must be called from the same thread
int Sequence::add(TimeStamp ts, ParamId param, EntryOffset  offset) noexcept {
    auto key = std::make_tuple(ts, param);

    std::unique_lock<std::mutex> lock(obj_mtx_, std::defer_lock)
                               , tmp_lock(tmp_mtx_, std::defer_lock);

    if (lock.try_lock()) {
        if (tmp_lock.try_lock()) {
            for(auto const& tup: temp_) {
                auto tkey = std::make_tuple(std::get<0>(tup), std::get<1>(tup));
                data_.insert(std::make_pair(tkey, std::get<2>(tup)));
            }
            tmp_lock.unlock();
        }
        data_.insert(std::make_pair(key, offset));
    } else {
        tmp_lock.lock();
        temp_.emplace_back(ts, param, offset);
    }
    return AKU_WRITE_STATUS_SUCCESS;
}

void Sequence::search(Caller& caller, InternalCursor* cursor, SingleParameterSearchQuery const& query) const noexcept {

    bool forward = query.direction == AKU_CURSOR_DIR_FORWARD;
    bool backward = query.direction == AKU_CURSOR_DIR_BACKWARD;

    if (query.upperbound < query.lowerbound  // Right timestamps and
        || !(forward ^ backward)             // right direction constant
    ) {
        cursor->set_error(caller, AKU_EBAD_ARG);
        return;
    }

    if (backward)
    {
        auto tskey = query.upperbound;
        auto idkey = (ParamId)~0;
        auto key = std::make_tuple(tskey, idkey);
        auto citer = data_.upper_bound(key);
        auto last_key = std::make_tuple(query.lowerbound, 0);

        std::unique_lock<std::mutex> lock(obj_mtx_);
        while(true) {
            auto& curr_key = citer->first;
            if (std::get<0>(curr_key) <= std::get<0>(last_key)) {
                break;
            }
            if (std::get<1>(curr_key) == query.param && std::get<0>(curr_key) <= tskey) {
                cursor->put(caller, citer->second);
            }
            if (citer == data_.begin()) {
                break;
            }
            citer--;
        }
    }
    else
    {
        auto tskey = query.lowerbound;
        auto idkey = (ParamId)0;
        auto key = std::make_tuple(tskey, idkey);
        auto citer = data_.lower_bound(key);
        auto last_key = std::make_tuple(query.upperbound, ~0);

        std::unique_lock<std::mutex> lock(obj_mtx_);
        while(citer != data_.end()) {
            auto& curr_key = citer->first;
            if (std::get<0>(curr_key) >= std::get<0>(last_key)) {
                break;
            }
            if (std::get<1>(curr_key) == query.param) {
                cursor->put(caller, citer->second);
            }
            citer++;
        }
    }
    cursor->complete(caller);
}

size_t Sequence::size() const noexcept {
    return data_.size();
}

Sequence::MapType::const_iterator Sequence::begin() const {
    return data_.begin();
}

Sequence::MapType::const_iterator Sequence::end() const {
    return data_.end();
}


// Bucket -------------------------------------

Bucket::Bucket(int n, size_t max_size, int64_t baseline)
    : rrindex_(0)
    , state(0)
    , baseline(baseline)
{
    const int NUMCPU = std::thread::hardware_concurrency();
    assert (n == NUMCPU);
    // TODO: remove first c-tor parameter, use hardware_concurrency always
    for (int i = 0; i < n; i++) {
        seq_.emplace_back(max_size);
    }
}

Bucket::Bucket(Bucket const& other)
    : seq_(other.seq_)
    , baseline(other.baseline)
    , rrindex_(0)
{
    state.store(other.state);
}

Bucket& Bucket::operator = (Bucket const& other) {
    if (&other == this)
        return *this;
    seq_ = other.seq_;
    baseline = other.baseline;
    state.store(other.state);
    return *this;
}

int Bucket::add(TimeStamp ts, ParamId param, EntryOffset  offset) noexcept {
    int cpuix = aku_getcpu();
    int index = cpuix % seq_.size();
    return seq_[index].add(ts, param, offset);
}

void Bucket::search(Caller &caller, InternalCursor* cursor, SingleParameterSearchQuery* params) const noexcept {
    for(Sequence const& seq: seq_) {
        seq.search(caller, cursor, params);
    }
}

typedef Sequence::MapType::const_iterator iter_t;

static bool less_than(iter_t lhs, iter_t rhs, PageHeader* page) {
    auto lentry = page->read_entry(lhs->second),
         rentry = page->read_entry(rhs->second);

    auto lkey = std::make_tuple(lentry->time, lentry->param_id),
         rkey = std::make_tuple(rentry->time, rentry->param_id);

    return lkey < rkey;
}

int Bucket::merge(Caller& caller, InternalCursor *cur, PageHeader* page) const noexcept {

    if (state.load() == 0) {
        return AKU_EBUSY;
    }

    // fastpath for empty case
    if (rrindex_.load() == 0) {
        return AKU_SUCCESS;
    }

    size_t n = seq_.size();

    // Init
    iter_t iter[n], end[n];
    for (size_t i = 0u; i < n; i++) {
        iter[i] = seq_[i].begin();
        end[i] = seq_[i].end();
    }

    // Merge

    if (n > 1) {
        int next_min = 0;
        while(true) {
            int min = next_min;
            int op_cnt = 0;
            for (int i = 0; i < n; i++) {
                if (i == min) continue;
                if (iter[i] != end[i]) {
                    if (less_than(iter[i], iter[min], page)) {
                        next_min = min;
                        min = i;
                    } else {
                        next_min = i;
                    }
                    op_cnt++;
                }
            }
            if (op_cnt == 0)
                break;
            auto offset = iter[min]->second;
            cur->put(caller, offset);
            std::advance(iter[min], 1);
        }
    } else {
        while(iter[0] != end[0]) {
            auto offset = iter[0]->second;
            cur->put(caller, offset);
            std::advance(iter[0], 1);
        }
    }

    assert(iter == end);

    return AKU_SUCCESS;
}

// Cache --------------------------------------

Cache::Cache(TimeDuration ttl, size_t max_size)
    : ttl_(ttl)
    , max_size_(max_size)
    , baseline_()
{
    // Cache prepopulation
    // Buckets allocated using std::deque<Bucket> for greater locality
    for (int i = 0; i < AKU_CACHE_POPULATION; i++) {
        buckets_.emplace_back(bucket_size_, max_size_, baseline_ + i);
        auto& s = buckets_.back();
        free_list_.push_back(s);
    }

    allocate_from_free_list(AKU_LIMITS_MAX_CACHES, baseline_);

    // We need to calculate shift width. So, we got ttl in some units
    // of measure (units of measure that akumuli doesn't know about).
    shift_ = log2(ttl.value);
    if ((1 << shift_) < AKU_LIMITS_MIN_TTL) {
        throw std::runtime_error("TTL is too small");
    }
}

template<class TCont>
Sequence* index2ptr(TCont& cont, int64_t index) noexcept {
    auto begin = cont.begin();
    std::advance(begin, index);
    auto& gen = *begin;
    return &gen;
}

template<class TCont>
Sequence const* index2ptr(TCont const& cont, int64_t index) noexcept {
    auto begin = cont.cbegin();
    std::advance(begin, index);
    auto& gen = *begin;
    return &gen;
}

/** Mark last n live buckets
  * @param TCont container of buckets
  */
template<class TCont>
void mark_last(TCont& container, int n) noexcept {
    auto end = container.end();
    auto begin = container.end();
    std::advance(begin, -1*n);
    for(auto i = begin; i != end; i++) {
        i->state++;
    }
}

void Cache::allocate_from_free_list(int nnew, int64_t baseline)noexcept {
    // TODO: add backpressure to producer to limit memory usage!
    auto n = free_list_.size();
    if (n < nnew) {
        // create new buckets
        auto cnt = nnew - n;
        for (int i = 0; i < cnt; i++) {
            buckets_.emplace_back(bucket_size_, max_size_, baseline + i);  // TODO: init baseline in c-tor
            auto& b = buckets_.back();
            free_list_.push_back(b);
        }
    }
    auto begin = free_list_.begin();
    auto end = free_list_.begin();
    std::advance(end, nnew);
    free_list_.splice(cache_.begin(), cache_, begin, end);
}

int Cache::add_entry_(TimeStamp ts, ParamId pid, EntryOffset offset, size_t* nswapped) noexcept {
    // TODO: set upper limit to ttl_ value to prevent overflow
    auto bucket_index = (ts.value >> shift_);
    auto index = baseline_ - bucket_index;

    // NOTE: If it is less than zero - we need to shift cache.
    // Otherwise we can select existing generation but this can result in late write
    // or overflow.

    Bucket* target = nullptr;
    if (index >= 0) {
        if (index == 0) {
            // shortcut for the most frequent case
            target = &cache_.front();
        }
        else {
            std::lock_guard<std::mutex> lock(lists_mutex_);
            auto it = std::find_if(cache_.begin(), cache_.end(), [=](Bucket const& bucket) {
                return bucket.state == 0 && bucket.baseline == bucket_index;
            });
            if (it == cache_.end()) {
                return AKU_EOVERFLOW;
            }
            target = &*it;
        }
    }
    else {
        // Future write! This must be ammortized across many writes.
        // If this procedure performs often - than we choose too small TTL
        auto count = 0 - index;
        baseline_ = bucket_index;
        index = 0;

        if (count >= AKU_LIMITS_MAX_CACHES) {
            // Move all items to swap
            size_t recycle_cnt = AKU_LIMITS_MAX_CACHES;
            mark_last(cache_, recycle_cnt);
            *nswapped += recycle_cnt;
            allocate_from_free_list(recycle_cnt, baseline_ - recycle_cnt);
        }
        else {
            // Calculate, how many gen-s must be swapped
            auto freeslots = AKU_LIMITS_MAX_CACHES - count;
            size_t curr_cache_size = cache_.size();  // FIXME: must count only buckets with zero state
            if (freeslots < curr_cache_size) {
                size_t recycle_cnt = curr_cache_size - freeslots;
                mark_last(cache_, recycle_cnt);
                *nswapped += recycle_cnt;
            }
            allocate_from_free_list(count, baseline_ - count);
        }
        std::lock_guard<std::mutex> lock(lists_mutex_);
        auto it = std::find_if(cache_.begin(), cache_.end(), [=](Bucket const& bucket) {
            return bucket.state == 0 && bucket.baseline == bucket_index;
        });
        if (it == cache_.end()) {
            return AKU_EGENERAL;
        }
        target = &*it;
    }
    // add to bucket
    return target->add(ts, pid, offset);
}

int Cache::add_entry(const Entry& entry, EntryOffset offset, size_t* nswapped) noexcept {
    return add_entry_(entry.time, entry.param_id, offset, nswapped);
}

int Cache::add_entry(const Entry2& entry, EntryOffset offset, size_t* nswapped) noexcept {
    return add_entry_(entry.time, entry.param_id, offset, nswapped);
}

void Cache::clear() noexcept {
    std::lock_guard<std::mutex> lock((lists_mutex_));
    // TODO: clean individual buckets
    cache_.splice(free_list_.begin(), free_list_, cache_.begin(), cache_.end());
}

int Cache::remove_old(EntryOffset* offsets, size_t size, uint32_t* noffsets) noexcept {
    return AKU_EGENERAL;
}

void Cache::search(SingleParameterSearchQuery* cursor) const noexcept {

    bool forward = cursor->direction == AKU_CURSOR_DIR_FORWARD;
    bool backward = cursor->direction == AKU_CURSOR_DIR_BACKWARD;

//    if (cursor->upperbound < cursor->lowerbound
//        || !(forward ^ backward)
//        || cursor->results == nullptr
//        || cursor->results_cap == 0
//    ) {
//        // Invalid direction or timestamps
//        cursor->state = AKU_CURSOR_COMPLETE;
//        cursor->error_code = AKU_EBAD_ARG;
//        return;
//    }

//    if (cursor->cache_init == 0) {
//        // Init search
//        auto tskey = cursor->upperbound.value;
//        auto idkey = cursor->param;
//        auto key = tskey >> shift_;
//        auto index = baseline_ - key;
//        // PROBLEM: index can change between calls
//        if (index < 0) {
//            // future read
//            index = 0;
//        }
//        cursor->cache_init = 1;
//        cursor->cache_index = 0;
//        cursor->cache_start_id = key;
//    }
    throw std::runtime_error("Not implemented");
}

}  // namespace Akumuli