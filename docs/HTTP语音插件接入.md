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

### 3.1 使用 Docker Compose 启动已有镜像

适用于 Linux / amd64 服务器和 Docker Compose V2 及以上版本。仓库根目录的 `docker-compose.yml` 会覆盖测试镜像的默认命令，使用 `-w -o 1` 启动常驻服务，并把 HTTP 插件配置只读挂载到安装目录。无需重新构建镜像。

将以下文件按目录结构上传到服务器，例如 `/opt/unimrcp-http/`：

```text
unimrcp-http/
├── docker-compose.yml
├── .env.example
├── conf/
│   └── unimrcpserver-http.xml
└── dist/
    └── unimrcp-http.tar
```

镜像已导入时可省略 tar 文件和 `docker load`。首次部署在该目录执行：

```sh
docker load -i dist/unimrcp-http.tar
cp .env.example .env
vi .env
vi conf/unimrcpserver-http.xml
```

- `.env` 的 `UNIMRCP_IMAGE` 应与导入后的镜像名称和标签一致；Compose 只使用本地镜像，不自动拉取。
- `.env` 的 ASR/TTS URL 必须从服务器可达。host 网络下的 `127.0.0.1` 是服务器本机；模型仍在 Windows 上时改成可达的 Windows 地址。其他 bridge 网络容器需要通过已发布的宿主机端口或其他可达地址访问。
- XML 的 `<properties><ip>` 默认是 `127.0.0.1`，适用于同机且共享宿主机网络的 FreeSWITCH。跨机器或从 bridge 网络容器连接时，改成服务器网卡上的可达 IP；SIP/MRCP/RTP 默认继承这个地址。处于 NAT 后方时还需按实际网络设置宣告地址。
- HTTP 配置省略 `<sip-transport>`，使用当前 Debian 镜像支持的默认 UDP/TCP。不要恢复显式的 `udp,tcp` 列表：镜像内旧版 Sofia-SIP 的列表解析存在内存生命周期缺陷，诊断依据和已有部署的调整方法见 3.3 节。
- 使用 `network_mode: host`，无需 `ports` 映射。跨机器使用 MRCPv2 时，允许 FreeSWITCH 访问服务器的 `8060/TCP、UDP`、`1544/TCP`、`25000–25100/UDP`。配置还启用了 MRCPv1 的 `1554/TCP`，仅使用 MRCPv2 时无需对外放行此端口。

如果之前已用 `docker run --name unimrcp-http` 启动同一服务，先执行 `docker stop unimrcp-http` 释放端口，再启动 Compose。随后执行：

```sh
docker compose config --quiet
docker compose up -d
docker compose ps
docker compose logs --tail 100 -f unimrcp
```

日志应显示 HTTP 插件成功加载，且没有端口绑定或引擎初始化错误。容器运行状态只说明进程存活，仍需按第 4 节验证真实 ASR/TTS 和 FreeSWITCH 链路。

修改 `.env` 或 XML 后，重新创建容器以加载配置：

```sh
docker compose up -d --force-recreate
```

停止部署使用 `docker compose down`。服务配置了 `unless-stopped` 自动重启策略；Docker 服务随系统启动时可恢复运行中的容器。控制台日志由 Docker 轮转，每份最多 10 MiB、保留 3 份。

Compose 配置语义参见 [Docker Compose 服务配置](https://docs.docker.com/reference/compose-file/services/) 和 [环境变量插值](https://docs.docker.com/compose/how-tos/environment-variables/variable-interpolation/)。

### 3.2 服务器无法下载 GDB 时的离线诊断

在可联网的本机基于原镜像构建诊断镜像，安装 GDB，并检查 libc、Sofia-SIP、libcurl、json-c 和 OpenSSL 的包版本未被升级。这个镜像用于定位故障，不代表故障已修复。

```powershell
wslc build -f tests/http-plugins/Dockerfile.debug -t local/unimrcp-http:1.8.0-debug .
wslc save -o dist/unimrcp-http-1.8.0-debug.tar local/unimrcp-http:1.8.0-debug
```

镜像归档也可经 gzip 压缩为 `.tar.gz` 后上传，Docker 可以直接导入。服务器在 Compose 所在目录执行（文件名按实际上传的包调整）：

```sh
docker load -i unimrcp-http-1.8.0-debug.tar.gz
docker compose stop unimrcp
UNIMRCP_IMAGE=local/unimrcp-http:1.8.0-debug \
  docker compose run --rm --no-deps --cap-add SYS_PTRACE \
  unimrcp /usr/local/bin/unimrcp-debug \
  2>&1 | tee unimrcp-startup-backtrace.log
```

此命令复用服务器的 XML、环境变量和网络设置，GDB 所需软件已包含在镜像里，运行时禁用在线调试符号下载。发生 SIGSEGV 时输出各线程调用栈和已加载的动态库；如果没有提前崩溃，30 秒后中断并输出线程状态。此时出现 SIGINT 和 `timeout` 的 124 状态是诊断时限所致。GDB 自身退出码不能用来判定 UniMRCP 是否正常，需查看信号和调用栈。

### 3.3 SIP 初始化段错误的配置规避

2026-09-28 服务器 GDB 堆栈为 `nua_stack_init_transport → nta_agent_add_tport → tport_tbind → su_casematch`，崩溃发生在 SIP 初始化期间。镜像内 `libsofia-sip-ua0` 的版本为 `1.12.11+20110422.1+1e14eea~dfsg-6`。

核对 [Debian 对应版本源码](https://deb.debian.org/debian/pool/main/s/sofia-sip/sofia-sip_1.12.11+20110422.1+1e14eea~dfsg.orig.tar.xz) 和镜像二进制，发现 `nta_agent_add_tport` 解析逗号分隔协议时，局部数组 `tps[9]` 在声明的代码块结束后仍通过 `tports` 使用。该二进制甚至省略了列表末尾的 NULL 写入，导致随后可能读取无效指针；服务器故障指令正是 `su_casematch` 读取第二个字符串参数的位置。这个缺陷与本次堆栈吻合。[上游当前源码](https://raw.githubusercontent.com/freeswitch/sofia-sip/master/libsofia-sip-ua/nta/nta.c) 已将该数组放在函数作用域。

删除配置后的服务器诊断日志确认 SIP 绑定 URL 已变为 `sip:127.0.0.1:8060`，不再带 `transport=udp,tcp`。本次诊断期间未出现 SIGSEGV；结束时为诊断脚本预设的 SIGINT，两个 SIP 线程均已进入 `su_base_port_run / epoll_wait` 事件循环。后续常驻服务日志已出现 `MRCP Server Started`，并成功处理 SIP INVITE、MRCP 连接及 SPEAK 请求，确认启动故障已绕过。随后出现的 TTS `004 error` 已定位为仍访问旧模型端口；将 ASR/TTS 地址改为 5004/5005 后，服务器真实模型的协议链路验证通过，结果见第 5 节。

本仓库的 HTTP 配置通过省略 `<sip-transport>` 避开上述解析路径，保留默认 UDP/TCP。已部署服务器在 Compose 目录执行以下命令，继续使用原运行镜像，无需下载依赖或重新打包：

```sh
cp -a conf/unimrcpserver-http.xml "conf/unimrcpserver-http.xml.bak-$(date +%Y%m%d-%H%M%S)"
sed -i '/^[[:space:]]*<sip-transport>udp,tcp<\/sip-transport>[[:space:]]*$/d' conf/unimrcpserver-http.xml
UNIMRCP_IMAGE=local/unimrcp-http:1.8.0 docker compose up -d --force-recreate unimrcp
docker compose logs --tail 100 -f unimrcp
```

预期日志包含 `MRCP Server Started`，且不再循环重启。若仍段错误，用 3.2 节的离线诊断镜像重新采集堆栈。此配置规避不修改依赖库本身；其他配置若继续传入逗号分隔协议列表，仍可能触发该缺陷。

### 3.4 验证正在运行的 UniMRCP 完整链路

直接用 `curl` 请求 ASR/TTS HTTP 接口，只能检查上游模型，不能验证 UniMRCP 插件和 SIP/MRCP/RTP。`tests/http-plugins/check_running_server.py` 是针对已运行服务的协议客户端：通过 SIP 建立 TTS 会话，发送 MRCP SPEAK，接收并保存 RTP 音频；随后建立 ASR 会话，将同一音频通过 RTP 回送，等待 MRCP RECOGNITION-COMPLETE 和 NLSML 识别文字。模型 HTTP 请求由当前服务端插件发起。

将该脚本上传到服务器 Compose 目录，保存为 `check_running_server.py`，然后执行：

```sh
docker compose exec -T unimrcp python3 - < check_running_server.py
```

若服务器已有更新后的完整仓库，也可将输入重定向路径改为 `tests/http-plugins/check_running_server.py`。原镜像已包含 Python 和脚本复用的协议客户端，无需重建。脚本不启动另一套 UniMRCP 或模拟模型，不修改现有配置，不停止当前服务。默认连接容器内 `127.0.0.1:8060`，适用于当前 loopback / host 网络部署；IP 改为其他网卡地址的部署不适用本脚本。SIP 端口改变时可加 `--sip-port 端口`。

通过条件：TTS 收到非静音 RTP 音频且 SPEAK-COMPLETE 为 `000`；ASR 的 RECOGNITION-COMPLETE 为 `000`，并返回非空识别文字；最终进程返回 0。还需比较识别文字与输入句子，检查识别质量。默认测试句为“你好，这是一段语音测试。”，可用 `--text '测试句'` 修改；每阶段默认等待 90 秒，模型错误或超时会报告 FAIL 并返回非零。

合成音频保存在容器 `/tmp/unimrcp-check.wav`，可复制出来试听：

```sh
docker cp unimrcp-http-unimrcp-1:/tmp/unimrcp-check.wav ./unimrcp-check.wav
```

本地用原镜像、真实 UniMRCP 进程及模拟 HTTP 上游验证了此客户端的成功路径、慢 TTS、TTS HTTP 错误、ASR 非法响应；错误场景均返回非零，服务端保持运行。服务器执行时会使用其真实模型地址。该检查覆盖 UniMRCP 服务端链路，FreeSWITCH 客户端及实际电话音频仍按第 4 节联调。

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

### 2026-09-28 本地复验与服务器故障边界

使用本机 WSLC 中已有的 `local/unimrcp-http:1.8.0`（镜像 ID 前缀 `7ac177ed1f5f`）复验；本机 WSL2 内核为 `6.18.35.2-microsoft-standard-WSL2`。两个临时容器均使用 `--network none`，测试服务只通过各自容器的 loopback 通信，运行结束后自动清理。

| 检查 | 结果 |
|---|---|
| HTTP 配置、`stdbuf`、`-w -o 1` 常驻启动 | 出现 `MRCP Server Started`，15 秒限时结束后正常关闭；`timeout` 返回 124，表示到达测试时限 |
| WAV/PCM 校验、NLSML 转义及 ASan/UBSan | 通过 |
| 模拟 HTTP 服务下的 SIP/MRCP/RTP、ASR/TTS、错误和无输入处理 | 通过 |
| STOP、通道复用、并发、HTTP 期间关闭、服务正常退出 | 通过；完整集成测试返回 0 |
| 与服务器一致的 `host` 网络 | 未执行成功：本机 WSLC 明确返回“不支持主机模式网络” |

复验命令（PowerShell，仓库根目录）：

```powershell
wslc run --rm --network none local/unimrcp-http:1.8.0 sh -lc 'cp /src/conf/unimrcpserver-http.xml /opt/unimrcp/conf/unimrcpserver.xml && timeout --signal=TERM --kill-after=5s 15s stdbuf -oL -eL /opt/unimrcp/bin/unimrcpserver -r /opt/unimrcp -w -o 1 -l 7'
wslc run --rm --network none local/unimrcp-http:1.8.0
```

服务器报告的环境为 `4.18.0-193.el8.x86_64` / Docker `26.1.3`，启动退出码为 139，`OOMKilled=false`，绕过 `stdbuf` 后仍发生段错误。后续离线 GDB 已定位到 Sofia-SIP 的 `su_casematch`，详见 3.3 节。将本地 `nofile` 软硬限制都调为服务器的 `1048576` 后仍未复现 SIGSEGV；不能据此认定内核或文件句柄限制是根因。

省略显式协议列表后，使用原镜像挂载工作区更新后的 XML 和测试脚本，在 `--network none --ulimit nofile=1048576:1048576` 下完成回归，返回 0：TCP SIP OPTIONS 返回 200，UDP SIP 会话和全部 ASR/TTS、错误处理、并发、取消及正常关闭测试通过。测试日志为 `dist/unimrcp-default-transport-test.log`。服务器随后用诊断镜像复验，未再出现原启动段错误；常驻模式也已报告启动成功，详见 3.3 节。

真实链路首次验证返回 TTS `004 error`。容器内诊断确认，实际 TTS 地址仍是 `http://127.0.0.1:8889/api/v1/tts`，连接被拒绝（Errno 111）。将 `.env` 中的模型地址改为 ASR `http://127.0.0.1:5004/v1/asr/upload`、TTS `http://127.0.0.1:5005/api/v1/tts` 并重新创建容器后，用户回传的服务器测试输出如下：

```text
Checking existing UniMRCP at 127.0.0.1:8060; text: 你好，这是一段语音测试。
PASS: TTS SPEAK-COMPLETE 000; received 33675 samples via RTP; /tmp/unimrcp-check.wav
PASS: ASR RECOGNITION-COMPLETE 000; NLSML text: 你好，这是一段语音测试。
PASS: SIP -> MRCP -> server HTTP plugins -> RTP roundtrip. Compare recognized text with the input phrase.
```

本次验证经过正在运行的 UniMRCP 及真实 HTTP 模型：TTS 返回成功完成事件，通过 RTP 收到 33675 个 16 kHz 音频采样（约 2.10 秒）；同一音频经 RTP 回送 ASR 后，识别文本与输入句一致。服务器的 SIP/MRCP、HTTP 插件、真实模型及 RTP 单句往返链路已通过。没有产生新的运行镜像；FreeSWITCH 客户端、实际电话、多轮通话及长期稳定性不属于本次单句验证范围。

- [UniMRCP 插件接口](https://www.unimrcp.org/manuals/html/PluginImplementationManual.html)
- [UniMRCP Server 配置](https://www.unimrcp.org/manuals/html/ServerConfigurationManual.html)
- [FreeSWITCH mod_unimrcp](https://github.com/freeswitch/mod_unimrcp)
