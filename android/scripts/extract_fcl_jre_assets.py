# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# extract_fcl_jre_assets.py - 把参考实现(FCL)APK assets 里的安卓 arm64 JRE 解成一个目录树,
# **只用于诊断**(推到自己应用的私有目录里跑 jre_exec_probe.sh)。不随我们的包发布,
# 也不当作 JRE 来源 —— 我们的来源是自有托管 + 运行时可配下载(见 docs/18 §6)。
#
# FCL 的资产布局(每个 JRE 一份,两份都要解到同一个目录):
#   <jreAssets>/app_runtime/java/jre<8|17|21|25>/universal.tar.xz   (conf/ legal/ lib/ ...)
#   <jreAssets>/app_runtime/java/jre<8|17|21|25>/bin-arm64.tar.xz   (bin/java, lib/libjli.so ...)
#
# 用法:
#   python extract_fcl_jre_assets.py <jreAssets 目录> <目标目录> [jre25]
# 例:
#   python android/scripts/extract_fcl_jre_assets.py \\
#          D:/SilentStudio/_ref/FCL/FCL/src/main/jreAssets D:/sxcl_local/jre25 jre25
#
# Windows 上创建符号链接多半没权限:链接按"复制目标内容"处理(legal/ 下的许可链接),
# 对 java -version 没有影响;真正要用的 bin/ 与 lib/ 都是普通文件。
import os
import shutil
import sys
import tarfile


def main(argv):
    if len(argv) < 3:
        print(__doc__ or "usage: extract_fcl_jre_assets.py <jreAssets> <dest> [jreN]")
        return 2
    assets = argv[1]
    dest = argv[2]
    name = argv[3] if len(argv) > 3 else "jre25"
    base = os.path.join(assets, "app_runtime", "java", name)
    if not os.path.isdir(base):
        print("没有这个 JRE 资产目录:", base)
        return 1
    tarballs = [os.path.join(base, t) for t in ("universal.tar.xz", "bin-arm64.tar.xz")]
    for path in tarballs:
        if not os.path.isfile(path):
            print("缺文件:", path)
            return 1
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)

    stats = {"dir": 0, "file": 0, "link_copy": 0, "link_skip": 0}
    for path in tarballs:
        with tarfile.open(path, "r:xz") as tar:
            for m in tar.getmembers():
                target = os.path.join(dest, m.name.replace("./", "", 1))
                if m.isdir():
                    os.makedirs(target, exist_ok=True)
                    stats["dir"] += 1
                elif m.isfile():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with tar.extractfile(m) as src, open(target, "wb") as out:
                        shutil.copyfileobj(src, out)
                    os.chmod(target, m.mode & 0o777)
                    stats["file"] += 1
                elif m.issym() or m.islnk():
                    link = m.linkname
                    resolved = (os.path.normpath(os.path.join(os.path.dirname(target), link))
                                if not os.path.isabs(link) else link)
                    if os.path.isfile(resolved):
                        shutil.copyfile(resolved, target)
                        stats["link_copy"] += 1
                    else:
                        stats["link_skip"] += 1
    java = os.path.join(dest, "bin", "java")
    print("解出:", dest, stats)
    print("bin/java:", os.path.isfile(java), os.path.getsize(java) if os.path.isfile(java) else -1)
    print("libjli.so:", os.path.isfile(os.path.join(dest, "lib", "libjli.so")))
    print("jre8 布局?", os.path.isdir(os.path.join(dest, "jre", "lib")),
          " 普通布局?", os.path.isdir(os.path.join(dest, "lib")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
