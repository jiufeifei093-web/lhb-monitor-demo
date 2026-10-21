
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>       /* [新增] bool 类型 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>       /* [新增] free() */
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qb/qbipcs.h>
#include <qb/qbloop.h>

#include "protocol.h"

/* [新增] 演示阶段：超过 5 秒仍未被处理的心跳视为过期。
 * 最后一个配置阶段再统一处理相关可配置参数。
 */
#define LHB_MAX_MESSAGE_AGE_MS 5000ULL
#define LHB_COMPONENT_SLOTS (LHB_QDISKD + 1)

/* 1. 事件循环和被监控组件状态 */
static qb_loop_t *main_loop;

/* [新增] 一个组件对应一份状态。0 号槽位不使用。 */
struct component_state {
    bool has_seen;
    uint32_t pid;
    uint64_t instance_id;
    uint64_t last_sequence;
    uint64_t last_send_monotonic_ms;
    uint64_t last_seen_monotonic_ms;
    qb_ipcs_connection_t *conn; /* 只在连接存续期内有效，不持有额外引用 */
};

/* [修改] 替代原有 last_seen_ms[4] */
static struct component_state components[LHB_COMPONENT_SLOTS];

/* 2. IPC 客户端连接回调：原有鉴权逻辑保持不变 */
static int32_t monitor_connection_accept(qb_ipcs_connection_t *conn,
                                         uid_t uid, gid_t gid)
{
    if (uid != geteuid()) {
        return -EACCES;
    }
    qb_ipcs_connection_auth_set(conn, uid, gid, 0600);
    return 0;
}

/* [新增] 连接成功创建后，初始化连接所属组件的上下文 */
static void monitor_connection_created(qb_ipcs_connection_t *conn)
{
    qb_ipcs_context_set(conn, NULL);
}

/* [新增] 连接断开只解除连接绑定，不清除历史心跳状态 */
static int32_t monitor_connection_closed(qb_ipcs_connection_t *conn)
{
    struct component_state *state = qb_ipcs_context_get(conn);

    if (state != NULL && state->conn == conn) {
        state->conn = NULL;
        fprintf(stderr, "[DISCONNECT] pid=%" PRIu32
                        " instance=%016" PRIx64 "\n",
                state->pid, state->instance_id);
    }

    qb_ipcs_context_set(conn, NULL);
    return 0;
}

/* [新增] 在 KCSM 所在主机读取单调时钟毫秒数 */
static int monotonic_time_ms(uint64_t *out_ms)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        return -1;
    }

    *out_ms = (uint64_t)now.tv_sec * 1000ULL +
              (uint64_t)now.tv_nsec / 1000000ULL;
    return 0;
}

/* 3. 接收心跳：格式 -> 来源 -> 时效 -> 顺序 -> 更新状态；不发送 ACK */
static int32_t monitor_msg_process(qb_ipcs_connection_t *conn,
                                   void *data, size_t size)
{
    if (size != sizeof(struct lhb_heartbeat)) {
        fprintf(stderr, "[REJECT] invalid heartbeat size: %zu\n", size);
        return 0;
    }

    struct lhb_heartbeat hb;
    memcpy(&hb, data, sizeof(hb));

    /* [修改] 原有校验 + 新增身份/保留字段的基本校验 */
    if (hb.hdr.id != LHB_HEARTBEAT_ID ||
        hb.hdr.size != sizeof(hb) ||
        hb.version != LHB_PROTO_VERSION ||
        hb.component < LHB_COROSYNC || hb.component > LHB_QDISKD ||
        hb.pid == 0 || hb.instance_id == 0 || hb.sequence == 0 ||
        hb.reserved != 0 ||
        hb.send_monotonic_ms == 0 || hb.send_realtime_ms == 0) {
        fprintf(stderr, "[REJECT] invalid heartbeat fields\n");
        return 0;
    }

    /* [新增] 从 libqb 获取连接的客户端 PID，拒绝冒填 PID */
    struct qb_ipcs_connection_stats_2 *stats =
        qb_ipcs_connection_stats_get_2(conn, 0);
    if (stats == NULL) {
        fprintf(stderr, "[REJECT] failed to obtain client PID\n");
        return 0;
    }

    int32_t peer_pid = stats->client_pid;
    free(stats);

    if (peer_pid <= 0 || (uint32_t)peer_pid != hb.pid) {
        fprintf(stderr, "[REJECT] PID mismatch: claimed=%" PRIu32
                        " peer=%" PRId32 "\n", hb.pid, peer_pid);
        return 0;
    }

    uint64_t now_ms;
    if (monotonic_time_ms(&now_ms) != 0) {
        return 0;
    }

    /* [新增] 两端位于同一 Linux 节点，使用 CLOCK_MONOTONIC 判定消息时效。
     * 发送实时时钟可能被 NTP 或管理员调整，仅保留用于诊断。
     */
    if (hb.send_monotonic_ms > now_ms ||
        now_ms - hb.send_monotonic_ms > LHB_MAX_MESSAGE_AGE_MS) {
        fprintf(stderr, "[REJECT] expired or future heartbeat from %s\n",
                lhb_component_name(hb.component));
        return 0;
    }

    struct component_state *state = &components[hb.component];
    struct component_state *bound = qb_ipcs_context_get(conn);

    /* [新增] 同一连接不能冒充另一个组件；同组件同时只接收一个连接 */
    if (bound != NULL && bound != state) {
        fprintf(stderr, "[REJECT] connection attempted to switch component\n");
        return 0;
    }
    if (state->conn != NULL && state->conn != conn) {
        fprintf(stderr, "[REJECT] concurrent connection for %s\n",
                lhb_component_name(hb.component));
        return 0;
    }

    bool new_instance = state->has_seen &&
                        (state->pid != hb.pid ||
                         state->instance_id != hb.instance_id);

    /* [新增] 同一连接更换身份不允许；必须在旧连接结束后注册新实例 */
    if (new_instance && state->conn == conn) {
        fprintf(stderr, "[REJECT] instance changed on live connection\n");
        return 0;
    }

    /* [新增] 同一实例的序号必须递增，发送时间不得倒退 */
    if (state->has_seen && !new_instance &&
        (hb.sequence <= state->last_sequence ||
         hb.send_monotonic_ms < state->last_send_monotonic_ms)) {
        fprintf(stderr, "[REJECT] duplicate/out-of-order heartbeat from %s"
                        " seq=%" PRIu64 " previous=%" PRIu64 "\n",
                lhb_component_name(hb.component),
                hb.sequence, state->last_sequence);
        return 0;
    }

    /* [新增] 到这里才允许建立绑定和更新最后一次有效心跳时间 */
    if (!state->has_seen) {
        printf("[REGISTER] %s pid=%" PRIu32 " instance=%016" PRIx64 "\n",
               lhb_component_name(hb.component), hb.pid, hb.instance_id);
    } else if (new_instance) {
        printf("[RESTART] %s old_pid=%" PRIu32
               " new_pid=%" PRIu32 " new_instance=%016" PRIx64 "\n",
               lhb_component_name(hb.component), state->pid,
               hb.pid, hb.instance_id);
    }

    state->conn = conn;
    qb_ipcs_context_set(conn, state);
    state->has_seen = true;
    state->pid = hb.pid;
    state->instance_id = hb.instance_id;
    state->last_sequence = hb.sequence;
    state->last_send_monotonic_ms = hb.send_monotonic_ms;
    state->last_seen_monotonic_ms = now_ms;

    printf("[HEARTBEAT] %-11s pid=%" PRIu32
           " instance=%016" PRIx64 " seq=%" PRIu64
           " send_mono_ms=%" PRIu64 " send_real_ms=%" PRIu64
           " last_seen_ms=%" PRIu64 "\n",
           lhb_component_name(hb.component), hb.pid,
           hb.instance_id, hb.sequence, hb.send_monotonic_ms,
           hb.send_realtime_ms, state->last_seen_monotonic_ms);
    fflush(stdout);
    return 0;
}

/* 4. 把 IPC 事件交给 qb_loop 调度：完全保留原有代码 */
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
        .connection_created = monitor_connection_created, /* [新增] */
        .msg_process = monitor_msg_process,
        .connection_closed = monitor_connection_closed   /* [新增] */
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