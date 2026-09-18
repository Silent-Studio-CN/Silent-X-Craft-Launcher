# 登录链测试夹具(tests/fixtures/auth)

这里的 JSON 都是**真实响应的脱敏副本**:字段名、层级、取值范围与微软/Xbox/Minecraft
服务端一致,只有 token 串本身换成了形状相同、内容为假的占位值(全部以 `FAKE.` 开头),
uuid 换成了 32 个 0。**不含任何真实凭据。**

| 文件 | 对应哪一跳 | 覆盖什么 |
| --- | --- | --- |
| ms_token_success.json | POST /oauth2/v2.0/token (authorization_code) | 成功:access/refresh/id token + expires_in |
| ms_token_error_invalid_grant.json | POST /oauth2/v2.0/token (refresh_token) | refresh token 过期(AADSTS700082) |
| ms_token_error_tenant_mismatch.json | POST /oauth2/v2.0/token | 应用租户类型没配对(AADSTS50059) |
| ms_token_error_pkce.json | POST /oauth2/v2.0/token | 公共客户端必须用 PKCE(AADSTS9002325) |
| ms_devicecode.json | POST /oauth2/v2.0/devicecode | 成功拿到 user_code/verification_uri/interval |
| ms_devicecode_pending.json | POST /oauth2/v2.0/token (device_code) | authorization_pending:继续轮询 |
| ms_devicecode_slow_down.json | 同上 | slow_down:间隔 +5 秒 |
| ms_devicecode_expired.json | 同上 | expired_token:设备码过期 |
| ms_devicecode_declined.json | 同上 | authorization_declined:用户点了拒绝 |
| xbl_user_auth_success.json | POST user.auth.xboxlive.com/user/authenticate | 成功:XBL Token + DisplayClaims.xui[0].uhs |
| xsts_success.json | POST xsts.auth.xboxlive.com/xsts/authorize | 成功:XSTS Token + uhs + NotAfter |
| xsts_err_2148916233.json | 同上 | 没有 Xbox 档案 |
| xsts_err_2148916235.json | 同上 | 所在地区不支持 |
| xsts_err_2148916238.json | 同上 | 未成年账号(需成人同意) |
| xsts_err_unknown.json | 同上 | 认不出的码:**必须原样带出数字** |
| mc_login_success.json | POST api.minecraftservices.com/authentication/login_with_xbox | 成功:MC access_token(expires_in 86400) |
| mc_login_401.json | 同上 | XSTS 不被接受 |
| mcstore_ok.json | GET .../entitlements/mcstore | 有 Java 版权益 |
| mcstore_404.json | 同上 | 没有任何权益 |
| mc_profile_ok.json | GET .../minecraft/profile | 有档案(name 非空) |
| mc_profile_empty_name.json | 同上 | **HTTP 200 但 name 为空 = 没买 Java 版**(必须报错) |
| mc_profile_404.json | 同上 | 404 = 没有 Java 版档案(必须报错) |
| bedrock_auth_ok.json | POST multiplayer.minecraft.net/authentication | 成功:证书链(基岩权益独立) |
| bedrock_auth_403.json | 同上 | 403 = 这个账号没有基岩版权益 |

夹具里每个 token 都长这样:`FAKE.<base64url 形状的占位串>.<base64url 形状的占位串>` ——
长度与真实 JWT 同量级,只为让"太长/太短"这类问题也能被测试发现,内容无意义。
