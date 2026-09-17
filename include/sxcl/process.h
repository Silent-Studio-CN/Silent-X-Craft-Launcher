/* 子进程启动与输出捕获 —— 静默安装加载器、启动游戏、抓日志的地基。
 *
 * 为什么自研而不是用 system()/popen():
 *   - 要同时收 stdout 与 stderr,并且**逐行回调**(安装器的进度标记、游戏的日志都要边跑边解析);
 *   - 要有超时与"回调里主动终止"的能力(静默安装卡死时不能挂住启动器);
 *   - 不能弹控制台窗口(Windows 上 CREATE_NO_WINDOW),GUI 启动器里闪黑框很刺眼;
 *   - 参数与环境变量按数组给,不拼 shell 字符串(路径带空格/中文时不至于被拆坏)。
 *
 * 线程模型:sxcl_process_run 阻塞,内部在工作线程语义下逐行回调(回调里别做重活)。
 */
#ifndef SXCL_PROCESS_H
#define SXCL_PROCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxcl_process_opts {
    const char *program;      /**< 可执行文件(可只给名字,按 PATH 查找);UTF-8 */
    const char *const *args;  /**< 参数数组,NULL 结尾;**不含** program 本身;可空 */
    const char *work_dir;     /**< 工作目录;可空 = 继承 */
    const char *const *env;   /**< 追加/覆盖的环境变量,"KEY=VALUE" 数组,NULL 结尾;可空 */
    int timeout_ms;           /**< <=0 表示不限时 */
    /** 每读到一行输出调用一次(尾随换行已去掉)。
     *  返回非 0 = 请求终止进程(用于"发现安装完成标记就收工"这类场景)。 */
    int (*on_line)(void *userdata, int is_stderr, const char *line);
    void *userdata;
} sxcl_process_opts;

typedef struct sxcl_process_result {
    int exit_code;        /**< 进程退出码;未能启动时为 -1 */
    int timed_out;        /**< 1 = 超时被我们终止 */
    int killed_by_client; /**< 1 = on_line 回调请求终止 */
    int64_t elapsed_ms;
    char error[192];      /**< 失败原因(人话) */
} sxcl_process_result;

/** 运行并等待结束。返回 0 = 正常跑完(退出码在 out 里);<0 = 没能启动或出错。 */
int sxcl_process_run(const sxcl_process_opts *opts, sxcl_process_result *out);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_PROCESS_H */
