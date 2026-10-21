
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qb/qbipcs.h>
#include <qb/qbloop.h>

#include "protocol.h"

/* [保留] 第 3 阶段心跳报文时效检查 */
#define LHB_MAX_MESSAGE_AGE_MS 5000ULL
#define LHB_COMPONENT_SLOTS (LHB_QDISKD + 1)

/* [新增] 第 4 阶段固定参数，后续改为配置文件读取 */
#define LHB_HEARTBEAT_TIMEOUT_MS 15000ULL
#define LHB_SCAN_INTERVAL_MS 1000ULL
#define LHB_NSEC_PER_MSEC 1000000ULL

/* 1. 事件循环 */
static qb_loop_t *main_loop;

/* [新增] 心跳健康状态 */
enum lhb_health_state {
    LHB_HEALTH_WAITING = 0,
    LHB_HEALTH_NORMAL,
    LHB_HEALTH_TIMEOUT
};

/* 2. 每个被监控进程的独立状态 */
struct component_state {
    /* [保留] 第 3 阶段的进程和心跳信息 */
    bool has_seen;
    uint32_t pid;
    uint64_t instance_id;
    uint64_t last_sequence;
    uint64_t last_send_monotonic_ms;
    uint64_t last_seen_monotonic_ms;
    qb_ipcs_connection_t *conn;

    /* [新增] 第 4 阶段状态机字段 */
    enum lhb_health_state health;
    bool waiting_timeout_reported;

    /* [新增] 故障现场快照 */
    uint32_t timeout_pid;
    uint64_t timeout_instance_id;
    uint64_t timeout_last_seen_ms;
    uint64_t timeout_detected_ms;
};

static struct component_state components[LHB_COMPONENT_SLOTS];

/* [新增] KCSM 监控启动时间 */
static uint64_t monitor_start_ms = 0;

/* [新增] 扫描定时器及错误标识 */
static qb_loop_timer_handle scan_timer_handle;
static bool scan_failed = false;

/* 3. IPC 连接鉴权：[保留] */
static int32_t monitor_connection_accept(
    qb_ipcs_connection_t *conn, uid_t uid, gid_t gid)
{
    if (uid != geteuid()) {
        return -EACCES;
    }

    qb_ipcs_connection_auth_set(conn, uid, gid, 0600);
    return 0;
}

/* 4. IPC 连接创建：[保留] */
static void monitor_connection_created(
    qb_ipcs_connection_t *conn)
{
    qb_ipcs_context_set(conn, NULL);
}

/* 5. IPC 连接关闭：[保留] 不清除历史心跳状态 */
static int32_t monitor_connection_closed(
    qb_ipcs_connection_t *conn)
{
    struct component_state *state =
        qb_ipcs_context_get(conn);

    if (state != NULL && state->conn == conn) {
        state->conn = NULL;

        fprintf(stderr,
                "[DISCONNECT] pid=%" PRIu32
                " instance=%016" PRIx64 "\n",
                state->pid, state->instance_id);
    }

    qb_ipcs_context_set(conn, NULL);
    return 0;
}

/* 6. 获取 KCSM 单调时钟时间：[保留] */
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

/*
 * 7. [新增] 单个组件的心跳故障超时判断
 *
 * 只有 NORMAL 状态才能进入 TIMEOUT。
 * TIMEOUT 状态不重复报告同一次故障。
 */
static void check_component_timeout(
    uint32_t component,
    struct component_state *state,
    uint64_t now_ms)
{
    if (!state->has_seen ||
        state->health != LHB_HEALTH_NORMAL ||
        now_ms < state->last_seen_monotonic_ms) {
        return;
    }

    uint64_t missing_ms =
        now_ms - state->last_seen_monotonic_ms;

    if (missing_ms >= LHB_HEARTBEAT_TIMEOUT_MS) {

        /* [新增] 保存故障发生时的进程信息 */
        state->timeout_pid = state->pid;
        state->timeout_instance_id = state->instance_id;
        state->timeout_last_seen_ms =
            state->last_seen_monotonic_ms;
        state->timeout_detected_ms = now_ms;

        /* [新增] 状态转换 NORMAL -> TIMEOUT */
        state->health = LHB_HEALTH_TIMEOUT;

        fprintf(stderr,
                "[TIMEOUT] %s pid=%" PRIu32
                " instance=%016" PRIx64
                " missing_ms=%" PRIu64
                " threshold_ms=%llu\n",
                lhb_component_name(component),
                state->pid,
                state->instance_id,
                missing_ms,
                (unsigned long long)LHB_HEARTBEAT_TIMEOUT_MS);
    }
}

/*
 * 8. [新增] 注册定时扫描任务
 *
 * libqb 定时器为一次性任务，
 * 因此需要在回调结束后再次注册。
 */
static void monitor_scan_timer(void *data);

static int schedule_scan_timer(void)
{
    int32_t rc = qb_loop_timer_add(
        main_loop,
        QB_LOOP_MED,
        LHB_SCAN_INTERVAL_MS * LHB_NSEC_PER_MSEC,
        NULL,
        monitor_scan_timer,
        &scan_timer_handle
    );

    if (rc < 0) {
        fprintf(stderr,
                "[ERROR] qb_loop_timer_add failed: %s\n",
                strerror(-rc));
        return -1;
    }

    return 0;
}

/*
 * 9. [新增] 定时扫描三个被监控组件
 *
 * 正常情况下约每 1 秒运行一次。
 */
static void monitor_scan_timer(void *data)
{
    (void)data;

    uint64_t now_ms;

    if (monotonic_time_ms(&now_ms) != 0) {
        scan_failed = true;
        qb_loop_stop(main_loop);
        return;
    }

    for (uint32_t component = LHB_COROSYNC;
         component <= LHB_QDISKD;
         ++component) {

        struct component_state *state =
            &components[component];

        /* [新增] 首次心跳等待状态 */
        if (state->health == LHB_HEALTH_WAITING) {

            if (!state->waiting_timeout_reported &&
                now_ms >= monitor_start_ms &&
                now_ms - monitor_start_ms >=
                    LHB_HEARTBEAT_TIMEOUT_MS) {

                state->waiting_timeout_reported = true;

                fprintf(stderr,
                        "[WAITING_TIMEOUT] %s "
                        "no first heartbeat in %llu ms; "
                        "not classified as an established-session failure\n",
                        lhb_component_name(component),
                        (unsigned long long)
                            LHB_HEARTBEAT_TIMEOUT_MS);
            }

            continue;
        }

        /* [新增] 检查已建立监控关系的组件 */
        check_component_timeout(
            component, state, now_ms);
    }

    /* [新增] 重新注册下一次扫描 */
    if (schedule_scan_timer() != 0) {
        scan_failed = true;
        qb_loop_stop(main_loop);
    }
}

/*
 * 10. [修改] 心跳接收处理
 *
 * 第 3 阶段的校验逻辑全部保留。
 * 本阶段增加故障超时检查和状态转换。
 */
static int32_t monitor_msg_process(
    qb_ipcs_connection_t *conn, void *data, size_t size)
{
    /* [保留] 消息长度检查 */
    if (size != sizeof(struct lhb_heartbeat)) {
        fprintf(stderr,
                "[REJECT] invalid heartbeat size: %zu\n",
                size);
        return 0;
    }

    struct lhb_heartbeat hb;
    memcpy(&hb, data, sizeof(hb));

    /* [保留] 心跳消息基本字段检查 */
    if (hb.hdr.id != LHB_HEARTBEAT_ID ||
        hb.hdr.size != sizeof(hb) ||
        hb.version != LHB_PROTO_VERSION ||
        hb.component < LHB_COROSYNC ||
        hb.component > LHB_QDISKD ||
        hb.pid == 0 ||
        hb.instance_id == 0 ||
        hb.sequence == 0 ||
        hb.reserved != 0 ||
        hb.send_monotonic_ms == 0 ||
        hb.send_realtime_ms == 0) {

        fprintf(stderr,
                "[REJECT] invalid heartbeat fields\n");
        return 0;
    }

    /* [保留] 获取 IPC 客户端 PID */
    struct qb_ipcs_connection_stats_2 *stats =
        qb_ipcs_connection_stats_get_2(conn, 0);

    if (stats == NULL) {
        fprintf(stderr,
                "[REJECT] failed to obtain client PID\n");
        return 0;
    }

    int32_t peer_pid = stats->client_pid;
    free(stats);

    /* [保留] 核对进程 PID */
    if (peer_pid <= 0 ||
        (uint32_t)peer_pid != hb.pid) {

        fprintf(stderr,
                "[REJECT] PID mismatch: claimed=%" PRIu32
                " peer=%" PRId32 "\n",
                hb.pid, peer_pid);
        return 0;
    }

    /* [保留] 获取 KCSM 当前单调时间 */
    uint64_t now_ms;

    if (monotonic_time_ms(&now_ms) != 0) {
        return 0;
    }

    /* [保留] 心跳消息时效检查 */
    if (hb.send_monotonic_ms > now_ms ||
        now_ms - hb.send_monotonic_ms >
            LHB_MAX_MESSAGE_AGE_MS) {

        fprintf(stderr,
                "[REJECT] expired or future heartbeat from %s\n",
                lhb_component_name(hb.component));
        return 0;
    }

    struct component_state *state =
        &components[hb.component];

    struct component_state *bound =
        qb_ipcs_context_get(conn);

    /* [保留] 一个连接只能绑定一个组件 */
    if (bound != NULL && bound != state) {
        fprintf(stderr,
                "[REJECT] connection attempted to switch component\n");
        return 0;
    }

    /* [保留] 同组件不允许两个活动连接同时占用 */
    if (state->conn != NULL &&
        state->conn != conn) {

        fprintf(stderr,
                "[REJECT] concurrent connection for %s\n",
                lhb_component_name(hb.component));
        return 0;
    }

    /* [保留] 判断是否为新进程实例 */
    bool new_instance =
        state->has_seen &&
        (state->pid != hb.pid ||
         state->instance_id != hb.instance_id);

    if (new_instance && state->conn == conn) {
        fprintf(stderr,
                "[REJECT] instance changed on live connection\n");
        return 0;
    }

    /* [保留] 检查序列号及发送时间顺序 */
    if (state->has_seen && !new_instance &&
        (hb.sequence <= state->last_sequence ||
         hb.send_monotonic_ms <
             state->last_send_monotonic_ms)) {

        fprintf(stderr,
                "[REJECT] duplicate/out-of-order heartbeat from %s"
                " seq=%" PRIu64
                " previous=%" PRIu64 "\n",
                lhb_component_name(hb.component),
                hb.sequence,
                state->last_sequence);
        return 0;
    }

    /*
     * [新增] 在更新有效心跳接收时间之前，
     * 先判断是否已经达到故障超时阈值。
     */
    check_component_timeout(
        hb.component, state, now_ms);

    /* [保留] 首次注册及实例更换日志 */
    if (!state->has_seen) {
        printf("[REGISTER] %s pid=%" PRIu32
               " instance=%016" PRIx64 "\n",
               lhb_component_name(hb.component),
               hb.pid,
               hb.instance_id);

    } else if (new_instance) {
        printf("[RESTART] %s old_pid=%" PRIu32
               " new_pid=%" PRIu32
               " new_instance=%016" PRIx64 "\n",
               lhb_component_name(hb.component),
               state->pid,
               hb.pid,
               hb.instance_id);
    }

    /* [保留] 更新最近一次有效心跳状态 */
    state->conn = conn;
    qb_ipcs_context_set(conn, state);

    state->has_seen = true;
    state->pid = hb.pid;
    state->instance_id = hb.instance_id;
    state->last_sequence = hb.sequence;
    state->last_send_monotonic_ms =
        hb.send_monotonic_ms;
    state->last_seen_monotonic_ms = now_ms;

    /*
     * [新增] 首次有效心跳：
     * WAITING -> NORMAL
     *
     * TIMEOUT 状态不自动恢复。
     */
    if (state->health == LHB_HEALTH_WAITING) {
        state->health = LHB_HEALTH_NORMAL;

        printf("[STATE] %s WAITING -> NORMAL\n",
               lhb_component_name(hb.component));
    }

    /*
     * [新增] TIMEOUT 后仍允许记录有效心跳，
     * 但不清除已确认的故障。
     */
    const char *tag =
        (state->health == LHB_HEALTH_TIMEOUT)
        ? "LATE_HEARTBEAT"
        : "HEARTBEAT";

    /* [修改] 增加状态相关的日志标识 */
    printf("[%s] %-11s pid=%" PRIu32
           " instance=%016" PRIx64
           " seq=%" PRIu64
           " send_mono_ms=%" PRIu64
           " send_real_ms=%" PRIu64
           " last_seen_ms=%" PRIu64 "\n",
           tag,
           lhb_component_name(hb.component),
           hb.pid,
           hb.instance_id,
           hb.sequence,
           hb.send_monotonic_ms,
           hb.send_realtime_ms,
           state->last_seen_monotonic_ms);

    fflush(stdout);
    return 0;
}

/* 11. IPC 事件循环回调：[保留] */
static int32_t poll_add(
    enum qb_loop_priority p,
    int32_t fd,
    int32_t events,
    void *data,
    qb_ipcs_dispatch_fn_t fn)
{
    return qb_loop_poll_add(
        main_loop, p, fd, events, data, fn);
}

static int32_t poll_mod(
    enum qb_loop_priority p,
    int32_t fd,
    int32_t events,
    void *data,
    qb_ipcs_dispatch_fn_t fn)
{
    return qb_loop_poll_mod(
        main_loop, p, fd, events, data, fn);
}

static int32_t poll_del(int32_t fd)
{
    return qb_loop_poll_del(main_loop, fd);
}

static int32_t job_add(
    enum qb_loop_priority p,
    void *data,
    qb_loop_job_dispatch_fn fn)
{
    return qb_loop_job_add(
        main_loop, p, data, fn);
}

/* 12. 启动 IPC 服务和定时扫描 */
int main(void)
{
    struct qb_ipcs_service_handlers handlers = {
        .connection_accept = monitor_connection_accept,
        .connection_created = monitor_connection_created,
        .msg_process = monitor_msg_process,
        .connection_closed = monitor_connection_closed
    };

    struct qb_ipcs_poll_handlers poll_handlers = {
        .dispatch_add = poll_add,
        .dispatch_mod = poll_mod,
        .dispatch_del = poll_del,
        .job_add = job_add
    };

    /* [保留] 创建主事件循环 */
    main_loop = qb_loop_create();

    if (!main_loop) {
        fprintf(stderr, "qb_loop_create failed\n");
        return 1;
    }

    /* [保留] 创建 libqb SHM IPC 服务 */
    qb_ipcs_service_t *service = qb_ipcs_create(
        LHB_SERVICE_NAME,
        1,
        QB_IPC_SHM,
        &handlers
    );

    if (!service) {
        fprintf(stderr, "qb_ipcs_create failed\n");
        qb_loop_destroy(main_loop);
        return 1;
    }

    /* [保留] 设置 IPC 事件回调 */
    qb_ipcs_poll_handlers_set(
        service, &poll_handlers);

    /* [保留] 启动 IPC 服务 */
    int32_t rc = qb_ipcs_run(service);

    if (rc < 0) {
        fprintf(stderr,
                "qb_ipcs_run failed: %s\n",
                strerror(-rc));

        /* libqb 已在失败时销毁 service */
        qb_loop_destroy(main_loop);
        return 1;
    }

    /*
     * [新增] IPC 服务启动之后，
     * 记录监控启动时间并注册第一个扫描定时器。
     */
    if (monotonic_time_ms(&monitor_start_ms) != 0 ||
        schedule_scan_timer() != 0) {

        qb_ipcs_destroy(service);
        qb_loop_destroy(main_loop);
        return 1;
    }

    printf("lhb-monitor started: %s (SHM, no ACK)\n",
           LHB_SERVICE_NAME);

    /* [新增] 打印当前故障检测参数 */
    printf("[CONFIG] timeout=%llu ms scan_interval=%llu ms\n",
           (unsigned long long)LHB_HEARTBEAT_TIMEOUT_MS,
           (unsigned long long)LHB_SCAN_INTERVAL_MS);

    fflush(stdout);

    /* [保留] 进入事件循环 */
    qb_loop_run(main_loop);

    /* [保留] 服务清理 */
    qb_ipcs_destroy(service);
    qb_loop_destroy(main_loop);

    /* [修改] 扫描故障时返回非零 */
    return scan_failed ? 1 : 0;
}