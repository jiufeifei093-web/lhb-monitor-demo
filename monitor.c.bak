#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qb/qbipcs.h>
#include <qb/qbloop.h>

#include "protocol.h"

/* 1. 事件循环和最近一次有效心跳的接收时间 */
static qb_loop_t *main_loop;
static uint64_t last_seen_ms[4]; /* 下标 1、2、3 */

/* 2. IPC 客户端连接回调 */
static int32_t monitor_connection_accept(qb_ipcs_connection_t *conn, uid_t uid, gid_t gid)
{
    if (uid != geteuid()) {
        return -EACCES;
    }
    qb_ipcs_connection_auth_set(conn, uid, gid, 0600);
    return 0;
}

/* 3. 接收心跳：校验 -> 记录时间 -> 打印日志；不发送 ACK */
static int32_t monitor_msg_process(qb_ipcs_connection_t *conn, void *data, size_t size)
{
    (void)conn;

    if (size != sizeof(struct lhb_heartbeat)) {
        fprintf(stderr, "invalid heartbeat size: %zu\n", size);
        return 0;
    }

    struct lhb_heartbeat hb;
    memcpy(&hb, data, sizeof(hb));

    if (hb.hdr.id != LHB_HEARTBEAT_ID ||
        hb.hdr.size != sizeof(hb) ||
        hb.version != LHB_PROTO_VERSION ||
        hb.component < LHB_COROSYNC || hb.component > LHB_QDISKD) {
        fprintf(stderr, "invalid heartbeat data\n");
        return 0;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        return 0;
    }

    last_seen_ms[hb.component] =
        (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;

    printf("[HEARTBEAT] %-11s seq=%" PRIu64 " last_seen_ms=%" PRIu64 "\n",
           lhb_component_name(hb.component), hb.sequence,
           last_seen_ms[hb.component]);
    fflush(stdout);
    return 0;
}

/* 4. 把 IPC 事件交给 qb_loop 调度 */
static int32_t poll_add(enum qb_loop_priority p, int32_t fd,
                        int32_t events, void *data, qb_ipcs_dispatch_fn_t fn)
{
    return qb_loop_poll_add(main_loop, p, fd, events, data, fn);
}

static int32_t poll_mod(enum qb_loop_priority p, int32_t fd,
                        int32_t events, void *data, qb_ipcs_dispatch_fn_t fn)
{
    return qb_loop_poll_mod(main_loop, p, fd, events, data, fn);
}

static int32_t poll_del(int32_t fd)
{
    return qb_loop_poll_del(main_loop, fd);
}

static int32_t job_add(enum qb_loop_priority p, void *data,
                       qb_loop_job_dispatch_fn fn)
{
    return qb_loop_job_add(main_loop, p, data, fn);
}

/* 5. 启动 SHM IPC 服务并进入事件循环 */
int main(void)
{
    struct qb_ipcs_service_handlers handlers = {
        .connection_accept = monitor_connection_accept,
        .msg_process = monitor_msg_process
    };
    struct qb_ipcs_poll_handlers poll_handlers = {
        .dispatch_add = poll_add,
        .dispatch_mod = poll_mod,
        .dispatch_del = poll_del,
        .job_add = job_add
    };

    main_loop = qb_loop_create();
    if (!main_loop) {
        fprintf(stderr, "qb_loop_create failed\n");
        return 1;
    }

    qb_ipcs_service_t *service = qb_ipcs_create(
        LHB_SERVICE_NAME, 1, QB_IPC_SHM, &handlers);
    if (!service) {
        fprintf(stderr, "qb_ipcs_create failed\n");
        qb_loop_destroy(main_loop);
        return 1;
    }

    qb_ipcs_poll_handlers_set(service, &poll_handlers);
    int32_t rc = qb_ipcs_run(service);
    if (rc < 0) {
        fprintf(stderr, "qb_ipcs_run failed: %s\n", strerror(-rc));
        /* libqb 已在 qb_ipcs_run() 失败时销毁 service */
        qb_loop_destroy(main_loop);
        return 1;
    }

    printf("lhb-monitor started: %s (SHM, no ACK)\n", LHB_SERVICE_NAME);
    fflush(stdout);

    /* 不注册自定义停止信号回调 */
    qb_loop_run(main_loop);

    /* 仅在事件循环自行返回时执行 */
    qb_ipcs_destroy(service);
    qb_loop_destroy(main_loop);
    return 0;
}
