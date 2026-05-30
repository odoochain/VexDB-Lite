/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef GRAPH_INDEX_CLUSTER_H
#define GRAPH_INDEX_CLUSTER_H

#include <vtl/vector>
#include <vtl/span>
#include <vtl/optional>
#include <vtl/disk_container/plain_store.hpp>

#include "pg_compat.h"
#include "graph_index/graph_index_struct.h"
#include <mutex>

/* use id can also directly get the element */
struct GraphIndexElementBase {
    /*
     * Flag bit layout:
     *   bit 0:     deleted flag (0x01)
     *   bit 1:     async insert point (0x02)
     *   bits 2-5:  ntids count (0x0f << 2, max 15)
     *   bit 6:     extended (0x40)
     *   bit 7:     double extended (0x80)
     */
    uint8 flag;
    ItemPointerData heaptids[GRAPH_INDEX_MAX_HEAPTIDS];

    const uint8 &get_flag() const { return flag; }
    uint8 &get_flag() { return flag; }
    const ItemPointer get_heaptids() const { return (const ItemPointer)heaptids; }
    ItemPointer get_heaptids() { return heaptids; }

    void init() { get_flag() = 0; }
    bool is_deleted() const { return get_flag() & 0x01; }
    void set_deleted() { get_flag() |= 0x01; }
    bool is_async() const { return get_flag() & 0x02; }
    void set_async(bool is_async) { is_async ? get_flag() |= 0x02 : get_flag() &= ~0x02; }
    bool is_extended() const { return get_flag() & 0x40; }
    void set_extended() { get_flag() |= 0x40; }
    bool is_double_extended() const { return get_flag() & 0x80; }
    void set_double_extended(bool v = true)
    {
        if (v) {
            get_flag() |= 0x80;
        } else {
            get_flag() &= 0x7f;
        }
    }
    uint8 ntids() const { return (get_flag() >> 2) & 0x0f; }
    /* make sure you know what is going on when you call this func */
    void set_ntids(uint8 n) { get_flag() = (get_flag() & 0xc3) | (n << 2); }
    bool empty() const { return ntids() == 0; }

protected:
    using PlainStore = disk_container::PlainStore;
};

struct PointExtensionContext {
    PointExtensionContext(Relation index, BlockNumber ps_blkno, bool need_wal,
                          bool concurrent = false)
    {
        if (!concurrent) {
            ps_.emplace(index, ps_blkno, need_wal);
        }
    }
    uint32 code_size{0};
    bool has_ps() const { return ps_.has_value(); }
    disk_container::PlainStore &ps() { return *ps_; }

    /* destroy() runs at end-of-build from the leader thread after worker
     * join, so no contention here. */
    void destroy() { ps_.destroy(); }
private:
    Optional<disk_container::PlainStore> ps_{};
};

struct GraphIndexPoint : public GraphIndexElementBase {
    using Data = ItemPointerData;

    GraphIndexPoint() = default; /* for GraphIndexCluster inheritance */
    GraphIndexPoint(PointExtensionContext &ctx, Span<const Data> data, bool is_async = false)
    {
        Assert(!data.empty());
        init();
        set_ntids(1u);
        heaptids[0] = data[0];
        if (is_async) {
            set_async(true);
        }
        if (data.size() > 1) {
            insert_tid(ctx, data.subspan(1));
        }
    }
    
    GraphIndexPoint(const ItemPointerData &tid)
    {
        init();
        set_ntids(1u);
        heaptids[0] = tid;
    }

    static constexpr uint32 fullsize(PointExtensionContext &)
        { return tid_page_cap * GRAPH_INDEX_MAX_HEAPTIDS; }
    static constexpr uint32 max_insert_size(PointExtensionContext &)
        { return PlainStore::max_size / sizeof(ItemPointerData); }
protected:
    static constexpr uint32 tid_page_cap = PlainStore::max_size / sizeof(Data);
public:
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data, bool &overwriten)
    {
        Assert(!data.empty());
        if (empty()) {
            /* 
             * is empty and searchable, 
             * only if the index is under vacuum and will be mark deleted later
             */
            return false;
        }

        Assert(data.size_bytes() <= PlainStore::max_size);
        uint8 nentry = ntids();
        if (!is_extended() && nentry + data.size() <= GRAPH_INDEX_MAX_HEAPTIDS) {
            for (const Data &d : data) {
                get_heaptids()[nentry++] = d;
            }
            set_ntids(nentry);
            overwriten = true;
            return true;
        }
        if (!ctx.has_ps()) {
            return false;
        }
        bool res = true;
        if (!is_extended()) {
            set_extended();
            set_ntids(1u);
            Assert(tid_page_cap >= nentry + data.size());
            Data temp_tids[nentry + data.size()];
            for (uint8 i = 0; i < nentry; ++i) {
                temp_tids[i] = get_heaptids()[i];
            }
            Data *temp_cur = temp_tids + nentry;
            for (const Data &d : data) {
                *temp_cur = d;
                ++temp_cur;
            }
            get_heaptids()[0] = ctx.ps().put(temp_tids, sizeof(Data) * (nentry + data.size()));
            overwriten = true;
        } else {
            Assert(nentry > 0);
            auto key = get_heaptids()[nentry - 1u];
            size_t old_size, new_size = 0;
            char buf[PlainStore::max_size];
            ctx.ps().get(key, [&](const void *in_data, Size size) {
                old_size = size;
                if (size + sizeof(Data) > PlainStore::max_size) {
                    return;
                }
                new_size = std::min(PlainStore::max_size, size + data.size_bytes());
                memcpy(buf, in_data, size);
            });
            if (new_size == 0) {
                if (likely(nentry < GRAPH_INDEX_MAX_HEAPTIDS)) {
                    get_heaptids()[nentry] = ctx.ps().put(data.data(), data.size_bytes());
                    set_ntids(nentry + 1u);
                    overwriten = true;
                } else {
                    res = false;
                }
            } else {
                Data *cur = (Data *)(buf + old_size);
                size_t space = (PlainStore::max_size - old_size) / sizeof(Data);
                size_t nnew = std::min(space, data.size());
                if (nnew < data.size() && unlikely(nentry >= GRAPH_INDEX_MAX_HEAPTIDS)) {
                    return false;
                }
                for (const Data &d : data.subspan(0, nnew)) {
                    *cur = d;
                    ++cur;
                }
                get_heaptids()[nentry - 1u] = ctx.ps().set(key, buf, new_size);
                overwriten = ItemPointerEquals(&key, get_heaptids() + nentry - 1);
                if (nnew < data.size()) {
                    auto s = data.subspan(nnew);
                    get_heaptids()[nentry] = ctx.ps().put(s.data(), s.size_bytes());
                    set_ntids(nentry + 1u);
                    overwriten = true;
                }
            }
        }
        return res;
    }
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data)
    {
        bool unused;
        return insert_tid(ctx, data, unused);
    }
    uint32 get_tids(Vector<Data> &tids, PointExtensionContext &ctx) const
    {
        uint32 res;
        if (!is_extended()) {
            res = ntids();
            tids.push_back(get_heaptids(), get_heaptids() + res);
            return res;
        }
        Assert(ctx.has_ps());
        uint8 nkey = ntids();
        res = 0;
        for (uint8 i = 0; i < nkey; ++i) {
            ctx.ps().get(get_heaptids()[i], [&](const void *in_data, Size size) {
                const ItemPointer data = (const ItemPointer)in_data;
                uint32 ndata = size / sizeof(Data);
                tids.push_back(data, data + ndata);
                res += ndata;
            });
        }
        return res;
    }
    template <typename F>
    void apply_on_tids(PointExtensionContext &ctx, F &&f) const
    {
        bool stop = false;
        const auto do_apply = [&](const ItemPointer data, uint16 idx) {
            while (idx != 0) {
                --idx;
                if (f(data[idx])) {
                    stop = true;
                    return;
                }
            }
        };
        if (!is_extended()) {
            do_apply(get_heaptids(), ntids());
            return;
        }
        Assert(ctx.has_ps());
        for (uint8 idx = ntids(); idx != 0 && !stop;) {
            --idx;
            ctx.ps().get(get_heaptids()[idx], [&](const void *data, Size size) {
                do_apply((const ItemPointer)data, size / sizeof(Data));
            });
        }
    }
    uint32 actual_ntids(PointExtensionContext &ctx) const
    {
        const uint8 nkey = ntids();
        if (!is_extended()) {
            return nkey;
        }
        Assert(ctx.has_ps());
        uint32 res = 0;
        for (uint8 i = 0; i < nkey; ++i) {
            ctx.ps().get(get_heaptids()[i], [&](const void *, Size size) {
                res += size / sizeof(Data);
            });
        }
        return res;
    }
    template <typename F>
    uint32 vacuum_tids(F &&filter, PointExtensionContext &ctx, bool &dirty)
    {
        dirty = false;
        const uint8 ntid = ntids();
        if (ntid == 0) {
            return 0;
        }

        if (!is_extended()) {
            uint8 start_idx = 0;
            for (uint8 i = 0; i < ntid; ++i) {
                if (filter(get_heaptids()[i])) {
                    continue;
                }
                get_heaptids()[start_idx] = get_heaptids()[i];
                ++start_idx;
            }
            dirty = start_idx != ntid;
            set_ntids(start_idx);
            return ntid - start_idx;
        }

        Assert(ctx.has_ps());
        ItemPointer buf = (ItemPointer)palloc(sizeof(Data) * tid_page_cap * 2);
        uint32 nremoved = 0;
        uint32 nremain = 0;
        uint8 start_idx = 0;
        for (uint8 i = 0; i < ntid; ++i) {
            ctx.ps().get(get_heaptids()[i], [&](const void *in_data, Size size) -> void {
                uint32 ndata = size / sizeof(Data);
                const ItemPointer data = (const ItemPointer)in_data;
                for (uint32 i = 0; i < ndata; ++i) {
                    if (filter(data[i])) {
                        ++nremoved;
                        continue;
                    }
                    buf[nremain] = data[i];
                    ++nremain;
                }
            });
            if (nremain >= tid_page_cap) {
                if (nremoved > 0) {
                    auto k = ctx.ps().set(get_heaptids()[start_idx], buf, tid_page_cap * sizeof(Data));
                    if (ItemPointerEquals(&k, get_heaptids() + start_idx)) {
                        dirty = true;
                        get_heaptids()[start_idx] = k;
                    }
                }
                ++start_idx;
                nremain -= tid_page_cap;
                memcpy(buf, buf + tid_page_cap, nremain * sizeof(Data));
            }
        }

        if (nremoved > 0 && nremain > 0) {
            Assert(start_idx < ntid);
            auto k = ctx.ps().set(get_heaptids()[start_idx], buf, nremain * sizeof(Data));
            if (ItemPointerEquals(&k, get_heaptids() + start_idx)) {
                dirty = true;
                get_heaptids()[start_idx] = k;
            }
            ++start_idx;
        }
        pfree(buf);

        if (start_idx < ntid) {
            set_ntids(start_idx);
            for (uint8 i = start_idx; i < ntid; ++i) {
                ctx.ps().erase(get_heaptids()[i]);
            }
            dirty = true;
        }

        return nremoved;
    }
};

bool try_set_under_redistrib(Relation index, uint32 id);
void reset_under_redistrib(Relation index, uint32 id);

#endif /* GRAPH_INDEX_CLUSTER_H */
