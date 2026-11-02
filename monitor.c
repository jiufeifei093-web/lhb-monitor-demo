
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

/* [新增] systemd Watchdog 接口 */
#include <systemd/sd-daemon.h>

#include "protocol.h"

/* [保留] 第 3、4 阶段参数 */
#define LHB_MAX_MESSAGE_AGE_MS 5000ULL
#define LHB_COMPONENT_SLOTS (LHB_QDISKD + 1)
#define LHB_HEARTBEAT_TIMEOUT_MS 15000ULL
#define LHB_SCAN_INTERVAL_MS 1000ULL
#define LHB_NSEC_PER_MSEC 1000000ULL

/* 1. 事件循环 */
static qb_loop_t *main_loop;

/* [保留] 心跳健康状态 */
enum lhb_health_state {
    LHB_HEALTH_WAITING = 0,
    LHB_HEALTH_NORMAL,
    LHB_HEALTH_TIMEOUT
};

/* [保留] 每个被监控组件的状态 */
struct component_state {
    bool has_seen;

    uint32_t pid;
    uint64_t instance_id;

    uint64_t last_sequence;
    uint64_t last_send_monotonic_ms;
    uint64_t last_seen_monotonic_ms;

    qb_ipcs_connection_t *conn;

    enum lhb_health_state health;
    bool waiting_timeout_reported;

    /* 故障现场快照 */
    uint32_t timeout_pid;
    uint64_t timeout_instance_id;
    uint64_t timeout_last_seen_ms;
    uint64_t timeout_detected_ms;
};

static struct component_state components[LHB_COMPONENT_SLOTS];

/* [保留] 定时扫描状态 */
static uint64_t monitor_start_ms;
static qb_loop_timer_handle scan_timer_handle;
static bool scan_failed;

/* [新增] systemd Watchdog 状态 */
static bool watchdog_enabled;
static uint64_t watchdog_ping_interval_ms;
static uint64_t watchdog_last_ping_ms;

/*
 * 2. IPC 连接鉴权
 * [保留] 第 3 阶段实现
 */
static int32_t monitor_connection_accept(
    qb_ipcs_connection_t *conn,
    uid_t uid,
    gid_t gid)
{
    if (uid != geteuid()) {
        return -EACCES;
    }

    qb_ipcs_connection_auth_set(conn, uid, gid, 0600);
    return 0;
}

/*
 * 3. IPC 连接创建
 * [保留]
 */
static void monitor_connection_created(
    qb_ipcs_connection_t *conn)
{
    qb_ipcs_context_set(conn, NULL);
}

/*
 * 4. IPC 连接关闭
 * [保留] 不清除历史心跳信息
 */
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
                state->pid,
                state->instance_id);
    }

    qb_ipcs_context_set(conn, NULL);
    return 0;
}

/*
 * 5. 获取 KCSM 单调时钟时间
 * [保留]
 */
static int monotonic_time_ms(uint64_t *out_ms)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        return -1;
    }

    *out_ms =
        (uint64_t)now.tv_sec * 1000ULL +
        (uint64_t)now.tv_nsec / 1000000ULL;

    return 0;
}

/*
 * 6. [新增] 初始化 systemd Watchdog
 *
 * 通过 sd_watchdog_enabled() 读取 WatchdogSec。
 * 未启用 Watchdog 时，允许 KCSM 单独运行。
 */
static int watchdog_init(void)
{
    uint64_t watchdog_usec = 0;

    int rc = sd_watchdog_enabled(
        0,
        &watchdog_usec
    );

    if (rc < 0) {
        fprintf(stderr,
                "[WATCHDOG_ERROR] sd_watchdog_enabled: %s\n",
                strerror(-rc));
        return -1;
    }

    if (rc == 0) {
        printf("[WATCHDOG] disabled "
               "(no WatchdogSec for this process)\n");
        return 0;
    }

    /*
     * [新增] Watchdog 时间单位为微秒。
     * 除以 2000，得到一半超时周期的毫秒数。
     *
     * 示例：WatchdogSec=10s
     *       watchdog_usec=10000000
     *       ping_interval_ms=5000
     */
    watchdog_ping_interval_ms =
        watchdog_usec / 2000ULL;

    /*
     * [新增] 限制过短的 Watchdog 周期。
     * 当前扫描周期为 1 秒，测试服务要求至少 4 秒超时。
     */
    if (watchdog_ping_interval_ms <
        2ULL * LHB_SCAN_INTERVAL_MS) {

        fprintf(stderr,
                "[WATCHDOG_ERROR] WatchdogSec too short "
                "for 1s scan (minimum 4s in this demo)\n");
        return -1;
    }

    watchdog_enabled = true;

    printf("[WATCHDOG] enabled "
           "timeout_usec=%" PRIu64
           " ping_interval_ms=%" PRIu64 "\n",
           watchdog_usec,
           watchdog_ping_interval_ms);

    return 0;
}

/*
 * 7. [新增] 发送 systemd Watchdog 健康通知
 *
 * 注意：
 * 该函数只在心跳扫描完成后调用。
 * 不创建独立 Watchdog 发送线程。
 */
static int watchdog_maybe_ping(uint64_t now_ms)
{
    if (!watchdog_enabled) {
        return 0;
    }

    if (now_ms < watchdog_last_ping_ms) {
        fprintf(stderr,
                "[WATCHDOG_ERROR] monotonic clock moved backwards\n");
        return -1;
    }

    uint64_t elapsed_ms =
        now_ms - watchdog_last_ping_ms;

    /* 尚未达到本次通知周期 */
    if (elapsed_ms < watchdog_ping_interval_ms) {
        return 0;
    }

    /* [新增] 向 systemd 发送健康通知 */
    int rc = sd_notify(
        0,
        "WATCHDOG=1"
    );

    if (rc <= 0) {
        fprintf(stderr,
                "[WATCHDOG_ERROR] health notification failed: %s\n",
                rc < 0
                    ? strerror(-rc)
                    : "notification socket unavailable");
        return -1;
    }

    /* [新增] 更新最近一次健康通知时间 */
    watchdog_last_ping_ms = now_ms;

    /*
     * 正常每次喂狗不打印日志，
     * 避免产生额外的周期性日志开销。
     */
    return 0;
}

/*
 * 8. 单个组件的心跳故障超时检测
 * [保留] 第 4 阶段功能
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

        state->timeout_pid = state->pid;
        state->timeout_instance_id =
            state->instance_id;
        state->timeout_last_seen_ms =
            state->last_seen_monotonic_ms;
        state->timeout_detected_ms = now_ms;

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
 * 9. 注册心跳扫描定时器
 * [保留] 第 4 阶段功能
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
 * 10. [修改] 心跳定时扫描回调
 *
 * 保留三个组件的故障检测。
 * 新增 Watchdog 健康通知。
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

        /* [保留] 首次等待超时检查 */
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

        /* [保留] 检查已建立监控关系的组件 */
        check_component_timeout(
            component,
            state,
            now_ms
        );
    }

    /* [保留] 注册下一次心跳扫描 */
    if (schedule_scan_timer() != 0) {
        scan_failed = true;
        qb_loop_stop(main_loop);

        /* [新增] 失败后不再发送健康通知 */
        return;
    }

    /*
     * [新增] 完成本轮扫描后再发送 Watchdog。
     * 主循环 Hang 时不会执行到这里。
     */
    if (watchdog_maybe_ping(now_ms) != 0) {
        scan_failed = true;
        qb_loop_stop(main_loop);
    }
}

/*
 * 11. 接收并校验心跳
 * [保留] 第 3、4 阶段功能
 */
static int32_t monitor_msg_process(
    qb_ipcs_connection_t *conn,
    void *data,
    size_t size)
{
    if (size != sizeof(struct lhb_heartbeat)) {
        fprintf(stderr,
                "[REJECT] invalid heartbeat size: %zu\n",
                size);
        return 0;
    }

    struct lhb_heartbeat hb;
    memcpy(&hb, data, sizeof(hb));

    /* [保留] 消息基本字段校验 */
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

    /* [保留] 核对 libqb 客户端 PID */
    struct qb_ipcs_connection_stats_2 *stats =
        qb_ipcs_connection_stats_get_2(conn, 0);

    if (stats == NULL) {
        fprintf(stderr,
                "[REJECT] failed to obtain client PID\n");
        return 0;
    }

    int32_t peer_pid = stats->client_pid;
    free(stats);

    if (peer_pid <= 0 ||
        (uint32_t)peer_pid != hb.pid) {

        fprintf(stderr,
                "[REJECT] PID mismatch: claimed=%" PRIu32
                " peer=%" PRId32 "\n",
                hb.pid,
                peer_pid);
        return 0;
    }

    uint64_t now_ms;

    if (monotonic_time_ms(&now_ms) != 0) {
        return 0;
    }

    /* [保留] 检查发送时间 */
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

    /* [保留] IPC 连接绑定检查 */
    if (bound != NULL && bound != state) {
        fprintf(stderr,
                "[REJECT] connection attempted to switch component\n");
        return 0;
    }

    if (state->conn != NULL &&
        state->conn != conn) {

        fprintf(stderr,
                "[REJECT] concurrent connection for %s\n",
                lhb_component_name(hb.component));
        return 0;
    }

    bool new_instance =
        state->has_seen &&
        (state->pid != hb.pid ||
         state->instance_id != hb.instance_id);

    if (new_instance && state->conn == conn) {
        fprintf(stderr,
                "[REJECT] instance changed on live connection\n");
        return 0;
    }

    /* [保留] 心跳序列号及时间顺序检查 */
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

    /* [保留] 更新心跳之前先检查是否达到故障超时 */
    check_component_timeout(
        hb.component,
        state,
        now_ms
    );

    /* [保留] 进程注册和实例更换日志 */
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

    /* [保留] 更新有效心跳信息 */
    state->conn = conn;
    qb_ipcs_context_set(conn, state);

    state->has_seen = true;
    state->pid = hb.pid;
    state->instance_id = hb.instance_id;
    state->last_sequence = hb.sequence;
    state->last_send_monotonic_ms =
        hb.send_monotonic_ms;
    state->last_seen_monotonic_ms = now_ms;

    /* [保留] WAITING -> NORMAL */
    if (state->health == LHB_HEALTH_WAITING) {
        state->health = LHB_HEALTH_NORMAL;

        printf("[STATE] %s WAITING -> NORMAL\n",
               lhb_component_name(hb.component));
    }

    /* [保留] 故障锁存日志 */
    const char *tag =
        (state->health == LHB_HEALTH_TIMEOUT)
        ? "LATE_HEARTBEAT"
        : "HEARTBEAT";

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

/*
 * 12. libqb IPC 回调适配
 * [保留] 原有事件循环机制
 */
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

/*
 * 13. [修改] KCSM 主函数
 *
 * 原有 IPC 启动流程保持不变。
 * 新增 systemd Watchdog 初始化与 READY 通知。
 */
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

    /* [保留] 注册 IPC 回调 */
    qb_ipcs_poll_handlers_set(
        service,
        &poll_handlers
    );

    /* [保留] 启动 IPC 服务 */
    int32_t rc = qb_ipcs_run(service);

    if (rc < 0) {
        fprintf(stderr,
                "qb_ipcs_run failed: %s\n",
                strerror(-rc));

        qb_loop_destroy(main_loop);
        return 1;
    }

    /*
     * [修改] 在启动事件循环之前：
     * 1. 初始化 systemd Watchdog
     * 2. 记录监控启动时间
     * 3. 注册心跳扫描定时器
     */
    if (watchdog_init() != 0 ||
        monotonic_time_ms(&monitor_start_ms) != 0 ||
        schedule_scan_timer() != 0) {

        qb_ipcs_destroy(service);
        qb_loop_destroy(main_loop);
        return 1;
    }

    /*
     * [新增] IPC 和定时扫描均已就绪，
     * 通知 systemd 服务启动完成。
     */
    rc = sd_notify(
        0,
        "READY=1\nSTATUS=KCSM monitoring active"
    );

    if (rc < 0 ||
        (watchdog_enabled && rc == 0)) {

        fprintf(stderr,
                "[WATCHDOG_ERROR] READY notification failed: %s\n",
                rc < 0
                    ? strerror(-rc)
                    : "notification socket unavailable");

        qb_ipcs_destroy(service);
        qb_loop_destroy(main_loop);
        return 1;
    }

    /* [新增] 初始化健康通知计时起点 */
    watchdog_last_ping_ms = monitor_start_ms;

    /* [保留] 原有启动日志 */
    printf("lhb-monitor started: %s (SHM, no ACK)\n",
           LHB_SERVICE_NAME);

    printf("[CONFIG] timeout=%llu ms"
           " scan_interval=%llu ms\n",
           (unsigned long long)LHB_HEARTBEAT_TIMEOUT_MS,
           (unsigned long long)LHB_SCAN_INTERVAL_MS);

    fflush(stdout);

    /* [保留] 启动主事件循环 */
    qb_loop_run(main_loop);

    /* [保留] 资源清理 */
    qb_ipcs_destroy(service);
    qb_loop_destroy(main_loop);

    /*
     * [修改] 当前 Demo 尚无正常退出主循环的路径。
     * 如果主循环返回，则使用非零退出码。
     */
    if (!scan_failed) {
        fprintf(stderr,
                "[ERROR] monitor event loop exited unexpectedly\n");
    }

    return 1;
}