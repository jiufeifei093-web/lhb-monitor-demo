#define _GNU_SOURCE  /* [修改] 允许使用 Linux getrandom() 接口 */

#include <errno.h>       /* [新增] EINTR 处理 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>  /* [新增] getrandom() */
#include <time.h>        /* [新增] clock_gettime() */
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

/* [新增] 为本次进程运行生成一个非零的随机实例 ID */
static int generate_instance_id(uint64_t *instance_id)
{
    do {
        unsigned char *ptr = (unsigned char *)instance_id;
        size_t remaining = sizeof(*instance_id);

        while (remaining > 0) {
            ssize_t n = getrandom(ptr, remaining, 0);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("getrandom");
                return -1;
            }
            if (n == 0) {
                fprintf(stderr, "getrandom returned 0 bytes\n");
                return -1;
            }

            ptr += (size_t)n;
            remaining -= (size_t)n;
        }
    } while (*instance_id == 0);

    return 0;
}

/* [新增] 读取指定时钟，返回毫秒时间戳 */
static int clock_time_ms(clockid_t clock_id, uint64_t *out_ms)
{
    struct timespec now;

    if (clock_gettime(clock_id, &now) != 0) {
        perror("clock_gettime");
        return -1;
    }
    if (now.tv_sec < 0) {
        fprintf(stderr, "clock_gettime returned negative seconds\n");
        return -1;
    }

    *out_ms = (uint64_t)now.tv_sec * 1000ULL +
              (uint64_t)now.tv_nsec / 1000000ULL;
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t component = (argc == 2) ? parse_component(argv[1]) : 0;
    if (component == 0) {
        fprintf(stderr, "usage: %s corosync|pacemakerd|qdiskd\n", argv[0]);
        return 1;
    }

    /* [新增] 每次进程启动确定身份；实例 ID 不随心跳变化 */
    uint32_t pid = (uint32_t)getpid();
    uint64_t instance_id = 0;
    if (generate_instance_id(&instance_id) != 0) {
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

        /* [新增] 每条心跳携带相同进程身份 */
        hb.pid = pid;
        hb.reserved = 0;
        hb.instance_id = instance_id;

        hb.sequence = seq;

        /* [新增] 发送前获取两种时间戳 */
        if (clock_time_ms(CLOCK_MONOTONIC, &hb.send_monotonic_ms) != 0 ||
            clock_time_ms(CLOCK_REALTIME, &hb.send_realtime_ms) != 0) {
            break;
        }

        ssize_t n = qb_ipcc_send(conn, &hb, sizeof(hb));
        if (n != (ssize_t)sizeof(hb)) {
            fprintf(stderr, "[%s] send failed: %zd\n", argv[1], n);
            break;
        }

        /* [修改] 输出新报文字段，便于演示检查 */
        printf("[%s] sent seq=%" PRIu64 " pid=%" PRIu32
               " instance=%016" PRIx64
               " mono_ms=%" PRIu64 " real_ms=%" PRIu64 "\n",
               argv[1], hb.sequence, hb.pid, hb.instance_id,
               hb.send_monotonic_ms, hb.send_realtime_ms);
        fflush(stdout);

        sleep(1);  /* [保留] 模拟发送端固定约 1 秒一次，暂不配置 */
    }

    qb_ipcc_disconnect(conn);
    return 1;
}