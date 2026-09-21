# android/prebuilt —— 随包携带的第三方 .so

## 里面是什么

| 文件 | 大小 | sha256 |
|---|---|---|
| `arm64-v8a/libssl_3.so` | 634,680 B | `01d2bd0baac626efd3309f35f99c4b826dd9b885a7e4d14d5b12b3603d3a407f` |
| `arm64-v8a/libcrypto_3.so` | 4,030,424 B | `1e6c12ae0c2dadfe9d178d7f80f0ab248a1877a234066098bf5594e5205e5740` |

两个文件的 SONAME 就是文件名(llvm-readelf -d 实测):`libssl_3.so` NEEDED `libcrypto_3.so`,
`libcrypto_3.so` 只 NEEDED libc/libdl。内嵌版本串实测 `OpenSSL 3.1.8`。

## 为什么必须随包带

**安卓上 HTTPS 没有内置实现**。Qt 的 `libQt6Network` 只提供框架,真正的 TLS 要由
`plugins/tls/libplugins_tls_qopensslbackend_arm64-v8a.so` + OpenSSL 提供。
实测缺陷(2026-09-21):之前的 APK `lib/arm64-v8a/` 里有 `libQt6Network_arm64-v8a.so`,
但**没有** tls 插件、**也没有** `libssl_3.so`/`libcrypto_3.so` —— 于是设备上官方源与
BMCLAPI **两条都**报"网络请求失败"。用户看到的"拉不到版本列表"就是这条。

Qt 6 的 openssl 后端在安卓上是**运行时 dlopen**(所以插件自己的 NEEDED 里看不到 OpenSSL),
名字写死为 `libssl_3.so` / `libcrypto_3.so`,因此这两个文件名**不能改**。

## 它们怎么进 APK

1. `android/deployment-settings.template.json` 里的 `android-extra-libs`:
   `androiddeployqt.exe` 里确实有这个键(在二进制的字符串表里搜得到字面量)。
   顺序是 **libcrypto 在前、libssl 在后**(androiddeployqt 文档:列在依赖前面的库在某些设备上加载失败)。
2. tls 插件走 `deployment-dependencies`(与 qtforandroid / qsvg 等插件同一条路)。
3. `android/scripts/build_apk.ps1` 在 `androiddeployqt` 之后**逐个检查**这三个文件是否真的到了
   `$OUT/libs/arm64-v8a/`,缺了就自己补上并打 `tls FALLBACK` 日志,补不上直接 `exit 3`。
   —— 这条自检是故意加的:**不能再让 APK 静默地不带 TLS 出去**。

## 来源与许可

- 上游:OpenSSL 3.1.8 的 Android arm64-v8a 预编译产物,由 **KDAB/android_openssl**
  (https://github.com/KDAB/android_openssl)的预编译分支提供。
  本仓库内的这两个文件取自本机 `D:/SilentStudio/_openssl_android/`(由主代理下载并核实)。
- 许可:**Apache License 2.0**,原文见 `LICENSES/Apache-2.0.txt`(下载自
  `https://raw.githubusercontent.com/openssl/openssl/openssl-3.1.8/LICENSE.txt`;
  KDAB `android_openssl` 仓库的 LICENSE 是同一份 Apache-2.0 文本,已逐字节比对:sha256 相同)。
- OpenSSL 3.x 起 license 从 OpenSSL/SSLeay 双许可改为 Apache-2.0,所以**不需要**再附 SSLeay 条款;
  若将来换回 1.1.1 系列,这里必须同时补上 OpenSSL 旧许可原文。
