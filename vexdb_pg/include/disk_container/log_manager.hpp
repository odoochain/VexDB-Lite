#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <disk_container/blockmgr.hpp>
extern "C" {
#include "access/generic_xlog.h"
}

namespace disk_container {

class DiskContainerLogMgr {
public:
    Relation index;

    explicit DiskContainerLogMgr(Relation rel) : index(rel) {}

    template <typename Func>
    void apply(Buffer buf, bool need_wal, bool full_image, Func &&func)
    {
        static_assert(std::is_same_v<decltype(func(BufferGetPage(buf))), bool>,
            "apply() requires the lambda to return bool indicating whether the page was modified");
        if (need_wal) {
            int flags = full_image ? GENERIC_XLOG_FULL_IMAGE : 0;
            GenericXLogState *state = GenericXLogStart(index);
            Page page = GenericXLogRegisterBuffer(state, buf, flags);
            if (func(page)) {
                GenericXLogFinish(state);
            } else {
                GenericXLogAbort(state);
            }
        } else {
            if (func(BufferGetPage(buf))) {
                MarkBufferDirty(buf);
            }
        }
    }

    template <typename Func>
    void apply(PageData &pd, bool need_wal, bool full_image, Func &&func)
    {
        apply(pd.buf, need_wal, full_image, [&](Page page) -> bool {
            return func(PageGetContents(page));
        });
    }

    void xl_extend_newpages(BlockNumber start_blkno, BlockNumber end_blkno)
    {
        log_newpage_range(index, MAIN_FORKNUM, start_blkno, end_blkno, true);
    }
};

} /* namespace disk_container */

#endif /* LOG_MANAGER_H */
