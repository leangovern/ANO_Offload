/* op_kernel/ano_probe_tiling.h — AnoProbe tiling 数据（任务 12-b） */
#ifndef ANO_PROBE_TILING_H
#define ANO_PROBE_TILING_H
#include <stdint.h>
struct AnoProbeTilingData {
    uint32_t sel;   /* 预留（探针模式走 params 张量，不经 tiling） */
};
#endif /* ANO_PROBE_TILING_H */
