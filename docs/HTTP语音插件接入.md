# UniMRCP 1.8.0：接入现有 HTTP ASR / TTS

新增 `httptts` 与 `httpasr` 两个独立引擎插件，复用 UniMRCP 的协议和媒体框架。
原有 demo 插件、默认配置、FreeSWITCH 运行实例和 Call 控制器均未切换。
本次修改先完成 UniMRCP 服务端这一层；FreeSWITCH 安装 mod_unimrcp、控制器改为事件驱动仍属于后续联调工作。

## 1. 接口与能力范围

| 插件 | HTTP 契约 | 媒体 |
|---|---|---|
| httptts | POST JSON `{text, format:"wav", sample_rate:16000}`，返回 WAV 二进制 | 16 kHz PCM16 WAV；单声道或双声道，双声道取均值；输出单声道 |
| httpasr | POST multipart：`file` WAV，`format=wav`，`sample_rate=16000`，`language_hints=zh`；返回 `{"text":"..."}` | 收集 16 kHz 单声道 PCM，停顿后封装完整 WAV |

每个 HTTP 请求携带随机 `X-Request-ID`。插件自身只记录该标识和错误类别，不记录文本、音频或 URL。
环境代理不参与请求；不跟随重定向；HTTPS 保留 libcurl 默认的证书和主机名验证。

此版本按整句处理。TTS 必须完整接收并验证 WAV 后才输出，不提供流式模型推理。
仅接受 MRCP TTS `text/plain`；音色和模型沿用 HTTP 服务默认值。ASR 仅支持 `builtin:speech/transcribe` 转写，输出 NLSML。
未实现 SRGS、SSML、波形存档、GET/SET-PARAMS、自定义音色、选择性 STOP 请求 ID 列表及排队合成；不支持的方法返回错误，不伪装成功。
支持全通道 STOP、TTS PAUSE/RESUME/BARGE-IN-OCCURRED，以及 ASR START-INPUT-TIMERS。
上游 HTTP 停止后忽略迟到结果；真正的用户插话检测及业务轮次协调仍需控制器实现。

TTS 最大 25 MiB，文本最大 80 KiB。严格校验 WAV 容器长度、格式、音频帧完整性，拒绝其他采样率。
仅兼容既有 TTS 网关的标准 44 字节 WAV 头占位值：RIFF `0x7FFFFFBF`、data `0x7FFFFF9B`；普通截断文件不会被当作有效音频。
ASR 空文字映射为 no-match，服务失败映射为 error，无输入不会调用 HTTP。
VAD 是 PCM 能量与持续时间检测，需按实际电话音质调参，不代表噪声环境下的人声分类模型。

## 2. 构建与测试（Linux / amd64）

在此仓库根目录，用已有 WSLC 构建独立测试镜像：

```powershell
wslc build -f tests/http-plugins/Dockerfile -t local/unimrcp-http:1.8.0 .
wslc run --rm local/unimrcp-http:1.8.0
```

Docker 用户可将 `wslc` 换成 `docker`。测试镜像包含编译器和测试程序，适合开发验证，不是精简生产镜像。
测试使用容器内模拟 HTTP 接口、实际 UniMRCP Server、SIP/MRCP 控制连接和 RTP 音频，不会拨打真实电话或访问你的实际语音服务。

Linux 原生构建依赖：带 UniMRCP 补丁的 APR、匹配的 APR-util、Sofia-SIP、Autotools、libcurl >= 7.56、json-c >= 0.15。
系统自带 APR 缺少 `apr_pool_mutex_set`，不能直接替代官方配套版本。测试 Dockerfile 下载官方 `unimrcp-deps-1.6.0` 源码包，校验 SHA256 后把 APR/APR-util 安装到 `/opt/unimrcp-deps`；没有删除这项线程安全功能来绕过链接错误。
Debian 12 的基础包名和完整构建步骤见测试 Dockerfile。安装配套 APR 后的构建示例：

```sh
./bootstrap
./configure --prefix=/opt/unimrcp \
  --with-apr=/opt/unimrcp-deps --with-apr-util=/opt/unimrcp-deps \
  --with-sofia-sip=/usr/lib/x86_64-linux-gnu/pkgconfig/sofia-sip-ua.pc \
  --enable-http-tts-plugin --enable-http-asr-plugin --disable-umc
make -j2
make install
```

Sofia 路径应与实际系统架构和安装位置一致。Windows 检出的源码可能带 CRLF；容器构建脚本只在镜像中归一化源码换行。
插件默认不参与原版构建，需显式启用以上两个选项。CMake 提供 `ENABLE_HTTP_TTS_PLUGIN`、`ENABLE_HTTP_ASR_PLUGIN` 开关，依赖通过 pkg-config 查找；本次验证使用 Linux Autotools，CMake 和 Windows 原生构建未验证。

## 3. 启用 HTTP 配置

编译完成后，把 `conf/unimrcpserver-http.xml` 复制为安装目录的 `conf/unimrcpserver.xml`，保留原文件备份。
此示例默认只监听 `127.0.0.1`，适用于同机运行或 Linux host 网络容器：

| 配置 | 默认值 |
|---|---|
| SIP 信令 | 8060 / UDP、TCP |
| MRCP 控制 | 1544 / TCP |
| UniMRCP RTP | 25000–25100 / UDP |
| 协商音频 | L16 / 16000 Hz，单声道 |
| ASR HTTP | `http://127.0.0.1:8887/v1/asr/upload` |
| TTS HTTP | `http://127.0.0.1:8889/api/v1/tts` |

可修改 `<engine>` 内的 `url` 参数，或使用环境变量覆盖：

```sh
export UNIMRCP_HTTP_ASR_URL=http://实际可达的语音服务地址:8887/v1/asr/upload
export UNIMRCP_HTTP_TTS_URL=http://实际可达的语音服务地址:8889/api/v1/tts
/opt/unimrcp/bin/unimrcpserver -r /opt/unimrcp
```

云端的 `127.0.0.1` 不会指向你的 Windows 电脑。ASR/TTS 留在 Windows 时，先提供可达的私网地址或开发用反向隧道。
不同容器默认也不共享 loopback；使用 host 网络，或明确设置容器可达地址。不同机器部署时同步修改 SIP、RTP 的宣告地址。

可配置的插件参数：

| 参数 | 默认值 | 含义 |
|---|---|---|
| http-timeout-ms | 60000 | HTTP 总超时；连接超时固定 5000 ms |
| max-utterance-ms | 60000 | ASR 一轮收音的时长和内存上限，最多 120000 ms |
| no-input-timeout-ms | 15000 | 没检测到讲话的超时 |
| speech-complete-timeout-ms | 1000 | 讲话后的尾静音时长 |
| min-speech-ms | 120 | 连续有声达到此时长才触发 START-OF-INPUT |
| vad-threshold | 500 | PCM16 的 RMS 幅度阈值 |

示例限制每个引擎最多 10 个通道；这不是容量测试结论，也不会解除现有 Call 控制器的一通电话限制。
每个通道有独立 HTTP 工作线程和有界音频缓冲；音频回调不执行 HTTP、不分配内存。

## 4. FreeSWITCH 联调前提

FreeSWITCH 镜像还需编译安装 `mod_unimrcp` 及对应客户端依赖。仅启动此 Server 不会切换原有 Call 流程。
FreeSWITCH 的 MRCP profile 要指向 Server 的 **SIP 端口 8060**，不是 MRCP TCP 端口 1544；双方音频协商设为 `L16/99/16000`。
为 FreeSWITCH MRCP 客户端选择独立 SIP/RTP 端口，避开 Server 的 8060、1544、25000–25100 及原 FreeSWITCH 的通话 RTP 范围。

ASR 使用转写 URI，并设置 `define-grammar=false`，避免让客户端先发送本适配器未实现的 DEFINE-GRAMMAR。
识别参数可覆盖 no-input、speech-complete、start-input-timers；设 `start-input-timers=false` 时必须由客户端明确启动输入计时器。

建议验收顺序：TTS 单句播放 → ASR 单句识别 → 控制器接识别事件并调用对话引擎 → 多轮通话 → 超时/挂断/取消。
Call 侧后续还需调整 ESL 事件订阅、前端判停、SFTP 就绪检查和文件式 TTS 缓存；本次没有替换正在使用的业务链路。

## 5. 验证与来源

测试入口 `tests/http-plugins/test_integration.py` 包含 WAV/JSON 格式与大小校验、真实协议交互、HTTP 失败、无声、STOP、同通道复用、并行通道、HTTP 进行中关闭及正常服务关闭。
测试 HTTP 服务为模拟服务；通过测试不等同于真实模型和 FreeSWITCH 已完成联调。

2026-09-21 已在 Debian 12 / amd64 测试容器完成编译和协议集成验证：

- `httptts.so`、`httpasr.so` 及完整 UniMRCP Server 编译、安装成功。
- WAV 编解码、长度/采样率/占位头校验、NLSML 转义通过 AddressSanitizer / UndefinedBehaviorSanitizer 检查。
- SIP 建立会话、MRCP 请求/事件、TTS RTP 播放、ASR RTP 收音及 HTTP multipart 上传通过。
- HTTP/WAV/JSON 错误、空结果、无输入、STOP 后复用、并行通道、HTTP 期间关闭通道及服务正常退出通过。

验证时真实 ASR/TTS 模型、FreeSWITCH `mod_unimrcp` 和业务控制器未参与；实际部署仍按第 4 节联调。

- [UniMRCP 插件接口](https://www.unimrcp.org/manuals/html/PluginImplementationManual.html)
- [UniMRCP Server 配置](https://www.unimrcp.org/manuals/html/ServerConfigurationManual.html)
- [FreeSWITCH mod_unimrcp](https://github.com/freeswitch/mod_unimrcp)
