#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <qb/qbipcc.h>

#include "protocol.h"

/* 只用于模拟三种监控对象，不代表真实组件健康状态 */
static uint32_t parse_component(const char *name)
{
    if (strcmp(name, "corosync") == 0)   return LHB_COROSYNC;
    if (strcmp(name, "pacemakerd") == 0) return LHB_PACEMAKERD;
    if (strcmp(name, "qdiskd") == 0)     return LHB_QDISKD;
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t component = (argc == 2) ? parse_component(argv[1]) : 0;
    if (component == 0) {
        fprintf(stderr, "usage: %s corosync|pacemakerd|qdiskd\n", argv[0]);
        return 1;
    }

    qb_ipcc_connection_t *conn = qb_ipcc_connect(
        LHB_SERVICE_NAME, LHB_BUFFER_SIZE);
    if (!conn) {
        fprintf(stderr, "qb_ipcc_connect failed (start monitor first)\n");
        return 1;
    }

    for (uint64_t seq = 1; ; ++seq) {
        struct lhb_heartbeat hb = {0};
        hb.hdr.id = LHB_HEARTBEAT_ID;
        hb.hdr.size = sizeof(hb);
        hb.version = LHB_PROTO_VERSION;
        hb.component = component;
        hb.sequence = seq;

        ssize_t n = qb_ipcc_send(conn, &hb, sizeof(hb));
        if (n != (ssize_t)sizeof(hb)) {
            fprintf(stderr, "[%s] send failed: %zd\n", argv[1], n);
            break;
        }

        printf("[%s] sent seq=%" PRIu64 "\n", argv[1], seq);
        fflush(stdout);
        sleep(1);
    }

    qb_ipcc_disconnect(conn);
    return 1;
}
