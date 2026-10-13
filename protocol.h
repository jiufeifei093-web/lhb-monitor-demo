#ifndef LHB_PROTOCOL_H
#define LHB_PROTOCOL_H

#include <stdint.h>
#include <qb/qbipc_common.h>

#define LHB_SERVICE_NAME  "lhb-demo-shm"
#define LHB_HEARTBEAT_ID    (QB_IPC_MSG_USER_START + 10)
#define LHB_PROTO_VERSION 1u
#define LHB_BUFFER_SIZE   4096u

enum lhb_component {
    LHB_COROSYNC   = 1,
    LHB_PACEMAKERD = 2,
    LHB_QDISKD     = 3
};

struct lhb_heartbeat {
    struct qb_ipc_request_header hdr;
    uint32_t version;
    uint32_t component;
    uint64_t sequence;
};

static inline const char *lhb_component_name(uint32_t component)
{
    switch (component) {
    case LHB_COROSYNC:   return "corosync";
    case LHB_PACEMAKERD: return "pacemakerd";
    case LHB_QDISKD:     return "qdiskd";
    default:             return "unknown";
    }
}

#endif
