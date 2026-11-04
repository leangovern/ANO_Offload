
/*
 * calution: this file was generated automatically do not change it.
*/

#ifndef ACLNN_ANO_PROBE_H_
#define ACLNN_ANO_PROBE_H_

#include "aclnn/acl_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* function: aclnnAnoProbeGetWorkspaceSize
 * parameters :
 * params : required
 * out : required
 * workspaceSize : size of workspace(output).
 * executor : executor context(output).
 */
__attribute__((visibility("default")))
aclnnStatus aclnnAnoProbeGetWorkspaceSize(
    const aclTensor *params,
    const aclTensor *out,
    uint64_t *workspaceSize,
    aclOpExecutor **executor);

/* function: aclnnAnoProbe
 * parameters :
 * workspace : workspace memory addr(input).
 * workspaceSize : size of workspace(input).
 * executor : executor context(input).
 * stream : acl stream.
 */
__attribute__((visibility("default")))
aclnnStatus aclnnAnoProbe(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream);

#ifdef __cplusplus
}
#endif

#endif
