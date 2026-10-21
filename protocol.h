#ifndef LHB_PROTOCOL_H
#define LHB_PROTOCOL_H

#include <stdint.h>
#include <qb/qbipc_common.h>

/* 1. IPC 服务参数 */
#define LHB_SERVICE_NAME  "lhb-demo-shm"
#define LHB_HEARTBEAT_ID  (QB_IPC_MSG_USER_START + 10)
#define LHB_PROTO_VERSION 2u
#define LHB_BUFFER_SIZE   4096u

/* 2. 被监控组件编号 */
enum lhb_component {
    LHB_COROSYNC   = 1,
    LHB_PACEMAKERD = 2,
    LHB_QDISKD     = 3
};

/* 3. 本地心跳消息 */
struct lhb_heartbeat {
    struct qb_ipc_request_header hdr;

    uint32_t version;
    uint32_t component;

    uint32_t pid;
    uint32_t reserved;

    uint64_t instance_id;
    uint64_t sequence;

    uint64_t send_monotonic_ms;
    uint64_t send_realtime_ms;
};

/* 4. 将组件编号转换为组件名称 */
static inline const char *
lhb_component_name(uint32_t component)
{
    switch (component) {
    case LHB_COROSYNC:
        return "corosync";

    case LHB_PACEMAKERD:
        return "pacemakerd";

    case LHB_QDISKD:
        return "qdiskd";

    default:
        return "unknown";
    }
}

#endif
