/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

package com.silentstudio.sxcl;

import android.content.Context;
import android.content.Intent;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.Charset;

/* GameHost - "游戏进程(:game)"这条线的**全部主进程侧逻辑**,一个文件里切成三段:
 *
 *   ① 边界契约(§A):GameLaunchSpec(输入)+ 协议常量 + 事件回调 —— 跨进程只认这一份;
 *   ② 运行器通道(§B):本地 socket 监听 / 逐行解析 / 退出码与信号回收 / 状态机 ——
 *      **零 UI 依赖**(没有 Activity/View/画中画/Intent,连 Context 都没有);
 *   ③ 启动器适配(§C):Context、Intent、画中画时序 —— 这三样只在这一段里出现。
 *
 * 为什么要切这三段:将来若把运行器搬到别的应用里,搬走的是 §A + §B 的调用契约
 * (`GameLaunchSpec` / 协议 / 事件)+ 游戏进程那侧(sxcl_game.c + sxcl_jre_bootstrap.c +
 * GameActivity),A 侧只要把 §B 换成"对端在另一个应用里"的实现,§C 一行都不用改。
 * **当前不开发独立应用版本**(用户 2026-09-21:"先不开发 B,会莫名增大开发时间"),
 * 所以这里不引入新包/新层,只把缝切在同一个文件里,文档见 docs/19 §3/§7。
 *
 * 拉起顺序(**被系统规则逼出来的,不是偏好**;真机原文见 docs/19 §2):
 *   ① 启动器还在前台(可见、非 pinned)-> startActivity(GameActivity) —— 允许;
 *   ② 游戏窗口就绪 -> GameActivity 反过来请启动器回前台并进画中画(spec.launcherPip=1);
 *   ③ 启动器回到前台后在 onResume 里 enterFloating()(复用最大化键那条画中画实现)。
 * 反过来(先进画中画再起游戏)会被系统判成后台活动启动:BAL_BLOCK,result code=102。
 *
 * 结束游戏:通道上发 END -> 运行器有序收尾(flush + 退出记录)-> :game 进程自己结束;
 * 主进程只轮询 /proc/<pid> 确认它消失。**绝不杀整个 App**。
 */
public final class GameHost {

    private static final String TAG = "sxcl";
    /* 画中画/游戏窗口 20s 内没有回来请求时,如实记一行(不做假动作) */
    private static final int PIP_FALLBACK_MS = 20000;
    /* 结束游戏后确认 :game 真的消失的观察窗口 */
    private static final int GONE_POLL_MS = 8000;

    private GameHost() {}

    /* ═══════════════════ §A 边界契约(跨进程只认这一份) ═══════════════════ */

    /** 通道抽象名的前缀:A 生成的名字形如 sxcl-game-<A 的 pid>-<sessionId> */
    static final String CHANNEL_PREFIX = "sxcl-game-";
    /* 协议动词(行文本 + 空格分隔 key=value;能被人眼在 logcat / 文件里直接读) */
    static final String CMD_END = "END";
    static final String EV_HELLO = "HELLO";
    static final String EV_READY = "READY";
    static final String EV_STATE = "STATE";
    static final String EV_LOG = "LOG";
    static final String EV_PING = "PING";
    static final String EV_EXIT = "EXIT";
    static final String EV_SIG = "SIG";
    static final String EV_BYE = "BYE";
    /* 会话目录里的文件名(运行器写,A 读) */
    static final String FILE_EXIT = "exit.txt";
    static final String FILE_HOST_STATE = "host-state.txt";

    /** 运行器 -> 调用方的事件(全部在后台线程回调)。 */
    public interface Listener {
        void onLog(String line);

        void onPid(int pid, String proc);

        void onExit(int code, int signal, String reason, String source);

        void onState(String state);
    }

    /** 边界输入:起游戏只认这份参数表(与"谁来显示界面"无关)。
     *  字段一律 String/int:换传输介质(Intent extras -> Binder)时不用改数据结构。 */
    public static final class GameLaunchSpec {
        static final String KEY_SESSION_ID = "sxcl.sessionId";
        static final String KEY_SOCKET = "sxcl.socketName";
        static final String KEY_SESSION_DIR = "sxcl.sessionDir";
        static final String KEY_JRE = "sxcl.jreHome";
        static final String KEY_CLASSPATH = "sxcl.classpath";
        static final String KEY_NATIVES = "sxcl.nativesDir";
        static final String KEY_GAME_DIR = "sxcl.gameDir";
        static final String KEY_MAIN_CLASS = "sxcl.mainClass";
        static final String KEY_JVM_ARGS = "sxcl.jvmArgs";
        static final String KEY_GAME_ARGS = "sxcl.gameArgs";
        static final String KEY_RENDERER = "sxcl.renderer";
        static final String KEY_CRASH = "sxcl.crashMode";
        static final String KEY_LAUNCHER_PIP = "sxcl.launcherPip";

        public String sessionId = "";
        public String socketName = "";
        public String sessionDir = "";
        public String jreHome = "";
        public String classpath = "";
        public String nativesDir = "";
        public String gameDir = "";
        public String mainClass = "";
        public String jvmArgs = "";
        public String gameArgs = "";
        /** 渲染器:本轮只进边界与日志(渲染层还没接,不假装有) */
        public String renderer = "";
        /** 验收用崩溃注入:0 关 / 1 abort(SIGABRT)/ 2 SIGTERM / 3 SIGKILL */
        public int crashMode = 0;
        /** 运行器窗口就绪后,请启动器回前台进画中画(为什么必须由运行器发起见类头注释) */
        public boolean launcherPip = true;

        public Bundle toExtras() {
            Bundle b = new Bundle();
            b.putString(KEY_SESSION_ID, nz(sessionId));
            b.putString(KEY_SOCKET, nz(socketName));
            b.putString(KEY_SESSION_DIR, nz(sessionDir));
            b.putString(KEY_JRE, nz(jreHome));
            b.putString(KEY_CLASSPATH, nz(classpath));
            b.putString(KEY_NATIVES, nz(nativesDir));
            b.putString(KEY_GAME_DIR, nz(gameDir));
            b.putString(KEY_MAIN_CLASS, nz(mainClass));
            b.putString(KEY_JVM_ARGS, nz(jvmArgs));
            b.putString(KEY_GAME_ARGS, nz(gameArgs));
            b.putString(KEY_RENDERER, nz(renderer));
            b.putInt(KEY_CRASH, crashMode);
            b.putBoolean(KEY_LAUNCHER_PIP, launcherPip);
            return b;
        }

        public static GameLaunchSpec fromExtras(Bundle b) {
            GameLaunchSpec s = new GameLaunchSpec();
            if (b == null)
                return s;
            s.sessionId = b.getString(KEY_SESSION_ID, "");
            s.socketName = b.getString(KEY_SOCKET, "");
            s.sessionDir = b.getString(KEY_SESSION_DIR, "");
            s.jreHome = b.getString(KEY_JRE, "");
            s.classpath = b.getString(KEY_CLASSPATH, "");
            s.nativesDir = b.getString(KEY_NATIVES, "");
            s.gameDir = b.getString(KEY_GAME_DIR, "");
            s.mainClass = b.getString(KEY_MAIN_CLASS, "");
            s.jvmArgs = b.getString(KEY_JVM_ARGS, "");
            s.gameArgs = b.getString(KEY_GAME_ARGS, "");
            s.renderer = b.getString(KEY_RENDERER, "");
            s.crashMode = b.getInt(KEY_CRASH, 0);
            s.launcherPip = b.getBoolean(KEY_LAUNCHER_PIP, true);
            return s;
        }

        /** 证据行:两个进程的日志里都用它,便于对齐"同一份 spec" */
        public String toLine() {
            return "session=" + nz(sessionId) + " socket=" + nz(socketName) + " dir="
                    + nz(sessionDir) + " jre=" + orNone(jreHome) + " cp=" + orNone(classpath)
                    + " natives=" + orNone(nativesDir) + " gameDir=" + orNone(gameDir) + " main="
                    + orNone(mainClass) + " jvmArgs=[" + nz(jvmArgs) + "] gameArgs=[" + nz(gameArgs)
                    + "] renderer=" + orNone(renderer) + " crash=" + crashMode + " launcherPip="
                    + (launcherPip ? 1 : 0);
        }

        private static String nz(String s) {
            return s != null ? s : "";
        }

        private static String orNone(String s) {
            return s == null || s.isEmpty() ? "(none)" : s;
        }
    }

    /** 从 "key=value ..." 形态的一行里取整数 / 字符串(协议两侧共用同一读法)。 */
    public static int intField(String line, String key, int fallback) {
        final String needle = key + "=";
        int at = line.indexOf(needle);
        if (at < 0)
            return fallback;
        at += needle.length();
        int end = at;
        while (end < line.length() && (Character.isDigit(line.charAt(end)) || line.charAt(end) == '-'))
            end++;
        try {
            return Integer.parseInt(line.substring(at, end));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    public static String strField(String line, String key, String fallback) {
        final String needle = key + "=";
        int at = line.indexOf(needle);
        if (at < 0)
            return fallback;
        at += needle.length();
        int end = at;
        while (end < line.length() && !Character.isWhitespace(line.charAt(end)))
            end++;
        return line.substring(at, end);
    }

    /* ═════════ §B 运行器通道(零 UI:只有 socket / 文件 / 线程) ═════════
     *
     * 这一段是"游戏运行器"在主进程里的那一半。它**不认识界面**:没有 Activity、没有 Intent、
     * 没有画中画,连 Context 都没有(会话目录由 spec 给绝对路径)。
     * 异常断开三种情况(与 sxcl_game.c 的头注一一对应):
     *   ① 对端自己写了退出记录(EXIT/SIG 行 或 exit.txt)-> 退出码/信号是真的;
     *   ② EOF 且没有记录 + /proc/<pid> 消失 -> killed(退出状态从进程外面不可观测,如实说);
     *   ③ EOF 但 /proc/<pid> 还在 -> disconnected(运行器还活着,只是回传断了)。
     */

    private static final Object LOCK = new Object();
    private static GameLaunchSpec spec = new GameLaunchSpec();
    private static Listener listener = null;
    private static LocalServerSocket server = null;
    private static LocalSocket gameSocket = null;
    private static OutputStream cmdOut = null;
    private static Thread acceptThread = null;
    private static Thread readerThread = null;

    private static volatile boolean live = false;
    private static volatile boolean connected = false;
    private static volatile String state = "idle";
    private static volatile int gamePid = -1;
    private static volatile String gameProc = "";
    private static volatile int exitCode = Integer.MIN_VALUE;
    private static volatile int exitSignal = 0;
    private static volatile String exitReason = "";
    private static volatile String exitSource = "";
    private static volatile long pings = 0;
    private static volatile long logLines = 0;

    /** 建会话 + 开始监听通道(不含任何界面动作)。返回人可读状态,失败 "ERR:..."。 */
    private static String openChannel(GameLaunchSpec in, Listener l) {
        if (in == null || in.socketName.isEmpty())
            return "ERR:empty-spec";
        synchronized (LOCK) {
            if (live)
                return "ERR:already-running(" + stateLine() + ")";
            spec = in;
            listener = l;
            gamePid = -1;
            gameProc = "";
            connected = false;
            exitCode = Integer.MIN_VALUE;
            exitSignal = 0;
            exitReason = "";
            exitSource = "";
            pings = 0;
            logLines = 0;
            state = "listening";
            try {
                server = new LocalServerSocket(spec.socketName);
            } catch (IOException e) {
                server = null;
                state = "listen-failed";
                evidence("建立本地 socket 失败 name=" + spec.socketName + " err=" + e);
                return "ERR:listen(" + e + ")";
            }
            live = true;
            evidence("会话已建 " + spec.toLine() + ";等运行器 connect(进程由界面层拉起)");
            acceptThread = new Thread(new Runnable() {
                @Override
                public void run() { acceptLoop(); }
            }, "sxcl-runner-accept");
            acceptThread.setDaemon(true);
            acceptThread.start();
        }
        return "session=" + spec.sessionId + " socket=" + spec.socketName + " dir=" + spec.sessionDir;
    }

    /** 结束游戏:通道上发 END(之后由运行器自己有序收尾)。 */
    private static boolean stopChannel() {
        OutputStream out;
        final int pid;
        synchronized (LOCK) {
            if (!live) {
                Log.i(TAG, "game-session: 没有正在跑的会话,忽略结束请求");
                return false;
            }
            out = cmdOut;
            pid = gamePid;
            if (out == null) {
                evidence("结束游戏失败:通道还没接上(运行器没起来或没连上)");
                return false;
            }
        }
        try {
            out.write((CMD_END + "\n").getBytes("UTF-8"));
            out.flush();
        } catch (IOException e) {
            evidence("结束游戏:写 END 失败 " + e);
            return false;
        }
        evidence("已发出结束命令(END):等运行器有序收尾(flush 日志 + 写退出记录)并确认它消失");
        Thread watcher = new Thread(new Runnable() {
            @Override
            public void run() {
                final long deadline = System.currentTimeMillis() + GONE_POLL_MS;
                while (System.currentTimeMillis() < deadline) {
                    if (pid > 0 && !isAlive(pid)) {
                        Log.i(TAG, "game-session: 确认 :game 进程已消失 pid=" + pid
                                + "(主进程照常活着 pid=" + Process.myPid() + ")");
                        return;
                    }
                    try {
                        Thread.sleep(250);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                Log.w(TAG, "game-session: " + GONE_POLL_MS + "ms 内没有确认 :game 消失 pid=" + pid
                        + "(如实记录,不做任何强杀)");
            }
        }, "sxcl-runner-gone");
        watcher.setDaemon(true);
        watcher.start();
        return true;
    }

    /** 会话状态(取证行) */
    public static String stateLine() {
        return "session=" + spec.sessionId + " host-pid=" + Process.myPid() + " game-pid=" + gamePid
                + " proc=" + (gameProc.isEmpty() ? "?" : gameProc) + " state=" + state
                + " connected=" + (connected ? 1 : 0) + " exit-code="
                + (exitCode == Integer.MIN_VALUE ? "?" : String.valueOf(exitCode)) + " exit-signal="
                + exitSignal + " reason=" + (exitReason.isEmpty() ? "?" : exitReason) + " source="
                + (exitSource.isEmpty() ? "?" : exitSource) + " pings=" + pings + " log-lines="
                + logLines;
    }

    private static boolean isAlive(int pid) {
        return pid > 0 && new File("/proc/" + pid).exists();
    }

    private static void evidence(String line) {
        Log.i(TAG, "game-session: " + line);
        Log.i(TAG, "game-session STATE: " + stateLine());
        if (listener != null) {
            try {
                listener.onState(state);
            } catch (Throwable t) {
                Log.w(TAG, "game-session: listener.onState 抛异常:" + t);
            }
        }
        if (spec.sessionDir.isEmpty())
            return;
        try {
            FileOutputStream out = new FileOutputStream(new File(spec.sessionDir, FILE_HOST_STATE));
            try {
                out.write((line + "\n" + stateLine() + "\n").getBytes("UTF-8"));
                out.flush();
            } finally {
                out.close();
            }
        } catch (IOException e) {
            Log.w(TAG, "game-session: cannot write host-state.txt: " + e);
        }
    }

    private static void acceptLoop() {
        LocalServerSocket srv = server;
        if (srv == null)
            return;
        try {
            LocalSocket s = srv.accept();
            gameSocket = s;
            connected = true;
            state = "connected";
            cmdOut = s.getOutputStream();
            evidence("运行器已接上通道(accept 返回)");
            readerThread = new Thread(new Runnable() {
                @Override
                public void run() { readLoop(); }
            }, "sxcl-runner-reader");
            readerThread.setDaemon(true);
            readerThread.start();
        } catch (IOException e) {
            if (live)
                evidence("accept 失败/被关闭:" + e);
        }
    }

    private static void readLoop() {
        final LocalSocket s = gameSocket;
        if (s == null)
            return;
        BufferedReader in = null;
        try {
            in = new BufferedReader(
                    new InputStreamReader(s.getInputStream(), Charset.forName("UTF-8")));
            String line;
            while ((line = in.readLine()) != null)
                handleLine(line);
        } catch (Throwable t) {
            Log.w(TAG, "game-session: 通道读取出错:" + t);
        } finally {
            try {
                if (in != null)
                    in.close();
            } catch (IOException ignored) {
                /* 关不掉不影响判定 */
            }
            onChannelClosed();
        }
    }

    private static void handleLine(String line) {
        if (line == null || line.isEmpty())
            return;
        if (line.startsWith(EV_HELLO)) {
            gamePid = intField(line, "pid", gamePid);
            gameProc = strField(line, "proc", gameProc);
            evidence("运行器报到:" + line + "  <- ps 里应当能看到这个 PID");
            if (listener != null)
                listener.onPid(gamePid, gameProc);
            return;
        }
        if (line.startsWith(EV_READY) || line.startsWith(EV_STATE)) {
            evidence("运行器状态:" + line);
            return;
        }
        if (line.startsWith(EV_PING)) {
            pings++;
            if (pings == 1 || pings % 6 == 0)
                evidence("运行器心跳:" + line);
            return;
        }
        if (line.startsWith(EV_EXIT)) {
            exitCode = intField(line, "code", exitCode == Integer.MIN_VALUE ? 0 : exitCode);
            exitReason = strField(line, "reason", exitReason);
            exitSource = "socket";
            evidence("运行器自报退出:" + line);
            return;
        }
        if (line.startsWith(EV_SIG)) {
            exitSignal = intField(line, "signal", exitSignal);
            exitCode = intField(line, "code", exitCode);
            exitReason = "signal";
            exitSource = "socket";
            evidence("运行器崩溃兜底回传信号:" + line);
            return;
        }
        if (line.startsWith(EV_BYE)) {
            evidence("运行器通知收尾(BYE)");
            return;
        }
        if (line.startsWith("PIP?")) {
            /* 运行器问:要不要把启动器拉回前台进画中画?
             * 它在画中画里的时候不该再被拉出来一次(那会把游戏窗口打断),所以由**主进程**
             * 回答 —— 只有主进程知道自己的画中画状态(静态字段各进程各一份)。 */
            final boolean inPip = SxclActivity.isInFloating();
            final String answer = "PIP need=" + (inPip ? 0 : 1);
            boolean sent = false;
            synchronized (LOCK) {
                if (cmdOut != null) {
                    try {
                        cmdOut.write((answer + "\n").getBytes("UTF-8"));
                        cmdOut.flush();
                        sent = true;
                    } catch (IOException e) {
                        Log.w(TAG, "game-session: 回答 PIP? 失败:" + e);
                    }
                }
            }
            evidence("运行器问画中画 -> " + answer + " (已发出=" + sent + ")");
            return;
        }
        String text = line;
        if (line.startsWith(EV_LOG))
            text = line.substring(EV_LOG.length()).trim();
        logLines++;
        if (listener != null) {
            try {
                listener.onLog(text);
            } catch (Throwable t) {
                Log.w(TAG, "game-session: listener.onLog 抛异常:" + t);
            }
        }
    }

    private static void onChannelClosed() {
        connected = false;
        if (!live) {
            closeChannel();
            return;
        }
        state = "channel-closed";
        Log.i(TAG, "game-session: 通道已断开(EOF)");
        if (exitSource.isEmpty())
            readExitFile();
        boolean alive = isAlive(gamePid);
        if (exitCode == Integer.MIN_VALUE && exitSignal == 0) {
            /* 没有退出记录时,"还活着还是死了"只能看 /proc/<pid>;被 SIGKILL 的进程在进程表里
             * 可能还会残留一小会儿(回收是异步的),所以给它 2s 再下结论 —— 结论写错方向
             * 比慢 2 秒糟得多。 */
            if (alive && gamePid > 0) {
                for (int i = 0; i < 10 && alive; ++i) {
                    try {
                        Thread.sleep(200);
                    } catch (InterruptedException e) {
                        break;
                    }
                    alive = isAlive(gamePid);
                }
            }
            if (!alive) {
                exitReason = "channel-closed(pid 已消失)";
                exitSource = "proc-poll";
                state = "killed";
                evidence("运行器进程不在了,但**没有退出记录**:被 kill -9 / OOM 杀时,"
                        + "退出码与信号从进程外面不可观测 —— 如实报 killed,不编数字");
            } else {
                exitReason = "channel-closed(pid 仍在)";
                exitSource = "proc-poll";
                state = "disconnected";
                evidence("运行器进程还活着,只是回传通道断了(游戏继续跑,日志仍在它自己的文件里)");
            }
        } else {
            state = "ended";
            evidence("运行器已结束:信号=" + exitSignal + " 退出码="
                    + (exitCode == Integer.MIN_VALUE ? "?" : String.valueOf(exitCode)) + " 来源="
                    + exitSource + " pid-alive=" + (alive ? 1 : 0));
        }
        live = false;
        if (listener != null) {
            try {
                listener.onExit(exitCode, exitSignal, exitReason, exitSource);
            } catch (Throwable t) {
                Log.w(TAG, "game-session: listener.onExit 抛异常:" + t);
            }
        }
        closeChannel();
    }

    private static void readExitFile() {
        if (spec.sessionDir.isEmpty())
            return;
        File f = new File(spec.sessionDir, FILE_EXIT);
        if (!f.isFile())
            return;
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            byte[] all = new byte[(int) f.length()];
            int off = 0;
            while (off < all.length) {
                int n = in.read(all, off, all.length - off);
                if (n <= 0)
                    break;
                off += n;
            }
            String text = new String(all, 0, off, "UTF-8");
            exitCode = intField(text, "code", exitCode);
            exitSignal = intField(text, "signal", exitSignal);
            exitReason = strField(text, "reason", exitReason);
            exitSource = "exit.txt";
            Log.i(TAG, "game-session: 从退出记录文件读到 code=" + exitCode + " signal=" + exitSignal
                    + " reason=" + exitReason);
        } catch (IOException e) {
            Log.w(TAG, "game-session: 读 exit.txt 失败:" + e);
        } finally {
            try {
                if (in != null)
                    in.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void closeChannel() {
        try {
            if (server != null)
                server.close();
        } catch (IOException ignored) {
        }
        server = null;
        try {
            if (gameSocket != null)
                gameSocket.close();
        } catch (IOException ignored) {
        }
        gameSocket = null;
        cmdOut = null;
    }

    /* ═════════ §C 启动器适配(Context / Intent / 画中画时序) ═════════ */

    private static Context appCtx = null;
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static boolean started = false;
    private static volatile boolean pipRequested = false;

    public static void attach(Context ctx) {
        if (ctx != null)
            appCtx = ctx.getApplicationContext();
    }

    /** 边界的完整入口:调用方(将来的启动页)填好 spec;这里只补会话缺省值 + 界面时序。 */
    public static synchronized String requestStart(GameLaunchSpec in, String pipState) {
        if (appCtx == null)
            return "ERR:no-context";
        if (live)
            return "ERR:already-running(" + stateLine() + ")";

        final GameLaunchSpec s = in != null ? in : new GameLaunchSpec();
        if (s.sessionId.isEmpty())
            s.sessionId = "s" + System.currentTimeMillis();
        if (s.socketName.isEmpty())
            s.socketName = CHANNEL_PREFIX + Process.myPid() + "-" + s.sessionId;
        if (s.sessionDir.isEmpty())
            s.sessionDir = new File(appCtx.getFilesDir(), "game/" + s.sessionId).getAbsolutePath();
        if (s.nativesDir.isEmpty())
            s.nativesDir = appCtx.getApplicationInfo().nativeLibraryDir;
        final File dir = new File(s.sessionDir);
        if (!dir.isDirectory() && !dir.mkdirs())
            return "ERR:cannot-mkdir(" + dir + ")";
        started = false;
        pipRequested = false;

        /* 上一个会话可能停在"监听着但没有游戏进程连上来"(例如它那次启动被系统 BAL_BLOCK 掉)。
         * 这种会话必须能释放,否则后面的"启动游戏"永远只会得到 already-running。 */
        if (live && !connected && "listening".equals(state)) {
            evidence("上一个会话停在 listening(没有运行器连上)-> 释放它,按这次请求重新开始");
            live = false;
            closeChannel();
        }
        final String status = openChannel(s, new Listener() {
            @Override
            public void onLog(String line) {
                Log.i(TAG, "[game] " + line);
            }

            @Override
            public void onPid(int pid, String proc) {
                Log.i(TAG, "game-session: 运行器 PID=" + pid + " proc=" + proc);
            }

            @Override
            public void onExit(int code, int signal, String reason, String source) {
                Log.i(TAG, "game-session: 运行器结束 code=" + code + " signal=" + signal + " reason="
                        + reason + " source=" + source);
            }

            @Override
            public void onState(String st) {
                Log.i(TAG, "game-session: 界面侧看到状态=" + st + " pip=" + SxclActivity.isInFloating());
            }
        });
        Log.i(TAG, "game-session: 会话请求 " + s.toLine() + " pip=" + pipState);
        Log.i(TAG, "game-session: 通道 -> " + status);
        if (status.startsWith("ERR:"))
            return status;

        /* 关键顺序:启动器**此刻必须在前台且不在画中画里**,这时起游戏才是被允许的
         * (从 pinned 活动发起新活动会被 BAL_BLOCK,见类头注释与 docs/19 §2.1)。
         * 如果它现在正在画中画里(上一局游戏退到画中画的状态),先自己回到全屏,
         * 等窗口真的回到前台再起游戏 —— 否则 startActivity 会静默失败(result code=102)。 */
        if (SxclActivity.isInFloating()) {
            Log.i(TAG, "game-session: 启动器当前在画中画里 -> 先回全屏再起游戏"
                    + "(pinned 活动发起新活动会被系统判成后台启动)");
            SxclActivity.exitFloating();
            main.postDelayed(new Runnable() {
                @Override
                public void run() { startGameActivity("after-leaving-pip"); }
            }, 1800);
        } else if (!startGameActivity("request")) {
            return "ERR:start-activity-failed";
        }
        main.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!pipRequested && live)
                    Log.w(TAG, "game-session: " + PIP_FALLBACK_MS + "ms 内没有收到运行器的"
                            + "画中画请求(launcherPip=" + spec.launcherPip
                            + ");启动器此刻不在可见处 —— 如实记录,不做假动作");
            }
        }, PIP_FALLBACK_MS);
        return status;
    }

    /** 便捷重载:boot 文件 / `am start --es action gamestart` 那条路(本轮验收用) */
    public static String requestStart(String jreHome, String mainClass, String gameArgs,
                                      String crashMode, String pipState) {
        GameLaunchSpec s = new GameLaunchSpec();
        s.jreHome = jreHome != null ? jreHome : "";
        s.mainClass = mainClass != null ? mainClass : "";
        s.gameArgs = gameArgs != null ? gameArgs : "";
        s.crashMode = parseCrashMode(crashMode);
        return requestStart(s, pipState);
    }

    /** 崩溃注入开关的取值:数字(0-3)或人话字符串(abort/term/kill)。
     *  真机验收用的是字符串形式(`--es gamecrash abort`),只认数字会让"注入"静默失效 ——
     *  这一条就是被真机验收抓出来的。 */
    static int parseCrashMode(String v) {
        if (v == null)
            return 0;
        final String s = v.trim().toLowerCase(java.util.Locale.ROOT);
        if (s.isEmpty())
            return 0;
        if (s.equals("abort") || s.equals("sigabrt"))
            return 1;
        if (s.equals("term") || s.equals("sigterm"))
            return 2;
        if (s.equals("kill") || s.equals("sigkill"))
            return 3;
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            Log.w(TAG, "game-session: 不认识崩溃注入取值 '" + v + "',按 0(关闭)处理");
            return 0;
        }
    }

    /* 把游戏 Activity 拉起来(独立 task,全屏)。
     * Intent extras 就是边界输入的**传输介质**;将来换成"另一个应用 + 显式 component"时,
     * 这里换成那一种拉起方式即可,spec 与协议都不变(docs/19 §7)。 */
    public static synchronized boolean startGameActivity(String why) {
        if (appCtx == null || started || !live)
            return false;
        started = true;
        Intent it = new Intent();
        it.setClassName(appCtx.getPackageName(), "com.silentstudio.sxcl.GameActivity");
        /* 独立 task(**必须**):GameActivity 的 taskAffinity 与启动器不同,
         * 否则 singleTask + 同一 task 会把启动器从栈上清掉 */
        it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        it.putExtras(spec.toExtras());
        try {
            appCtx.startActivity(it);
            Log.i(TAG, "game-session: 已启动游戏 Activity(" + why + ",独立 task,android:process=:game)");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "game-session: 启动游戏 Activity 失败(" + why + "):" + t);
            return false;
        }
    }

    /** 运行器请求启动器回前台进画中画(SxclActivity 在 onNewIntent 里登记)。 */
    public static void noteLauncherPipRequest() {
        pipRequested = true;
        Log.i(TAG, "game-session: 收到运行器的画中画请求(启动器回前台 -> enterFloating)");
    }

    /* ---- 画中画状态回调(由 SxclActivity 转发) ---- */
    public static void onPipChanged(boolean inPip) {
        if (!inPip) {
            Log.i(TAG, "game-session: 画中画已退出(启动器回到全屏,游戏进程不受影响)");
            return;
        }
        Log.i(TAG, "game-session: 启动器已进入画中画(游戏继续跑,运行器不受任何影响)");
    }

    /* ---- 结束游戏 / 状态查询 ---- */
    public static boolean endGame() {
        return stopChannel();
    }

    public static boolean isSessionLive() {
        return live;
    }

    public static int gamePid() {
        return gamePid;
    }

    public static String sessionDirPath() {
        return spec.sessionDir;
    }

    public static GameLaunchSpec currentSpec() {
        return spec;
    }
}
