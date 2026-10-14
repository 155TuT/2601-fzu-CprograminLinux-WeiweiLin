学号：待填写　　姓名：待填写　　专业：待填写　　班级：待填写

# 《Linux操作系统设计实践》实验四：综合应用

实验日期：2026 年 10 月 14 日　　选用方案：方案 4

## 一、实验环境

| 项目 | 实测环境 |
| --- | --- |
| 操作系统 | Ubuntu 24.04.2 LTS，WSL2，x86_64 |
| 内核 | 6.6.87.2-microsoft-standard-WSL2 |
| C 编译与构建 | GCC 13.3.0、GNU Make 4.3 |
| 严格编译参数 | `-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g` |
| CPU | AMD Ryzen AI 7 H 350，8 核 16 线程 |
| 独立 GPU | NVIDIA GeForce RTX 5060 Laptop GPU，约 8 GB 显存 |
| WSL GPU 通路 | `/dev/dxg`、Windows 提供的 CUDA 驱动接口 |
| 模型 | Qwen3.5-4B 原模型的 Q5_K_M GGUF，文件 3,143,656,608 字节 |
| 推理引擎 | llama.cpp b11429，commit `d81235049384534c167caea52b85a694f6103d14` |
| CUDA 运行库 | 官方随该版本提供的 CUDA 13.4 用户态运行包，解包到项目 `.runtime/` |
| 本地推理参数 | 全层 GPU，2 个推理槽，每槽 16384 token，新轮最多 4096 个新 token/尝试；已有轮次使用冻结配置 |
| Python | Python 3.12，推理适配和验证只依赖标准库 |

以下命令均在 `homework/task4` 目录执行。硬件识别和运行版本见 [硬件日志](logs/model-hardware.txt) 与 [模型说明](docs/local-model.md)。首次下载访问 GitHub/Hugging Face；模型推理只访问本机回环地址。

## 二、系统开发情况介绍

### 2.1 需求与完成范围

依据实验指导书 PDF 第 7–9 页（印刷页码 5–7）的方案 4：服务端、客户端与独立生成程序分离，支持多个客户端同时提问、边生成边显示、文件和日志隔离；文本由本地小语言模型生成，同一连接的新问题携带此前对话，不调用外部大模型 API。模型回答质量不作为本实验的核心考察项。

| 要求 | 对应实现 |
| --- | --- |
| C/Linux 系统程序 | C 客户端与服务端，TCP、fork/exec、poll、信号、文件同步与锁 |
| 独立生成程序 | 每轮独立 Python 模型适配进程，通过本机 llama-server 生成 |
| 本地 Qwen3.5-4B | 固定版本 GGUF，在 WSL CUDA 上运行，无远程推理调用 |
| 并发输出与隔离 | 每连接独立 worker，独立会话、请求、journal 和日志；共享模型权重、两个推理槽 |
| 同连接多轮上下文 | 按顺序读取本会话已完成轮次，构造结构化 user/assistant 消息 |
| 流式输入输出 | 分帧 INPUT，在 COMMIT 后推理；SSE 内容转为 THINK/DELTA |
| 思考期间打断 | 同时监听终端与 TCP，`/stop`、Ctrl+C 发 CANCEL |
| 纠偏 | `/steer TEXT` 持久保存要求，旧轮确认终结后合并原问题并提交新轮 |
| 低速与长首 token | 工作进程约每秒心跳；心跳不计入输出游标 |
| 意外中断恢复 | 原始生成 journal、正式回答缓存、客户端 checkpoint，冻结前缀续写 |
| 日常会话管理 | 新建、切换、软删除、正文归档及失败后的 `/retry` |

输入按行收集后分帧上传，模型不会推理未提交的按键。系统处理文字对话，没有工具执行、自动改文件或视觉输入功能。

### 2.2 模型与硬件方案

NVIDIA 工具识别出 RTX 5060 Laptop，CUDA 二进制完成设备枚举与全层模型加载；GPU 和 CPU 均完成短基准。

| 本机基准 | 生成速度 | 样本说明 |
| --- | ---: | --- |
| Q5_K_M / CUDA | 80.52 ± 0.52 token/s | 64 个生成 token，3 次短测 |
| Q4_K_M / CUDA | 86.34 ± 1.57 token/s | 相同长度的 3 次短测 |
| Q5_K_M / CPU | 14.78 token/s | 16 个生成 token，单次短样本 |

原始记录：[Q5 GPU](logs/model-q5-gpu-raw.txt)、[Q4 GPU](logs/model-q4-gpu-raw.txt)、[Q5 CPU](logs/model-q5-cpu-only-raw.txt)。CPU 样本更短，只说明本机 CPU 可运行及大致速度差异；这些数字不能代表长期满载或并发吞吐，token/s 也不等于中文字符/秒。

Q5 比 Q4 约多 0.4 GB 模型文件。每槽 16k、双槽运行的容量测量中全机显存占用约 5921/8151 MiB，包含桌面等已有占用，因此采用 Q5；未进行回答质量基准。模型常驻进程避免每轮重复加载权重，课程生成进程仅负责模型适配。两槽同时处理各 5529-token 提示时，分别在 3.768/4.040 秒以 EOS 完成，未截断上下文，见 [容量实测](logs/model-context-16k.txt)。该项测量使用 2048-token 输出预算，默认新轮预算为 4096。

已确认本机可以加载所选模型并通过本地推理服务完成对话。固定版本、启动参数和上游来源见 [模型说明](docs/local-model.md)。采样参考 [Qwen 模型卡](https://huggingface.co/Qwen/Qwen3.5-4B)，本项目的上下文和生成预算属于本机交互配置。

### 2.3 模块与调用

```text
C client A ──TCP── C worker A ──socketpair── Python adapter A ──HTTP/SSE──┐
      │                │                        │                     │
 checkpoint A      session A             raw generation A              ├─ llama-server ─ CUDA GPU
C client B ──TCP── C worker B ──socketpair── Python adapter B ──HTTP/SSE──┘
      │                │                        │
 checkpoint B      session B             raw generation B
```

| 位置 | 作用 |
| --- | --- |
| `include/task4/` | 协议、生成进程、会话/checkpoint、文件工具、UTF-8 公共接口 |
| `src/server/`、`src/client/main.c` | 连接状态机、并发、交互控制、持久显示 |
| `src/client/terminal.c`、`src/client/state.c` | UTF-8 编辑、键鼠输入、会话锁、列表、归档与软删除 |
| `src/client/view.c`、`src/client/screen.c`、`src/client/status.c` | 可重排 TUI、指标解析、行差分和非阻塞输出 |
| `src/net/`、`src/storage/` | 帧收发、服务发现、终态与恢复记录、文件同步 |
| `src/backend/process.c`、`src/backend/model.c` | 生成子进程生命周期、模型配置快照 |
| `src/model/` | 上下文、官方模板、SSE、思考解析、原始日志和指标 |
| `config/`、`scripts/model/`、`deps/` | 推理设置、固定依赖安装与启动、许可证 |
| `scripts/server/dashboard.py` | 只读会话看板，显示锁状态和带时效的指标 |
| `tests/`、`logs/`、`screenshots/` | 可控功能检查、真实模型测试与白底截图 |

公共头文件与实现分离，源文件按职责组织，Makefile 的 `build/` 与源目录对应。模型权重与运行库放在 `.runtime/`，不提交到版本库。目录与启动命令见 [README](README.md)。

<!-- code-stats:start -->
公共头文件 12 个，C 源文件 14 个，模型 Python 模块 5 个，共 6067 行（含注释与空行）。统计不包括 Makefile、安装/测试辅助脚本和文档。

| 源文件 | 行数 |
| --- | ---: |
| `include/task4/backend.h` | 35 |
| `include/task4/checkpoint.h` | 24 |
| `include/task4/client_state.h` | 51 |
| `include/task4/discovery.h` | 38 |
| `include/task4/files.h` | 12 |
| `include/task4/protocol.h` | 58 |
| `include/task4/screen.h` | 21 |
| `include/task4/status.h` | 16 |
| `include/task4/store.h` | 31 |
| `include/task4/terminal.h` | 49 |
| `include/task4/text.h` | 22 |
| `include/task4/view.h` | 30 |
| `src/backend/model.c` | 59 |
| `src/backend/process.c` | 82 |
| `src/client/main.c` | 1031 |
| `src/client/screen.c` | 161 |
| `src/client/state.c` | 574 |
| `src/client/status.c` | 134 |
| `src/client/terminal.c` | 579 |
| `src/client/view.c` | 446 |
| `src/net/discovery.c` | 405 |
| `src/net/protocol.c` | 316 |
| `src/server/main.c` | 385 |
| `src/storage/checkpoint.c` | 303 |
| `src/storage/files.c` | 37 |
| `src/storage/session.c` | 235 |
| `src/model/adapter.py` | 397 |
| `src/model/discovery.py` | 148 |
| `src/model/journal.py` | 126 |
| `src/model/stream.py` | 54 |
| `src/model/transport.py` | 208 |
<!-- code-stats:end -->

### 2.4 主要系统调用

| 接口 | 用途 |
| --- | --- |
| `socket/bind/listen/accept/connect` | 课程客户端与服务端的 TCP 通信 |
| `socketpair` | 模型事件帧的进程间传输 |
| `poll/send/recv`、`pidfd_open` | 生成数据、取消及子进程退出；完成部分收发 |
| `fork/execvp/dup2` | 每连接 worker、每轮适配进程、标准流隔离 |
| `sigaction/kill/waitpid/prctl` | 信号控制、子进程回收、父进程崩溃清理 |
| `open/read/write/close`、标准 I/O | 问题、回答、原始前缀、配置与日志读写 |
| `flock` | 会话与客户端 checkpoint 独占 |
| `tcgetattr/tcsetattr/ioctl` | 终端编辑模式、设置恢复和窗口尺寸 |
| `fsync/rename/renameat2/mkstemp/mkdtemp` | 落盘、原子发布、软删除与恢复队列 |
| `clock_gettime` | 单调时钟期限 |
| `getrandom`、`/dev/urandom` | 会话 token 与服务实例标识 |

### 2.5 流式、历史与恢复

TCP 报头由五个网络序 uint32 组成：magic、type、request_id、offset、length，共 20 字节。问题通过 BEGIN/INPUT/COMMIT 上传；THINK 传输思考，DELTA 每帧传输一个完整 UTF-8 码点，END 确认完成。STATUS 更新显示属性，HEARTBEAT 维护网络等待期限；两者均不进入对话正文或模型历史。

模型适配器用 `/apply-template` 冻结聊天模板，再用 `/completion` 获取 SSE。原始片段先追加 journal 并同步，再解析思考与回答；C worker 先写 `response.txt` 再发送，客户端先写 checkpoint 再显示。服务端数据可以领先客户端，重连时重放缺口；模型继续部分使用冻结提示词和已落盘的原始生成前缀，不重新生成已保存文本。该机制不保存 GPU KV cache 或采样随机状态，新的后缀可能不同于不中断的运行。

下一轮的历史只包含当前会话已完成轮次的问题和正式回答，不包含取消轮次、未完成回答、思考或状态文本。续写同一未完成轮时，原始前缀可能含未闭合思考，两种输入用途不同。模型身份、模板或构建不匹配以及运行容量缩小时拒绝续写；仅增大容量仍沿用本轮冻结设置。

正常结束且生成进程成功退出后才写 `done`；输出预算用尽、上下文不足、HTTP 异常或缺少完成事件的 EOF 保留未完成状态。显式取消先回收生成子进程、保存 `cancelled` 再确认；意外断线保留续写资格。纠偏按“持久保存要求 → 取消确认 → 新请求”处理，避免丢失用户已提交的修改。完整契约见 [架构](docs/architecture.md) 和 [协议](docs/protocol.md)。

## 三、程序运行演示说明

### 3.1 编译与功能验证

```sh
make -j4 CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g'
make verify
make -q
```

自动验证使用真实 C 程序、隔离临时状态、可控模型事件或本机 HTTP 测试服务，不需要显卡、模型或联网。覆盖协议分帧、恢复与取消、服务发现、终端输入、会话管理、模型适配及显示缓冲。严格编译和完整自动验证均通过，结果见 [当前检查记录](logs/verify-current.txt)；可控对端用于确定性功能检查，真实 Qwen 的推理结果另行记录。

### 3.2 本地模型与并发多轮

首次执行 `python3 scripts/model/setup.py`，随后在三个终端分别运行：

```sh
bash scripts/model/serve.sh gpu
./bin/stream_server --state state/model-server
./bin/stream_client
```

两层服务默认自动分配端口。模型启动器复用验证匹配的健康实例，或在新实例就绪后发布回环地址；C 服务端把地址与状态目录锁、启动实例标识绑定。无参数客户端选择唯一活动服务；存在多个候选时需用 `--server-state DIR` 指定。恢复根据 checkpoint 的 token 找到原状态目录，端口可以改变；推理参数和模板保持本轮快照。

`python3 scripts/model/smoke.py` 启动两个真实 C 客户端，分别设置代号并在同一连接第二轮回忆，核对会话 token、结构化历史和正式回答。[真实模型日志](logs/model-smoke.txt) 中两路分别回答 MAPLE、CEDAR，15.530 秒内完成；使用自动端口的独立测量耗时 21.825 秒，见 [自动发现实测](logs/model-smoke-auto-ports.txt)。这是相应配置与输入的实测，不是速度上界或模型质量保证。

当前客户端的全 stdio PTY 实测完成两轮，并正确回忆代号 SPRUCE。两轮使用 `attempt_max_tokens=4096`、`context_tokens=16384`；新建后切回原会话，两轮正文均可恢复。包含截图采集和会话切换的本次检查耗时 19.013 秒，见 [真实模型 TUI 记录](logs/model-tui-4096.txt)。下图显示真实模型回答，思考仅在本地隐藏。

![当前客户端的本地 Qwen 多轮对话](screenshots/23-model-current-96x28.png)

### 3.3 取消、纠偏与续传

生成期间输入 `/stop` 或按 Ctrl+C 取消本轮；输入 `/steer 请只用中文，并把最终回答限制为一句话` 则先取消，再携带原问题和修改要求启动下一轮。取消后的部分回答留在磁盘，但不进入后续模型历史。END 和 CANCEL 交叉到达时，以服务端保存的终态为准，重复确认不会成为新一轮数据。

[真实模型控制记录](logs/model-controls.txt) 中，`/steer` 和 `/stop` 的单次确认耗时分别为 8.363 ms、5.025 ms，纠偏回答为 LINUX；这些不是延迟上界。该记录还使用独立 `thinking=false` 配置，在保存 8 字节正文后通过 SIGTERM 中断，恢复后从同一原始前缀继续，最终正文为 140 字节。日常默认配置为 `thinking=true`。

![当前客户端取消思考后提交纠偏要求](screenshots/24-controls-96x28.png)

图中内容由可控对端提供，展示当前客户端的取消与纠偏界面；真实模型的控制结果见上面的运行记录。

用 `./bin/stream_client --list-sessions` 查找会话，再以 `--resume 路径/checkpoint.ckpt` 恢复。TUI 中可直接 `/retry`；`--resume-only` 恢复当前轮后退出，`--cancel-pending` 结束待完成轮。客户端 checkpoint 与服务端原状态目录都必须保留。已完成轮只需重放缓存，不必重新推理。

### 3.4 终端输入与会话管理

每个终端运行 `./bin/stream_client`，自动创建独立 `session-XXXXXX` 目录。TUI 在顶部显示会话，中间显示问题、灰色思考与回答，底部保留指标和输入区。支持 UTF-8 码点编辑、方向键、输入历史及 bracketed paste；粘贴换行合并为空格，按 Enter 提交。输出与窗口缩放不清除草稿，生成期间提交文字作为纠偏，完成后提交作为下一轮问题。

滚轮每次滚动对话约 3 行，PgUp/PgDn 翻页，↑/↓ 仅浏览输入历史。向上阅读时停止自动跟随，回到底部后恢复。`/sessions` 列表、`/new` 新建、`/switch ID|FILE` 切换；生成中切出保留原轮断点，切回用 `/retry` 继续；无效、占用或当前会话目标不打断原生成，删除其他闲置会话也不影响当前输出。`/delete [ID]` 将受管会话目录移入 `.trash`，保留服务端记录；拒绝外部路径、符号链接或另一客户端占用的会话。

完整 TUI 把完成或取消的轮次归档到 `<checkpoint>.history/`，切换时恢复多轮正式正文和输入历史。未归档的更早正文不从服务端下载。模型与网络错误留在当前对话中，输入区继续可用；批处理保持失败退出码。同一 checkpoint 只允许一个写入者，暂停进程仍持锁，应回原终端 `fg` 后正常退出，不能删除 `.lock` 强行解锁。

绘制约每 34 ms 合帧，只发送变化的屏幕行，通过独立非阻塞终端描述符写出。在 140×38、32 次单字节增量的受控负载中，ANSI 输出为 3147 字节；终端停止读取超过两秒时，客户端仍接收 END 并保存完成断点，见 [显示量测量](logs/render-benchmark.json)。这属于终端输出测量，不代表模型速度。

![浅色宽窗口](screenshots/14-tui-light-140x38.png)

![窄窗口](screenshots/16-tui-light-60x18.png)

![会话切换与多轮恢复](screenshots/21-conversations-96x28.png)

![窄窗口中报错后继续输入](screenshots/22-conversation-error-60x18.png)

本节四张图均使用当前编译的 C 客户端，文本为界面夹具。每张图来自全 stdio PTY 的实际完整重绘，原样重放到相同尺寸的白底 xterm 后截取；没有在 VS Code 集成终端实测。报告内全部客户端截图来自同一编译版本，来源、尺寸与采集命令见 [截图记录](logs/report-screenshots-current.json)。深色主题和字体限制见 [TUI 说明](docs/tui.md)。

### 3.5 只读看板与资源边界

```sh
python3 scripts/server/dashboard.py --state state/model-server
```

看板每秒读取会话文件及锁状态，显示连接、阶段、token、速度和上下文估计，不获取业务锁、不写会话。默认隐藏问题与思考正文；离线指标具有时效标记，`ctx~` 是输入加已生成 token 的估计，不是 GPU KV cache 精确占用。

上下文窗口、生成预算和网络超时是三个不同限制：每槽 16384 token 容纳模板、历史、问题和生成；每次尝试最多 4096 个新 token，思考与正文共用，实际预算还受剩余上下文限制；客户端默认等待合法完整网络帧的期限为 10000 ms。收到合法 THINK/DELTA/STATUS/HEARTBEAT 刷新网络等待，键盘或绘制不刷新。期限到达时先处理已到达数据，不完整帧仍受总期限限制。

模型进度事件的占位 token 不计入生成量，真实空文本生成 token 计数；有效原生累计计数及终止事件的 `timings.predicted_n` 用于校准。模型有关历史、能力或知识日期的自述不作为程序状态依据。程序当前没有独立思考预算、重复检测、自动直接回答或系统能力提示；增加预算不保证消除长思考。详细说明见 [上下文与指标](docs/local-model.md#上下文指标与资源边界)。

## 四、实验结果总结

系统实现本地模型的并发流式输出、会话隔离、多轮上下文、交互取消与持久恢复。C 管理连接、进程、信号、文件与 TUI，Python 标准库模块处理本地推理协议和原始生成日志；模型常驻进程共享权重，适配进程按轮次独立运行。

边界包括：文本交互不支持视觉输入或工具调用；终端编辑单位为 UTF-8 码点而非完整字素簇；TUI 显示缓存约 1 MiB、128 条记录，淘汰显示内容不删除磁盘或模型历史。模型无自动摘要，长历史可能用尽冻结容量；输出预算用尽时保留前缀，续写仍受剩余容量限制。恢复不保存 KV cache 和随机采样器，不能保证中断前后与单次连续推理逐 token 相同。两个推理槽不保证每路达到单路基准速度。协议面向本机课程实验，没有 TLS 或独立账户认证。

## 五、编程工作总结

本实验把进程管理、进程间通信、TCP 通信与文件操作组合为持续交互系统。fork/exec 确定工作边界，锁确定状态修改者，分帧和完整收发确定字节流的消息边界。每一层分别记录已持久化的数据，恢复游标才具有明确含义。

流式显示、模型生成和持久化各有自己的节奏。正文交付采用先保存后发送，终端显示使用有界非阻塞缓冲；思考与正文分开计游标，显示指标不混入下一轮历史。正常完成、用户取消、意外断线对应不同状态，使后续操作能区分“继续这一轮”和“开始下一轮”。

纠偏必须先保存用户要求，再取消并等待确认，最后提交新轮；输入和输出同时发生时，顺序比单个接口是否可调用更重要。验证也应区分可控协议检查、真实终端输入与真实模型生成，它们分别支持不同结论。短基准用于描述本机样本，不代表模型质量或所有负载下的表现。

## 六、注释源码附录

下面内容由 `python3 scripts/render_report.py` 按当前源文件生成，包含 C 公共头文件、实现、Python 模型模块、Makefile 和配置。安装、测试和演示脚本在仓库对应目录中提供。个人信息由提交者填写。

<!-- source-appendix -->

### 附录 1：include/task4/backend.h

```c
#ifndef TASK4_BACKEND_H
#define TASK4_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

struct t4_generation {
    pid_t pid;
    int fd; /* Nonblocking socket receiving the adapter's framed stdout. */
};

/*
 * prepare snapshots model configuration for a durable turn; start may be
 * called again after disconnect. Model output uses typed T4 frames, and its
 * own raw journal preserves the generated prefix across fresh inference jobs.
 */
struct t4_backend {
    const char *name;
    int (*prepare)(const char *model_config, const char *turn_dir);
    /* runtime_config supplies transport routing only; a model turn keeps its
     * frozen prompt, sampling options and limits across server port changes. */
    int (*start)(const char *adapter, const char *turn_dir,
                 const char *context_path, const char *runtime_config,
                 uint32_t offset,
                 struct t4_generation *generation);
};

extern const struct t4_backend t4_model_backend;

/* Closes the pipe, terminates and reaps the child, and sets both fields to -1. */
void t4_generation_stop(struct t4_generation *generation);
int t4_generation_spawn(char *const argv[], const char *log_path,
                        struct t4_generation *generation);

#endif
```

### 附录 2：include/task4/checkpoint.h

```c
#ifndef TASK4_CHECKPOINT_H
#define TASK4_CHECKPOINT_H
#include "task4/protocol.h"

struct t4_checkpoint {
    char session[33];
    /* done: 0 pending, 1 completed, 2 explicitly cancelled. */
    uint32_t request, offset, prompt_length, done;
    uint32_t file_lines;
    unsigned char prompt[T4_MAX_PROMPT + 1];
    unsigned char *answer;
    uint32_t saved_request, saved_offset, saved_done;
    int journal_loaded;
    /* A durable steering sidecar belongs to queued_request in this session. */
    uint32_t queued_request, queued_length;
    unsigned char queued_prompt[T4_MAX_PROMPT + 1];
};
int t4_checkpoint_load(const char *path, struct t4_checkpoint *c);
int t4_checkpoint_save(const char *path, struct t4_checkpoint *c);
int t4_checkpoint_lock(const char *path);
int t4_checkpoint_queue(const char *path, struct t4_checkpoint *c,
                        const unsigned char *prompt, uint32_t length);

#endif
```

### 附录 3：include/task4/client_state.h

```c
#ifndef TASK4_CLIENT_STATE_H
#define TASK4_CLIENT_STATE_H

#include <stddef.h>
#include <limits.h>
#include "task4/checkpoint.h"

struct t4_client_session {
    char id[15], path[PATH_MAX], status[24];
    long owner;
};
typedef int (*t4_client_session_fn)(const struct t4_client_session *, void *);
typedef int (*t4_client_history_fn)(const struct t4_checkpoint *, void *);

/* Create private parents and a unique session-XXXXXX/checkpoint.ckpt path.
 * The checkpoint itself does not yet exist. Returns 0, or -1 with errno. */
int t4_client_state_new(const char *session_dir, char *out, size_t capacity);

/* List managed checkpoint paths and lock/file status without loading, changing
 * or revealing their question/answer contents. Missing directories are empty. */
int t4_client_state_list(const char *session_dir);

/* Stable ID ordering, callbacks return 0 to continue or -1 to abort. A missing
 * directory is empty. Enumeration never loads or changes the checkpoint. */
int t4_client_state_scan(const char *session_dir, t4_client_session_fn visit, void *opaque);

/* Resolve a managed session-XXXXXX ID or existing checkpoint path. The caller
 * must then acquire t4_checkpoint_lock and load; resolving grants no lock. */
int t4_client_state_resolve(const char *session_dir, const char *id_or_path,
                          char *out, size_t capacity);

/* Soft-delete an unlocked managed ID into session_dir/.trash. Checkpoint and
 * lock inode move together, never unlinking the lock. Refuses arbitrary paths,
 * symlinks and active sessions. Returns the recoverable directory in out. */
int t4_client_state_delete(const char *session_dir, const char *id,
                         char *out, size_t capacity);

/* A compact, checksummed display archive alongside path as path+.history.
 * Only terminal prompt/answer are stored; status, thinking and steering are
 * excluded. Caller holds the active checkpoint lock. Same turn is idempotent;
 * conflicting content returns EEXIST and is never replaced. */
int t4_client_history_archive(const char *path, const struct t4_checkpoint *c);
/* Visit completed/cancelled archives in request order. session must match the
 * current checkpoint. Each borrowed checkpoint is valid during its callback. */
int t4_client_history_scan(const char *path, const char *session,
                         t4_client_history_fn visit, void *opaque);

/* Explain a lock/load failure using the caller's saved errno. */
void t4_client_state_lock_error(const char *path, int error);

#endif
```

### 附录 4：include/task4/discovery.h

```c
#ifndef TASK4_DISCOVERY_H
#define TASK4_DISCOVERY_H

#include <limits.h>
#include <stdint.h>

struct t4_endpoint {
    char host[16];
    uint16_t port;
    char state[PATH_MAX];
    char instance[33];
};

/* Only the master retains the lease. Workers close their inherited copies. */
struct t4_service {
    int lease_fd, state_fd, registry_fd;
    uint64_t device, inode;
    char state[PATH_MAX], entry[80], instance[33];
    int published;
};

/* 0: newly owned lease; 1: matching live service in existing; -1: error.
 * EBUSY means another master owns the state but has not published its endpoint. */
int t4_service_acquire(const char *state, struct t4_service *service,
                       struct t4_endpoint *existing);
int t4_service_publish(struct t4_service *service, const char *registry,
                       const char *host, uint16_t port);
void t4_service_child_close(struct t4_service *service);
void t4_service_close(struct t4_service *service);

/* explicit_state bypasses registry scanning. token filters saved sessions.
 * ENOENT: none live; EEXIST: ambiguous. Neither chooses an arbitrary server. */
int t4_service_discover(const char *registry, const char *explicit_state,
                        const char *token, struct t4_endpoint *endpoint);
/* Recheck the state lease and instance before the same-connection wire probe. */
int t4_service_validate(const struct t4_endpoint *endpoint);

#endif
```

### 附录 5：include/task4/files.h

```c
#ifndef TASK4_FILES_H
#define TASK4_FILES_H

#include <limits.h>
#include <stddef.h>

/* 两侧共有的文件工具，不依赖服务端会话或生成后端。 */
int t4_path(char result[PATH_MAX], const char *directory, const char *name);
int t4_write_all(int fd, const void *data, size_t length);
int t4_directory_sync(const char *directory);

#endif
```

### 附录 6：include/task4/protocol.h

```c
#ifndef TASK4_PROTOCOL_H
#define TASK4_PROTOCOL_H

#include <stdint.h>

/* Five network-order uint32_t fields form the fixed 20-byte header. */
#define T4_MAGIC UINT32_C(0x54345331)
#define T4_MAX_FRAME UINT32_C(4096)
#define T4_MAX_PROMPT UINT32_C(4096)
#define T4_MAX_RESPONSE UINT32_C(16777216)

enum t4_frame_type {
    T4_HELLO = 1,
    T4_WELCOME = 2,
    T4_BEGIN = 3,
    T4_INPUT = 4,
    T4_COMMIT = 5,
    T4_DELTA = 6,
    T4_END = 7,
    T4_ERROR = 8,
    T4_HEARTBEAT = 9,
    T4_THINK = 10,       /* 独立思考流，不推进 final answer 的恢复游标。 */
    T4_CANCEL = 11,      /* 结束当前轮次；断线本身不等于取消。 */
    T4_CANCELLED = 12,
    T4_STATUS = 13,
    T4_PROBE = 14,      /* Optional read-only service identity check, before HELLO. */
    T4_SERVICE = 15     /* 32-byte instance nonce; never a session/history record. */
};

struct t4_frame {
    uint32_t type;
    uint32_t request_id;
    uint32_t offset;
    uint32_t length;
    unsigned char data[T4_MAX_FRAME + 1U];
};

/*
 * A complete frame has a monotonic-clock deadline of idle_ms milliseconds.
 * Partial transfers are completed internally. EINTR is returned to the caller
 * so a signal handler can ask its event loop to stop. Payloads are raw bytes;
 * applications must check their own text and state-machine requirements.
 */
/* Returns 1 for a complete send, and -1 on error. */
int t4_send(int fd, const struct t4_frame *frame, int idle_ms);
/* Returns 1 for a frame, 0 only for EOF between frames, and -1 on error. */
int t4_recv(int fd, struct t4_frame *frame, int idle_ms);

/* Returned sockets are nonblocking, close-on-exec, and use TCP_NODELAY. */
int t4_connect(const char *host, uint16_t port, int idle_ms);
/* IPv4 only; a NULL bind address selects 127.0.0.1. Port 0 selects a free port. */
int t4_listen(const char *bind_ipv4, uint16_t port, uint16_t *actual_port);

/* Strict unsigned decimal parsing: no signs, whitespace, or trailing text. */
int t4_uint(const char *text, uint32_t minimum, uint32_t maximum,
            uint32_t *value);

#endif
```

### 附录 7：include/task4/screen.h

```c
#ifndef TASK4_SCREEN_H
#define TASK4_SCREEN_H

#include <stddef.h>

/* Internal, bounded nonblocking screen transport. One immutable frame may be
 * in flight; callers keep newer transcript state until it has drained. */
struct t4_screen;
struct t4_screen *t4_screen_open(void);
void t4_screen_close(struct t4_screen *screen);
int t4_screen_resize(struct t4_screen *screen, unsigned rows, unsigned columns);
void t4_screen_invalidate(struct t4_screen *screen);
int t4_screen_row(struct t4_screen *screen, unsigned row, const char *style);
int t4_screen_append(struct t4_screen *screen, const void *data, size_t length);
void t4_screen_cursor(struct t4_screen *screen, unsigned row, unsigned column);
int t4_screen_submit(struct t4_screen *screen);
int t4_screen_drain(struct t4_screen *screen);
int t4_screen_busy(const struct t4_screen *screen);
int t4_screen_control(struct t4_screen *screen, const char *text);

#endif
```

### 附录 8：include/task4/status.h

```c
#ifndef TASK4_STATUS_H
#define TASK4_STATUS_H

#include "task4/terminal.h"

/* STATUS is presentation data. Parsing never changes a checkpoint or prompt. */
struct t4_client_status {
    struct t4_ui_status ui;
    char phase[32], model[128];
};

void t4_status_init(struct t4_client_status *status);
/* Merge known top-level properties atomically; leave old values on failure. */
int t4_status_update(struct t4_client_status *status, const char *json, size_t length);

#endif
```

### 附录 9：include/task4/store.h

```c
#ifndef TASK4_STORE_H
#define TASK4_STORE_H

#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include "task4/backend.h"
#include "task4/files.h"

struct t4_session {
    char token[33];
    char directory[PATH_MAX];
    char context[PATH_MAX];
    int lock_fd;
    int log_fd;
};

/* 同一会话只允许一个连接持有锁；空 token 创建独立会话。 */
int t4_session_open(const char *root, const char *token, struct t4_session *s);
void t4_session_close(struct t4_session *s);
int t4_event(struct t4_session *s, const char *event, uint32_t request, uint32_t offset);
/* 检查顺序及重试 prompt，返回持久回复长度、完成标志和本轮目录。 */
int t4_turn_open(struct t4_session *s, uint32_t request,
                 const unsigned char *prompt, uint32_t length,
                 char directory[PATH_MAX], uint32_t *cached, int *done,
                 const char *model_config, const struct t4_backend *backend);
int t4_turn_done(const char *directory);
/* A cancelled turn is terminal but never included in later model history. */
int t4_turn_cancel(const char *directory);

#endif
```

### 附录 10：include/task4/terminal.h

```c
#ifndef TASK4_TERMINAL_H
#define TASK4_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

struct t4_terminal;

/* Borrowed strings are copied immediately; one cached status belongs to a turn. */
struct t4_ui_status {
    const char *phase;
    const char *model;
    uint32_t prompt_tokens, context_tokens, generated_tokens;
    double tokens_per_second, elapsed_seconds;
};

/* NULL leaves legacy I/O untouched unless both stdin and stderr are terminals. */
struct t4_terminal *t4_terminal_open(void);
/* Nonblocking: 0 edits/no input, 1 line, 2 EOF, -1 error, -2 invalid long line. */
int t4_terminal_read(struct t4_terminal *terminal, unsigned char line[4097],
                     uint32_t *length);
int t4_terminal_pending(const struct t4_terminal *terminal);
/* Output and prompt/resize updates preserve the unsent draft and its cursor. */
int t4_terminal_prompt(struct t4_terminal *terminal, const char *prompt);
int t4_terminal_write(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_resize(struct t4_terminal *terminal);
int t4_terminal_session(struct t4_terminal *terminal, const char *session, const char *model);
int t4_terminal_begin_turn(struct t4_terminal *terminal, uint32_t request,
                           const char *prompt, uint32_t resume_offset);
int t4_terminal_thinking(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_answer(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_status(struct t4_terminal *terminal, const struct t4_ui_status *status);
int t4_terminal_end_turn(struct t4_terminal *terminal, const char *state, const char *summary);
int t4_terminal_theme(struct t4_terminal *terminal, int dark);
int t4_terminal_thinking_visible(struct t4_terminal *terminal, int show);
/* Call tick every event-loop pass with a poll cap of 33 ms; flush forces paint. */
int t4_terminal_tick(struct t4_terminal *terminal);
int t4_terminal_flush(struct t4_terminal *terminal);
/* Switch conversation while preserving theme and thinking visibility. */
int t4_terminal_reset(struct t4_terminal *terminal);
/* Seed keyboard history from a restored user prompt, without submitting it. */
int t4_terminal_history(struct t4_terminal *terminal, const char *prompt);
void t4_terminal_clear_input(struct t4_terminal *terminal);
/* Call at safe points around job control; none of these are signal handlers. */
int t4_terminal_suspend(struct t4_terminal *terminal);
int t4_terminal_resume(struct t4_terminal *terminal);
void t4_terminal_close(struct t4_terminal *terminal);

#endif
```

### 附录 11：include/task4/text.h

```c
#ifndef TASK4_TEXT_H
#define TASK4_TEXT_H
#include <stddef.h>

/* 检查一个完整 Unicode 码点；不允许过长编码、代理项或超出 U+10FFFF。 */
static inline unsigned t4_character_width(unsigned char c)
{
    if (c >= 1 && c <= 0x7f) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
static inline int t4_character_valid(const unsigned char *p, size_t n)
{
    if (n == 0 || n != t4_character_width(p[0])) return 0;
    for (size_t i = 1; i < n; ++i) if (p[i] < 0x80 || p[i] > 0xbf) return 0;
    if (n == 3 && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0))) return 0;
    if (n == 4 && ((p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90))) return 0;
    return 1;
}
#endif
```

### 附录 12：include/task4/view.h

```c
#ifndef TASK4_VIEW_H
#define TASK4_VIEW_H

#include "task4/terminal.h"

/* Internal transcript/layout component; terminal.c owns keyboard and termios. */
struct t4_view;
struct t4_screen;
enum t4_view_style { T4_VIEW_TEXT, T4_VIEW_MUTED, T4_VIEW_ACCENT, T4_VIEW_BORDER };
struct t4_view *t4_view_open(void);
void t4_view_close(struct t4_view *view);
void t4_view_resize(struct t4_view *view, unsigned rows, unsigned columns);
void t4_view_dirty(struct t4_view *view);
int t4_view_is_dirty(const struct t4_view *view);
int t4_view_render(struct t4_view *view, struct t4_screen *screen);
void t4_view_reset(struct t4_view *view);
const char *t4_view_style(const struct t4_view *view, enum t4_view_style style);
int t4_view_notice(struct t4_view *view, const void *data, size_t length);
int t4_view_session(struct t4_view *view, const char *session, const char *model);
int t4_view_begin(struct t4_view *view, uint32_t request, const char *prompt, uint32_t offset);
int t4_view_thinking(struct t4_view *view, const void *data, size_t length);
int t4_view_answer(struct t4_view *view, const void *data, size_t length);
int t4_view_status(struct t4_view *view, const struct t4_ui_status *status);
int t4_view_end(struct t4_view *view, const char *state, const char *summary);
void t4_view_theme(struct t4_view *view, int dark);
void t4_view_thinking_visible(struct t4_view *view, int show);
void t4_view_page(struct t4_view *view, int backward);
void t4_view_scroll(struct t4_view *view, int backward, size_t lines);

#endif
```

### 附录 13：src/backend/model.c

```c
#include "task4/backend.h"
#include "task4/files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

/* Freeze options with the turn; editing config affects new turns only. */
static int model_prepare(const char *config, const char *directory)
{
    char path[PATH_MAX];
    if (t4_path(path, directory, "model-config.json") < 0) return -1;
    int input = open(config, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0) return -1;
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int result = -1;
    if (output < 0) goto cleanup;
    char buffer[4096];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto cleanup;
        if (!n) break;
        total += (size_t)n;
        if (total > 65536) { errno = EFBIG; goto cleanup; }
        if (t4_write_all(output, buffer, (size_t)n) < 0) goto cleanup;
    }
    result = fsync(output);
cleanup:
    {
        int saved = errno;
        close(input);
        if (output >= 0) close(output);
        errno = saved;
        return result;
    }
}

static int model_start(const char *adapter, const char *directory,
                       const char *context, const char *runtime_config,
                       uint32_t offset,
                       struct t4_generation *generation)
{
    char config[PATH_MAX], log[PATH_MAX], cursor[11];
    if (t4_path(config, directory, "model-config.json") < 0 ||
        t4_path(log, directory, "generator.log") < 0) return -1;
    snprintf(cursor, sizeof(cursor), "%u", offset);
    char *const args[] = {"python3", (char *)adapter, "--turn", (char *)directory,
        "--context", (char *)context, "--offset", cursor, "--config", config,
        "--runtime-config", (char *)runtime_config, NULL};
    return t4_generation_spawn(args, log, generation);
}

const struct t4_backend t4_model_backend = {
    .name = "llama", .prepare = model_prepare, .start = model_start
};
```

### 附录 14：src/backend/process.c

```c
#define _GNU_SOURCE
#include "task4/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* The adapter has a bounded lifetime and owns only its stdio descriptors. */
int t4_generation_spawn(char *const argv[], const char *log_path,
                        struct t4_generation *generation)
{
    int pair[2], log = -1;
    generation->pid = generation->fd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return -1;
    int flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    log = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 0) goto fail;
    pid_t parent = getpid(), child = fork();
    if (child < 0) goto fail;
    if (!child) {
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigset_t mask;
        sigemptyset(&action.sa_mask);
        const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE};
        for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
            if (sigaction(signals[i], &action, NULL) < 0) _exit(126);
        sigemptyset(&mask);
        if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0 ||
            prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(126);
        close(pair[0]);
        int null = open("/dev/null", O_RDONLY);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 ||
            dup2(pair[1], STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0 ||
            fcntl(STDIN_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDERR_FILENO, F_SETFD, 0) < 0) _exit(126);
        if (null > 2) close(null);
        if (pair[1] > 2) close(pair[1]);
        if (log > 2) close(log);
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "exec generator: %s\n", strerror(errno));
        _exit(127);
    }
    close(pair[1]); close(log);
    generation->pid = child; generation->fd = pair[0];
    return 0;
fail:
    {
        int saved = errno;
        close(pair[0]); close(pair[1]);
        if (log >= 0) close(log);
        errno = saved;
        return -1;
    }
}

void t4_generation_stop(struct t4_generation *generation)
{
    int saved = errno;
    if (!generation) return;
    if (generation->fd >= 0) close(generation->fd);
    generation->fd = -1;
    if (generation->pid > 0) {
        kill(generation->pid, SIGTERM);
        pid_t reaped;
        do { reaped = waitpid(generation->pid, NULL, WNOHANG); } while (reaped < 0 && errno == EINTR);
        if (!reaped) {
            /* Cancellation remains bounded even for a misbehaving adapter. */
            kill(generation->pid, SIGKILL);
            while (waitpid(generation->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    generation->pid = -1;
    errno = saved;
}
```

### 附录 15：src/client/main.c

```c
#define _GNU_SOURCE

#include "task4/checkpoint.h"
#include "task4/client_state.h"
#include "task4/discovery.h"
#include "task4/protocol.h"
#include "task4/status.h"
#include "task4/terminal.h"
#include "task4/text.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static volatile sig_atomic_t interrupt_requested;
static volatile sig_atomic_t resize_requested, suspend_requested;
static int interactive_signals;
static int verbose_status, show_thinking = 1;
static char last_server_error[T4_MAX_FRAME + 1];
static char timeout_detail[256];
static struct t4_terminal *terminal;

static int timeout_failure(const char *operation, int milliseconds, const char *limit)
{
    (void)snprintf(timeout_detail, sizeof(timeout_detail),
                   "Connection timed out while %s (%.1f s %s)",
                   operation, milliseconds / 1000.0, limit);
    errno = ETIMEDOUT;
    return -1;
}
static void stop_signal(int signo)
{
    if (signo == SIGINT && interactive_signals) interrupt_requested = 1;
    else if (signo == SIGWINCH) resize_requested = 1;
    else if (signo == SIGTSTP) suspend_requested = 1;
    else stopped = signo;
}

/* All terminal output uses the same renderer; the editable draft has its own
 * row and is never reconstructed from model text or discarded by an END. */
static void message(const char *format, ...)
{
    char text[8192];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (size < 0) return;
    size_t length = (size_t)size < sizeof(text) ? (size_t)size : sizeof(text) - 1;
    if (terminal) (void)t4_terminal_write(terminal, text, length);
    else { (void)fwrite(text, 1, length, stderr); (void)fflush(stderr); }
}

static int output(const void *data, size_t length, int answer)
{
    if (terminal && (answer ? t4_terminal_answer(terminal, data, length)
                           : t4_terminal_thinking(terminal, data, length)) < 0) return -1;
    if (!terminal || (answer && !isatty(STDOUT_FILENO))) {
        FILE *stream = answer ? stdout : stderr;
        if (fwrite(data, 1, length, stream) != length || fflush(stream) == EOF) return -1;
    }
    return 0;
}

static int terminal_signals(void)
{
    if (suspend_requested) {
        suspend_requested = 0;
        if (terminal && t4_terminal_suspend(terminal) < 0) return -1;
        /* Restore the shell's tty BEFORE stopping. SIGSTOP also works in a
         * session whose process group is orphaned (e.g. a PTY test harness). */
        if (raise(SIGSTOP)) return -1;
        if (terminal && t4_terminal_resume(terminal) < 0) return -1;
    }
    if (resize_requested) {
        resize_requested = 0;
        if (terminal && t4_terminal_resize(terminal) < 0) return -1;
    }
    return stopped ? (errno = EINTR, -1) : 0;
}

struct controls {
    int interactive, quit, cancel_pending;
    uint32_t previous_request, previous_offset;
    const char *session_dir;
};

struct connection_options;
struct conversation {
    const struct connection_options *options;
    char *path;
    struct t4_checkpoint *checkpoint;
    int *fd, *lock;
};

/* 0: not a navigation command; 1: handled in place; 2: changed conversation.
 * A rejected command must leave ask()'s socket and receive state untouched. */
static int navigation(const unsigned char *line, struct controls *u, struct conversation *chat);

static int session_row(const struct t4_client_session *session, void *opaque)
{
    (void)opaque;
    message("%s  [%s]  /switch %s\n", session->id, session->status, session->id);
    return 0;
}

static void sessions(const char *directory)
{
    message("Conversations in %s (current conversation may be marked active):\n", directory);
    if (t4_client_state_scan(directory, session_row, NULL) < 0)
        message("[sessions] %s\n", strerror(errno));
    message("/new | /switch ID | /delete ID (local recycle bin)\n");
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Block handlers while testing their flags, then atomically unblock inside
 * ppoll. Otherwise Ctrl+C between the flag check and poll could strand an
 * idle editor until another key was pressed. */
static int wait_descriptors(struct pollfd *fds, nfds_t count, int milliseconds)
{
    sigset_t mask, old;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTSTP); sigaddset(&mask, SIGWINCH);
    if (sigprocmask(SIG_BLOCK, &mask, &old) < 0) return -1;
    int result;
    if (stopped || interrupt_requested || suspend_requested || resize_requested) {
        errno = EINTR; result = -1;
    } else {
        struct timespec timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
        result = ppoll(fds, count, milliseconds < 0 ? NULL : &timeout, &old);
    }
    int saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    errno = saved;
    return result;
}

static int read_line(FILE *input, unsigned char text[T4_MAX_PROMPT + 1], uint32_t *length);

/* Legacy file/canonical input uses 0 for EOF; the editor uses 0 for an edit
 * without submission. Keep those distinct in the protocol state machine. */
static int control_line(unsigned char *line, uint32_t *length)
{
    if (!terminal) return read_line(stdin, line, length);
    int result = t4_terminal_read(terminal, line, length);
    return result == 0 ? -3 : result == 2 ? 0 : result;
}

static int next_line(FILE *input, unsigned char *line, uint32_t *length)
{
    if (!terminal) return read_line(input, line, length);
    if (t4_terminal_prompt(terminal, "question> ") < 0) return -1;
    for (;;) {
        if (terminal_signals() < 0) return -1;
        if (t4_terminal_tick(terminal) < 0) return -1;
        if (interrupt_requested) {
            interrupt_requested = 0;
            t4_terminal_clear_input(terminal);
            message("[controls] input cleared; no active turn to cancel.\n");
        }
        int result = control_line(line, length);
        if (result != -3) return result;
        struct pollfd tty = {STDIN_FILENO, POLLIN, 0};
        int ready = wait_descriptors(&tty, 1, 33);
        if (ready < 0 && errno != EINTR) return -1;
    }
}

static void help(void)
{
    message("[conversations] /sessions: list | /new: new conversation | /switch ID or FILE\n"
                    "[conversations] /delete [ID]: move local conversation to recycle bin; server copy retained\n"
                    "[recovery] /retry: reconnect and continue the saved turn; errors stay in this screen\n"
                    "[controls] /stop or Ctrl+C: cancel this turn; /steer TEXT: cancel and correct\n"
                    "[controls] /quit: cancel and exit; /help: show controls\n"
                    "[controls] EOF/disconnect keeps the pending checkpoint for recovery.\n"
                    "[controls] While generating, submitting plain text also corrects this turn.\n"
                    "[controls] Arrows/Home/End edit; Up/Down history; pasted newlines become spaces.\n"
                    "[controls] Mouse wheel/PgUp/PgDn scroll chat; Up/Down recall input history.\n"
                    "[controls] /thinking hides/shows reasoning; /theme light|dark.\n"
                    "[controls] --cancel-pending cancels a saved pending turn on reconnect.\n");
}

/* Presentation controls never change a request, generation mode or history. */
static int display_control(const unsigned char *line, uint32_t length)
{
    if (!terminal) return 0;
    if (length == 9 && !memcmp(line, "/thinking", 9)) {
        show_thinking = !show_thinking;
        return t4_terminal_thinking_visible(terminal, show_thinking) < 0 ? -1 : 1;
    }
    if (length == 11 && !memcmp(line, "/theme dark", 11))
        return t4_terminal_theme(terminal, 1) < 0 ? -1 : 1;
    if (length == 12 && !memcmp(line, "/theme light", 12))
        return t4_terminal_theme(terminal, 0) < 0 ? -1 : 1;
    return 0;
}

static int text_valid(const unsigned char *text, uint32_t length)
{
    for (uint32_t at = 0; at < length;) {
        unsigned width = t4_character_width(text[at]);
        if (!width || width > length - at || !t4_character_valid(text + at, width)) return 0;
        at += width;
    }
    return 1;
}

/* A SIGINT must not discard a partially received/sent protocol frame. Defer
 * it only across one bounded frame transfer; SIGTERM still aborts recovery. */
static int frame_signal_mask(sigset_t *old)
{
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGWINCH); sigaddset(&mask, SIGTSTP);
    return sigprocmask(SIG_BLOCK, &mask, old);
}

static int receive_frame(int fd, struct t4_frame *frame, int idle)
{
    sigset_t old;
    if (frame_signal_mask(&old) < 0) return -1;
    int result = t4_recv(fd, frame, idle), saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    if (result < 0 && saved == ETIMEDOUT)
        return timeout_failure("receiving a complete server frame", idle, "frame deadline");
    if (!result) saved = ECONNRESET;
    errno = saved;
    return result;
}

static int send_frame(int fd, uint32_t type, uint32_t request, uint32_t offset,
                      const void *data, uint32_t length, int idle)
{
    struct t4_frame frame = {.type = type, .request_id = request,
                             .offset = offset, .length = length};
    if (length) memcpy(frame.data, data, length);
    sigset_t old;
    if (frame_signal_mask(&old) < 0) return -1;
    int result = t4_send(fd, &frame, idle), saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    if (result < 0 && saved == ETIMEDOUT)
        return timeout_failure("sending a request frame", idle, "frame deadline");
    errno = saved;
    return result;
}

static int steer(const char *path, struct t4_checkpoint *c,
                 const unsigned char *correction, uint32_t length)
{
    static const unsigned char separator[] = "\n\nUser correction: ";
    const uint32_t extra = (uint32_t)sizeof(separator) - 1;
    if (!length || c->request >= 10000 || c->prompt_length > T4_MAX_PROMPT - extra ||
        length > T4_MAX_PROMPT - extra - c->prompt_length) {
        message("[controls] correction rejected: combined prompt must fit %u bytes.\n", T4_MAX_PROMPT);
        return 0;
    }
    if (c->queued_length) {
        message("[controls] correction already queued; waiting for cancellation.\n");
        return 0;
    }
    unsigned char combined[T4_MAX_PROMPT + 1];
    memcpy(combined, c->prompt, c->prompt_length);
    memcpy(combined + c->prompt_length, separator, extra);
    memcpy(combined + c->prompt_length + extra, correction, length);
    uint32_t total = c->prompt_length + extra + length;
    if (t4_checkpoint_queue(path, c, combined, total) < 0) return -1;
    message("[controls] correction queued durably; cancelling turn=%u.\n", c->request);
    return 1;
}

static int ask(int fd, const char *path, struct t4_checkpoint *c, int idle,
               struct controls *u, struct conversation *chat)
{
    last_server_error[0] = timeout_detail[0] = 0;
    if (send_frame(fd, T4_BEGIN, c->request, c->offset, NULL, 0, idle) < 0) return -1;
    for (uint32_t offset = 0; offset < c->prompt_length;) {
        uint32_t n = c->prompt_length - offset;
        if (n > 128) n = 128;
        if (send_frame(fd, T4_INPUT, c->request, offset, c->prompt + offset, n, idle) < 0) return -1;
        offset += n;
    }
    if (send_frame(fd, T4_COMMIT, c->request, c->prompt_length, NULL, 0, idle) < 0) return -1;
    if (terminal) {
        if (t4_terminal_begin_turn(terminal, c->request, (const char *)c->prompt, c->offset) < 0) return -1;
        if (c->offset && t4_terminal_answer(terminal, c->answer, c->offset) < 0) return -1;
    } else message("[client] turn=%u resume=%u prompt=%u bytes\n", c->request, c->offset, c->prompt_length);
    if (terminal && t4_terminal_prompt(terminal, "correct /stop> ") < 0) return -1;
    struct t4_frame frame;
    int cancel = c->queued_length != 0 || u->cancel_pending;
    u->cancel_pending = 0;
    int cancelled_sent = 0, phase = 0, thinking_seen = 0;
    uint32_t thinking_offset = 0;
    struct t4_client_status status;
    t4_status_init(&status);
    int64_t last_network = milliseconds();
    if (last_network < 0) return -1;
    for (;;) {
        if (terminal_signals() < 0) return -1;
        if (terminal && t4_terminal_tick(terminal) < 0) return -1;
        if (interrupt_requested) {
            interrupt_requested = 0; cancel = 1;
            if (terminal) t4_terminal_clear_input(terminal);
        }
        if (cancel && !cancelled_sent) {
            if (send_frame(fd, T4_CANCEL, c->request, c->offset, NULL, 0, idle) < 0) return -1;
            cancelled_sent = 1;
            if (terminal) {
                status.ui.phase = "cancelling";
                if (t4_terminal_status(terminal, &status.ui) < 0) return -1;
            } else message("[controls] cancellation requested for turn=%u; awaiting confirmation.\n", c->request);
        }
        struct pollfd descriptors[2] = {{fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        int buffered = terminal && t4_terminal_pending(terminal);
        int64_t now = milliseconds();
        if (now < 0) return -1;
        int64_t remaining = idle - (now - last_network);
        /* A terminal write or suspended foreground job can delay this loop
         * while valid server frames accumulate in the socket. Inspect its
         * readiness before declaring silence; local rendering time does not
         * prove that the network was idle. Partial frames remain bounded by
         * receive_frame's independent, absolute transfer deadline. */
        int timeout = remaining <= 0 ? 0 : terminal && remaining > 33 ? 33 : (int)remaining;
        int ready = wait_descriptors(descriptors, u->interactive ? 2 : 1, buffered ? 0 : timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (remaining <= 0 && !descriptors[0].revents)
            return timeout_failure("waiting for server data", idle, "network idle limit");
        if (!ready && !buffered) continue; /* Render clock never refreshes the network deadline. */
        if (buffered) descriptors[1].revents |= POLLIN;
        if (u->interactive && descriptors[1].revents) {
            unsigned char line[T4_MAX_PROMPT + 1];
            uint32_t length;
            int received = control_line(line, &length);
            if (received == -1 && errno == EINTR) { clearerr(stdin); continue; }
            if (received != -3 && received <= 0) {
                if (received == -2) {
                    message("[controls] invalid control line skipped.\n");
                    continue;
                }
                errno = received ? errno : ECONNRESET;
                return -1; /* EOF is a resumable disconnect, not an explicit CANCEL. */
            }
            int display = received == 1 ? display_control(line, length) : 0;
            if (display < 0) return -1;
            int navigated = received == 1 && !display ? navigation(line, u, chat) : 0;
            if (navigated < 0 || navigated == 2) return navigated;
            if (received == -3 || display || navigated) { /* UI-only edits never submit a request. */ }
            else if (length == 5 && !memcmp(line, "/help", 5)) help();
            else if (length == 9 && !memcmp(line, "/sessions", 9)) sessions(u->session_dir);
            else if (length == 6 && !memcmp(line, "/retry", 6)) message("[recovery] this turn is already running.\n");
            else if (cancelled_sent) message("[controls] waiting for cancellation confirmation.\n");
            else if (length == 5 && !memcmp(line, "/stop", 5)) cancel = 1;
            else if (length == 5 && !memcmp(line, "/quit", 5)) { cancel = 1; u->quit = 1; }
            else if (length > 7 && !memcmp(line, "/steer ", 7)) {
                int queued = steer(path, c, line + 7, length - 7);
                if (queued < 0) return -1;
                if (queued) cancel = 1;
            } else if (length && line[0] != '/') {
                int queued = steer(path, c, line, length);
                if (queued < 0) return -1;
                if (queued) cancel = 1;
            } else message("[controls] use /steer TEXT to correct an active turn, or /stop.\n");
            if (cancel && !cancelled_sent) continue;
        }
        if (!(descriptors[0].revents)) continue;
        if (receive_frame(fd, &frame, idle) != 1) return -1;
        last_network = milliseconds();
        if (last_network < 0) return -1;
        if (frame.type == T4_ERROR) {
            memcpy(last_server_error, frame.data, frame.length);
            last_server_error[frame.length] = 0;
            if (terminal) (void)t4_terminal_end_turn(terminal, "error", (const char *)frame.data);
            message("[server error] %.*s\n", (int)frame.length, frame.data);
            errno = EPROTO; return -1;
        }
        /* END may win a race with CANCEL. The READY server can then acknowledge
         * the already terminal old turn while we have started the next one. */
        if (u->previous_request && (frame.type == T4_END || frame.type == T4_CANCELLED) && !frame.length &&
            frame.request_id == u->previous_request && frame.offset == u->previous_offset) continue;
        if (frame.request_id != c->request) {
            errno = EPROTO; return -1;
        }
        if (frame.type == T4_THINK) {
            if (!frame.length || !text_valid(frame.data, frame.length) ||
                (thinking_seen && frame.offset != thinking_offset) ||
                frame.offset > T4_MAX_RESPONSE ||
                frame.length > T4_MAX_RESPONSE - frame.offset) { errno = EPROTO; return -1; }
            thinking_seen = 1; thinking_offset = frame.offset + frame.length;
            if (phase != 1) {
                if (!terminal) message("\n[thinking] ");
                phase = 1;
            }
            if (output(frame.data, frame.length, 0) < 0) return -1;
            continue;
        }
        if (frame.offset != c->offset) { errno = EPROTO; return -1; }
        if (frame.type == T4_STATUS && text_valid(frame.data, frame.length)) {
            if (t4_status_update(&status, (const char *)frame.data, frame.length) == 0) {
                /* Generator metrics precede worker exit and durable END. */
                if (!strcmp(status.phase, "completed")) strcpy(status.phase, "finishing");
                if (!strcmp(status.phase, "generating"))
                    status.ui.phase = phase == 2 ? "answering" : phase == 1 ? "thinking" : "generating";
                if (cancelled_sent) status.ui.phase = "cancelling";
                if (terminal && t4_terminal_status(terminal, &status.ui) < 0) return -1;
                if (!terminal && verbose_status) message("\n[status] %.*s\n", (int)frame.length, frame.data);
            } else if (!terminal && verbose_status) {
                /* Retain support for older peers' one-line textual STATUS. */
                message("\n[status] %.*s\n", (int)frame.length, frame.data);
            }
            continue;
        }
        if (frame.type == T4_HEARTBEAT && !frame.length) continue;
        if ((frame.type == T4_END || frame.type == T4_CANCELLED) && !frame.length) {
            c->done = frame.type == T4_END ? 1U : 2U;
            if (t4_checkpoint_save(path, c) < 0) return -1;
            if (!terminal) { if (output("\n", 1, 1) < 0) return -1; }
            else if (!isatty(STDOUT_FILENO) && (fputc('\n', stdout) == EOF || fflush(stdout) == EOF)) return -1;
            if (terminal) {
                if (t4_terminal_end_turn(terminal, c->done == 2 ? "cancelled" : "completed", NULL) < 0) return -1;
            } else {
                message("\n[client] %s turn=%u bytes=%u\n",
                        c->done == 2 ? "cancelled" : "completed", c->request, c->offset);
                if (status.ui.context_tokens)
                    message("[stats] input=%u context=%u generated=%u elapsed=%.1fs rate=%.1f tok/s\n",
                            status.ui.prompt_tokens, status.ui.context_tokens, status.ui.generated_tokens,
                            status.ui.elapsed_seconds, status.ui.tokens_per_second);
            }
            u->previous_request = c->request; u->previous_offset = c->offset;
            return 0;
        }
        if (frame.type != T4_DELTA || !t4_character_valid(frame.data, frame.length) ||
            frame.length > T4_MAX_RESPONSE - c->offset) { errno = EPROTO; return -1; }
        memcpy(c->answer + c->offset, frame.data, frame.length);
        c->offset += frame.length;
        /* 游标与完整正文先落盘；终端不是持久介质，恢复只打印尚未保存的后缀。 */
        if (t4_checkpoint_save(path, c) < 0) return -1;
        if (phase != 2) { if (!terminal) message("\n[answer]\n"); phase = 2; }
        if (output(frame.data, frame.length, 1) < 0) return -1;
        if (stopped) return -1;
    }
}

/* 消费超长/NUL整行，尾部不会被错误地变成下一条问题。 */
static int read_line(FILE *input, unsigned char text[T4_MAX_PROMPT + 1], uint32_t *length)
{
    int byte, invalid = 0, any = 0;
    *length = 0;
    while ((byte = fgetc(input)) != EOF && byte != '\n') {
        any = 1;
        if (!byte || *length == T4_MAX_PROMPT) invalid = 1;
        else text[(*length)++] = (unsigned char)byte;
    }
    text[*length] = 0;
    if (ferror(input)) return -1;
    if (invalid) return -2;
    return byte == EOF && !any ? 0 : 1;
}

static void usage(const char *program)
{
    message("Usage: %s [--host HOST --port 1..65535 | --server-state DIR]\n"
            "  [--discovery-dir DIR]\n"
            "  [--session-dir DIR | --resume FILE | --checkpoint FILE]\n"
            "  [--input FILE] [--resume-only | --cancel-pending] [--idle-ms 1500..3600000]\n"
            "  [--theme light|dark] [--plain] [--verbose-status]\n"
            "  %s --list-sessions [--session-dir DIR]\n"
            "Default: discover one active local server via state/services, then create an independent session.\n"
            "--resume requires an existing checkpoint; --checkpoint creates or resumes.\n", program, program);
}

static int discover(const char *registry, const char *state, const char *token,
                    struct t4_endpoint *endpoint)
{
    if (t4_service_discover(registry, state, token, endpoint) == 0) return 0;
    int saved = errno;
    if (saved == EEXIST)
        message("[client] multiple matching servers; select --server-state DIR or --host HOST --port PORT.\n");
    else if (saved == ENOENT)
        message("[client] no active matching server in %s; start the server first%s.\n",
                state ? state : registry,
                token && *token ? " using the original --state directory for this checkpoint" : "");
    else message("[client] cannot discover server: %s; check --server-state or --discovery-dir.\n", strerror(saved));
    errno = saved;
    return -1;
}

struct connection_options {
    const char *host, *server_state, *discovery_dir;
    uint32_t port, idle;
    int explicit_port;
};

/* Every reconnect resolves the current endpoint; inference and checkpoint
 * identity remain owned by the existing session, never by a cached port. */
static int connect_session(const struct connection_options *options, const char *path,
                           struct t4_checkpoint *c)
{
    last_server_error[0] = timeout_detail[0] = 0;
    struct t4_endpoint endpoint;
    const char *host = options->host;
    uint16_t port = (uint16_t)options->port;
    int idle = (int)options->idle;
    if (!options->explicit_port) {
        if (discover(options->discovery_dir, options->server_state, c->session, &endpoint) < 0) return -1;
        host = endpoint.host; port = endpoint.port;
    }
    int fd = t4_connect(host, port, idle);
    if (fd < 0) {
        if (errno == ETIMEDOUT) return timeout_failure("connecting to the server", idle, "connection deadline");
        return -1;
    }
    struct t4_frame frame;
    if (!options->explicit_port) {
        if (t4_service_validate(&endpoint) < 0 || send_frame(fd, T4_PROBE, 0, 0, NULL, 0, idle) < 0 ||
            receive_frame(fd, &frame, idle) != 1) goto failed;
        if (frame.type == T4_ERROR) goto server_error;
        if (frame.type != T4_SERVICE || frame.request_id || frame.offset || frame.length != 32 ||
            memcmp(frame.data, endpoint.instance, 32) || t4_service_validate(&endpoint) < 0) {
            message("[client] discovered server changed; no HELLO was sent. Retry after startup completes.\n");
            errno = ESTALE; goto failed;
        }
        message("[client] discovered %s:%u state=%s\n", host, port, endpoint.state);
    }
    if (send_frame(fd, T4_HELLO, 0, 0, c->session, (uint32_t)strlen(c->session), idle) < 0 ||
        receive_frame(fd, &frame, idle) != 1) goto failed;
    if (frame.type == T4_ERROR) goto server_error;
    if (frame.type != T4_WELCOME || frame.request_id || frame.offset || frame.length != 32) {
        errno = EPROTO; goto failed;
    }
    for (uint32_t i = 0; i < 32; ++i)
        if (!((frame.data[i] >= '0' && frame.data[i] <= '9') ||
              (frame.data[i] >= 'a' && frame.data[i] <= 'f'))) { errno = EPROTO; goto failed; }
    if (c->session[0] && memcmp(c->session, frame.data, 32)) { errno = EPROTO; goto failed; }
    memcpy(c->session, frame.data, 32); c->session[32] = 0;
    if (t4_checkpoint_save(path, c) < 0) goto failed;
    return fd;
server_error:
    memcpy(last_server_error, frame.data, frame.length); last_server_error[frame.length] = 0;
    errno = EPROTO;
failed:
    {
        int saved = errno ? errno : ECONNRESET;
        close(fd); errno = saved; return -1;
    }
}

static int display_saved(const struct t4_checkpoint *saved, void *opaque)
{
    uint32_t skip = *(const uint32_t *)opaque;
    if (!saved->request || saved->request == skip) return 0;
    return t4_terminal_history(terminal, (const char *)saved->prompt) < 0 ||
           t4_terminal_begin_turn(terminal, saved->request, (const char *)saved->prompt, 0) < 0 ||
           t4_terminal_answer(terminal, saved->answer, saved->offset) < 0 ||
           t4_terminal_end_turn(terminal, saved->done == 2 ? "cancelled" : "completed", "saved conversation") < 0 ? -1 : 0;
}

static int restore_display(const char *path, const struct t4_checkpoint *c, int show_pending)
{
    if (t4_terminal_reset(terminal) < 0 || t4_terminal_session(terminal, c->session, "local") < 0) return -1;
    if (c->session[0] && t4_client_history_scan(path, c->session, display_saved, (void *)&c->request) < 0)
        message("[history] saved history unavailable: %s; current checkpoint retained.\n", strerror(errno));
    if (c->request && t4_terminal_history(terminal, (const char *)c->prompt) < 0) return -1;
    if (c->request && (c->done || show_pending)) {
        if (t4_terminal_begin_turn(terminal, c->request, (const char *)c->prompt, c->offset) < 0 ||
            t4_terminal_answer(terminal, c->answer, c->offset) < 0 ||
            t4_terminal_end_turn(terminal, c->done == 2 ? "cancelled" : c->done ? "completed" : "paused",
                                 c->done ? "restored from checkpoint" : "/retry to continue") < 0) return -1;
    }
    message("Checkpoint: %s\n/help  /sessions  /new\n", path);
    return 0;
}

static int archive_turn(const char *path, const struct t4_checkpoint *c)
{
    if (c->request && c->done && t4_client_history_archive(path, c) < 0) {
        message("[history] could not archive this turn: %s; latest checkpoint remains saved.\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void disconnect_session(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void conversation_error(int *fd)
{
    int saved = errno ? errno : EPROTO;
    const char *detail = last_server_error[0] ? last_server_error :
        saved == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(saved);
    disconnect_session(fd);
    (void)t4_terminal_end_turn(terminal, "error", detail);
    message("[conversation error] %s\nSaved checkpoint retained. /retry to reconnect; /stop to cancel; /new or /switch to change conversation.\n", detail);
    last_server_error[0] = timeout_detail[0] = 0;
    errno = saved;
}

static int same_file(const char *a, const char *b)
{
    struct stat left, right;
    return stat(a, &left) == 0 && stat(b, &right) == 0 &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

/* Stage and lock the destination before releasing the current checkpoint.
 * Return navigation's handled/changed result; -1 is a terminal failure after
 * committing the switch. Rejected targets preserve a live generation too. */
static int switch_session(const char *target, const struct connection_options *options,
                          struct controls *u, char path[PATH_MAX], struct t4_checkpoint *c,
                          int *fd, int *lock)
{
    char next_path[PATH_MAX];
    if (target) {
        if (t4_client_state_resolve(u->session_dir, target, next_path, sizeof(next_path)) < 0) goto rejected;
        if (same_file(path, next_path)) { message("[sessions] already in this conversation.\n"); return 1; }
    } else if (t4_client_state_new(u->session_dir, next_path, sizeof(next_path)) < 0) goto rejected;
    struct t4_checkpoint next = {0};
    next.answer = malloc(T4_MAX_RESPONSE);
    if (!next.answer) goto rejected;
    int next_lock = t4_checkpoint_lock(next_path);
    if (next_lock < 0 || t4_checkpoint_load(next_path, &next) < 0) {
        int saved = errno;
        if (next_lock >= 0) close(next_lock);
        free(next.answer); errno = saved; goto rejected;
    }
    if (archive_turn(path, c) < 0) {
        int saved = errno; close(next_lock); free(next.answer); errno = saved; goto rejected;
    }
    disconnect_session(fd); close(*lock);
    free(c->answer); *c = next; *lock = next_lock;
    strcpy(path, next_path);
    u->previous_request = u->previous_offset = 0; u->cancel_pending = 0;
    if (restore_display(path, c, 1) < 0) return -1;
    *fd = connect_session(options, path, c);
    if (*fd < 0) { conversation_error(fd); return 2; }
    if (t4_terminal_session(terminal, c->session, "local") < 0) return -1;
    if (c->request && !c->done) message("[sessions] pending turn paused; /retry continues its saved prefix.\n");
    return 2;
rejected:
    message("[sessions] switch failed: %s; use /sessions to select an available conversation%s.\n",
            strerror(errno), *fd < 0 && c->request && !c->done ? "; current turn is paused, /retry to continue" : "");
    return 1;
}

static int navigation(const unsigned char *line, struct controls *u, struct conversation *chat)
{
    const char *command = (const char *)line;
    if (strcmp(command, "/new") && strcmp(command, "/switch") && strncmp(command, "/switch ", 8)
        && strcmp(command, "/delete") && strncmp(command, "/delete ", 8)) return 0;
    if (!chat) { message("[sessions] conversation switching requires the interactive TUI.\n"); return 1; }
    if (!strcmp(command, "/switch")) {
        message("[sessions] /switch requires an ID or checkpoint path from /sessions.\n"); return 1;
    }
    if (!strcmp(command, "/new") || !strncmp(command, "/switch ", 8))
        return switch_session(command[1] == 'n' ? NULL : command + 8, chat->options, u,
                              chat->path, chat->checkpoint, chat->fd, chat->lock);

    char id[15], selected[PATH_MAX], trash[PATH_MAX];
    int implicit = !strcmp(command, "/delete");
    const char *target = command + 7;
    if (implicit) {
        char parent[PATH_MAX]; strcpy(parent, chat->path);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        const char *base = strrchr(parent, '/'); base = base ? base + 1 : parent;
        if (strlen(base) != 14) { message("[sessions] /delete requires a managed session ID from /sessions.\n"); return 1; }
        strcpy(id, base); target = id;
    } else ++target;
    /* Deletion accepts a managed ID, never a checkpoint path. Validate this
     * before a path alias could cause us to leave the current conversation. */
    if (strlen(target) != 14 || strncmp(target, "session-", 8) ||
        strspn(target + 8, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != 6) {
        message("[sessions] /delete requires a managed session ID from /sessions.\n"); return 1;
    }
    if (t4_client_state_resolve(u->session_dir, target, selected, sizeof(selected)) < 0) {
        message("[sessions] delete failed: %s\n", strerror(errno)); return 1;
    }
    int current = same_file(chat->path, selected);
    if (implicit && !current) {
        message("[sessions] current checkpoint is outside this managed list; select an explicit ID with /sessions.\n");
        return 1;
    }
    if (current) {
        int changed = switch_session(NULL, chat->options, u, chat->path, chat->checkpoint, chat->fd, chat->lock);
        if (changed != 2) return changed;
    }
    if (t4_client_state_delete(u->session_dir, target, trash, sizeof(trash)) < 0)
        message("[sessions] delete failed: %s (active conversations cannot be deleted).\n", strerror(errno));
    else message("[sessions] deleted locally; recoverable at %s; server copy retained.\n", trash);
    return current ? 2 : 1;
}

/* Persistent TUI shell. Network failures end an attempt, never this UI. Batch
 * and --resume-only callers retain their exit-status contract in main(). */
static int interactive_chat(const struct connection_options *options, struct controls *u,
                            char path[PATH_MAX], struct t4_checkpoint *c, int *fd, int *lock)
{
    struct conversation chat = {options, path, c, fd, lock};
    archive_turn(path, c);
    if (restore_display(path, c, 0) < 0) return -1;
    int run = c->request && (!c->done || c->queued_length);
    while (!stopped && !u->quit) {
        if (run) {
            run = 0;
            /* CANCELLED may have committed just before a crash, with the
             * durable correction still queued. Finish that transaction once
             * before accepting any unrelated new question. */
            if (c->done && c->queued_length) {
                if (c->request >= 10000) { errno = EOVERFLOW; conversation_error(fd); continue; }
                if (archive_turn(path, c) < 0) { conversation_error(fd); continue; }
                c->prompt_length = c->queued_length;
                memcpy(c->prompt, c->queued_prompt, c->prompt_length + 1);
                ++c->request; c->offset = 0; c->done = 0;
                if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
            }
            if (*fd < 0) {
                *fd = connect_session(options, path, c);
                if (*fd < 0) { conversation_error(fd); continue; }
            }
            int result = ask(*fd, path, c, (int)options->idle, u, &chat);
            if (stopped) break;
            if (result < 0) { conversation_error(fd); continue; }
            if (result == 2) {
                run = c->done && c->queued_length;
                continue; /* Navigation already committed the new conversation. */
            }
            else {
                int archived = archive_turn(path, c);
                if (u->quit) break;
                if (c->queued_length) {
                    if (archived < 0) { conversation_error(fd); continue; }
                    if (c->request >= 10000) { errno = EOVERFLOW; conversation_error(fd); continue; }
                    c->prompt_length = c->queued_length;
                    memcpy(c->prompt, c->queued_prompt, c->prompt_length + 1);
                    ++c->request; c->offset = 0; c->done = 0;
                    if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
                    run = 1; continue;
                }
            }
        }
        unsigned char line[T4_MAX_PROMPT + 1];
        uint32_t length;
        int received = next_line(stdin, line, &length);
        if (received == -1 && errno == EINTR && !stopped) continue;
        if (!received) return 0;
        if (received == -2) { message("[input] question is too long or invalid; edit and retry.\n"); continue; }
        if (received < 0) return -1;
        if (stopped) break;
        if (display_control(line, length)) continue;
        const char *command = (const char *)line;
        if (!strcmp(command, "/quit")) return 0;
        if (!strcmp(command, "/help")) { help(); continue; }
        if (!strcmp(command, "/sessions")) { sessions(u->session_dir); continue; }
        int navigated = navigation(line, u, &chat);
        if (navigated < 0) return -1;
        if (navigated) {
            if (navigated == 2 && c->done && c->queued_length) run = 1;
            continue;
        }
        if (!strcmp(command, "/retry") || !strcmp(command, "/stop")) {
            if (c->request && (!c->done || c->queued_length)) {
                u->cancel_pending = !strcmp(command, "/stop");
                if (restore_display(path, c, 0) < 0) return -1;
                run = 1;
            } else if (!strcmp(command, "/retry")) {
                disconnect_session(fd); *fd = connect_session(options, path, c);
                if (*fd < 0) conversation_error(fd); else message("[recovery] connected; no pending turn.\n");
            } else message("[controls] no active turn to cancel.\n");
            continue;
        }
        if (length && line[0] == '/' && strncmp(command, "/steer ", 7)) {
            message("[controls] unknown command; /help lists available commands.\n"); continue;
        }
        if (c->done && c->queued_length) {
            message("[recovery] a saved correction is waiting; /retry submits it before another question.\n");
            continue;
        }
        if (c->request && !c->done) {
            const unsigned char *correction = line;
            uint32_t size = length;
            if (!strncmp(command, "/steer ", 7)) { correction += 7; size -= 7; }
            int queued = steer(path, c, correction, size);
            if (queued < 0) { conversation_error(fd); continue; }
            if (queued) { if (restore_display(path, c, 0) < 0) return -1; run = 1; }
            continue;
        }
        if (!strncmp(command, "/steer ", 7)) { message("[controls] no pending turn; enter a new question.\n"); continue; }
        if (c->request >= 10000) { message("[conversation] turn limit reached; /new starts another conversation.\n"); continue; }
        if (*fd < 0) {
            *fd = connect_session(options, path, c);
            if (*fd < 0) { conversation_error(fd); continue; }
        }
        if (archive_turn(path, c) < 0) continue;
        memcpy(c->prompt, line, length + 1); c->prompt_length = length;
        ++c->request; c->offset = 0; c->done = 0;
        if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
        run = 1;
    }
    return stopped ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *path = NULL, *input_path = NULL;
    const char *session_dir = "state/clients";
    const char *server_state = NULL, *discovery_dir = "state/services";
    struct t4_endpoint endpoint;
    char automatic_path[PATH_MAX];
    uint32_t port = 0, idle = 10000;
    int explicit_port = 0, explicit_host = 0;
    int resume_only = 0, cancel_pending = 0, require_existing = 0, list = 0, plain = 0, dark = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") && argc == 2) { usage(argv[0]); help(); return 0; }
        if (!strcmp(argv[i], "--list-sessions")) { list = 1; continue; }
        if (!strcmp(argv[i], "--plain")) { plain = 1; continue; }
        if (!strcmp(argv[i], "--verbose-status")) { verbose_status = 1; continue; }
        if (!strcmp(argv[i], "--resume-only")) { resume_only = 1; continue; }
        if (!strcmp(argv[i], "--cancel-pending")) { cancel_pending = 1; resume_only = 1; continue; }
        if (i + 1 == argc) goto usage;
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) { host = value; explicit_host = 1; }
        else if (!strcmp(key, "--checkpoint") || !strcmp(key, "--resume")) {
            if (path) goto usage;
            path = value; require_existing = !strcmp(key, "--resume");
        }
        else if (!strcmp(key, "--session-dir")) session_dir = value;
        else if (!strcmp(key, "--server-state")) server_state = value;
        else if (!strcmp(key, "--discovery-dir")) discovery_dir = value;
        else if (!strcmp(key, "--theme")) {
            if (strcmp(value, "light") && strcmp(value, "dark")) goto usage;
            dark = !strcmp(value, "dark");
        }
        else if (!strcmp(key, "--input")) input_path = value;
        else if (!strcmp(key, "--port")) {
            if (t4_uint(value, 1, 65535, &port) < 0) goto usage;
            explicit_port = 1;
        }
        else if (!strcmp(key, "--idle-ms")) { if (t4_uint(value, 1500, 3600000, &idle) < 0) goto usage; }
        else goto usage;
    }
    if (list) {
        if (path || input_path || resume_only) goto usage;
        if (t4_client_state_list(session_dir) == 0) return 0;
        perror("list sessions"); return 1;
    }
    if ((explicit_host && !explicit_port) || (server_state && (explicit_host || explicit_port))) {
        message("[client] --host requires --port; --server-state selects a discovered local service.\n");
        goto usage;
    }
    if (resume_only && !path) {
        message("[client] recovery requires --resume FILE or --checkpoint FILE.\n");
        goto usage;
    }
    if (require_existing && access(path, F_OK) < 0) {
        message("[client] cannot resume %s: %s; select an existing checkpoint.\n", path, strerror(errno));
        return 1;
    }
    if (!path) {
        /* Do not create an empty client directory when no service is available. */
        if (!explicit_port) {
            if (discover(discovery_dir, server_state, NULL, &endpoint) < 0) return 1;
        }
        if (t4_client_state_new(session_dir, automatic_path, sizeof(automatic_path)) < 0) {
            perror("create independent session"); return 1;
        }
        path = automatic_path;
    }
    (void)setlocale(LC_CTYPE, "");
    struct controls u = {.interactive = !input_path && isatty(STDIN_FILENO), .session_dir = session_dir};
    interactive_signals = u.interactive;
    if (u.interactive) {
        /* Poll and stdio must observe the same tty bytes without read-ahead. */
        if (setvbuf(stdin, NULL, _IONBF, 0) != 0) { message("stdin buffering failed.\n"); return 1; }
    }
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0 || sigaction(SIGWINCH, &action, NULL) < 0 ||
        (u.interactive && sigaction(SIGTSTP, &action, NULL) < 0)) {
        perror("sigaction"); return 1;
    }
    struct t4_checkpoint c = {0};
    c.answer = malloc(T4_MAX_RESPONSE);
    if (!c.answer) { perror("malloc"); return 1; }
    int lock = -1, fd = -1, result = 1, failure_errno = 0, used_tui = 0;
    FILE *input = stdin;
    lock = t4_checkpoint_lock(path);
    if (lock < 0 || t4_checkpoint_load(path, &c) < 0) {
        t4_client_state_lock_error(path, errno); goto cleanup;
    }
    u.cancel_pending = cancel_pending && !c.done && c.request != 0;
    if (input_path && !(input = fopen(input_path, "r"))) { perror("input"); goto cleanup; }
    if (input_path && !resume_only) {
        /* 批量输入恢复沿用原文件，跳过已持久提交的整行，不重复提问。 */
        for (uint32_t line = 0; line < c.file_lines; ++line) {
            int byte, any = 0;
            while ((byte = fgetc(input)) != EOF && byte != '\n') any = 1;
            if (ferror(input) || (byte == EOF && !any)) { errno = EINVAL; goto failure; }
        }
    }
    const struct connection_options options = {host, server_state, discovery_dir, port, idle, explicit_port};
    fd = connect_session(&options, path, &c);
    if (fd < 0) goto failure;
    if (u.interactive && isatty(STDERR_FILENO) && !plain) {
        terminal = t4_terminal_open();
        if (!terminal) goto failure;
        used_tui = 1;
        if (t4_terminal_theme(terminal, dark) < 0 || t4_terminal_session(terminal, c.session, "local") < 0)
            goto failure;
    }
    if (terminal) {
        message("Checkpoint: %s\n/help for keys; /thinking toggles reasoning visibility.\n", path);
    } else {
        message("[client] session=%s checkpoint=%s\n", c.session, path);
        message("[client] reconnect with --resume %s (same server state; auto-discovery follows port changes).\n", path);
        if (u.interactive) help();
    }
    if (terminal && !resume_only && !input_path) {
        if (path != automatic_path) {
            if (strlen(path) >= sizeof(automatic_path)) { errno = ENAMETOOLONG; goto failure; }
            strcpy(automatic_path, path); path = automatic_path;
        }
        result = interactive_chat(&options, &u, automatic_path, &c, &fd, &lock);
        if (result < 0) goto failure;
        goto cleanup;
    }
    if (terminal && c.done && c.request) {
        /* A local checkpoint contains the latest saved turn. Restore its visible
         * answer for orientation; older server history remains model-owned. */
        if (t4_terminal_begin_turn(terminal, c.request, (const char *)c.prompt, c.offset) < 0 ||
            t4_terminal_answer(terminal, c.answer, c.offset) < 0 ||
            t4_terminal_end_turn(terminal, c.done == 2 ? "cancelled" : "completed", "restored from checkpoint") < 0)
            goto failure;
    }
    if (!c.done) {
        message("[client] resuming pending answer; durable prefix=%u bytes\n", c.offset);
        if (ask(fd, path, &c, (int)idle, &u, NULL) < 0) goto failure;
    }
    while (!stopped) {
        if (u.quit) { result = 0; break; }
        if (c.queued_length) {
            if (c.request >= 10000) { errno = EOVERFLOW; goto failure; }
            c.prompt_length = c.queued_length;
            memcpy(c.prompt, c.queued_prompt, c.prompt_length + 1);
            ++c.request; c.offset = 0; c.done = 0;
            /* Save new request first; checkpoint_save then removes the sidecar.
             * A crash in between is recognized by matching request + prompt. */
            if (t4_checkpoint_save(path, &c) < 0 || ask(fd, path, &c, (int)idle, &u, NULL) < 0)
                goto failure;
            continue;
        }
        if (resume_only) { result = 0; break; }
        if (terminal_signals() < 0) goto failure;
        if (u.interactive && !terminal) {
            message("question (/help for controls)> "); fflush(stderr);
        }
        int received = next_line(input, c.prompt, &c.prompt_length);
        if (received == -1 && u.interactive && errno == EINTR) {
            if (stopped) goto failure;
            clearerr(input); interrupt_requested = 0;
            if (terminal_signals() < 0) goto failure;
            continue;
        }
        if (!received) { result = 0; break; }
        if (input_path) ++c.file_lines;
        if (received == -1) goto failure;
        if (received == -2) {
            message("[client] invalid line skipped (max %u bytes, no NUL).\n", T4_MAX_PROMPT);
            continue;
        }
        int display = display_control(c.prompt, c.prompt_length);
        if (display < 0) goto failure;
        if (display) continue;
        if (c.prompt_length == 5 && !memcmp(c.prompt, "/quit", 5)) { result = 0; break; }
        if (u.interactive && c.prompt_length == 5 && !memcmp(c.prompt, "/help", 5)) { help(); continue; }
        if (u.interactive && ((c.prompt_length == 5 && !memcmp(c.prompt, "/stop", 5)) ||
            (c.prompt_length >= 6 && !memcmp(c.prompt, "/steer", 6)))) {
            message("[controls] no active turn; enter the corrected question.\n");
            continue;
        }
        if (c.request >= 10000) { errno = EOVERFLOW; goto failure; }
        ++c.request;
        c.offset = 0; c.done = 0;
        if (t4_checkpoint_save(path, &c) < 0 || ask(fd, path, &c, (int)idle, &u, NULL) < 0) goto failure;
    }
    goto cleanup;
failure:
    if (!errno) errno = EPROTO;
    failure_errno = errno;
    const char *detail = last_server_error[0] ? last_server_error :
        failure_errno == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(failure_errno);
    if (terminal) (void)t4_terminal_end_turn(terminal, "error", detail);
    else message("[client] stream/checkpoint: %s; reconnect with --resume %s\n", detail, path);
cleanup:
    if (terminal) { t4_terminal_close(terminal); terminal = NULL; }
    if (used_tui) {
        if (last_server_error[0]) message("[server error] %s\n", last_server_error);
        if (failure_errno) message("[client] %s; saved session: --resume %s\n",
                failure_errno == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(failure_errno), path);
        else message("[client] saved session: --resume %s\n", path);
    }
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    if (input && input != stdin && fclose(input) == EOF) result = 1;
    free(c.answer);
    return stopped ? 128 + stopped : result;
usage:
    usage(argv[0]);
    return 2;
}
```

### 附录 16：src/client/screen.c

```c
#define _POSIX_C_SOURCE 200809L

#include "task4/screen.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_LIMIT (8U * 1024U * 1024U)

struct buffer { char *data; size_t length, capacity; };
struct t4_screen {
    int fd, invalid;
    unsigned rows, columns, row, cursor_row, cursor_column, old_row, old_column;
    struct buffer *current, *previous, pending;
    size_t sent;
};

static int append(struct buffer *buffer, const void *data, size_t length)
{
    if (length > OUTPUT_LIMIT - buffer->length) { errno = EOVERFLOW; return -1; }
    size_t needed = buffer->length + length + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < needed) capacity *= 2;
        void *copy = realloc(buffer->data, capacity);
        if (!copy) return -1;
        buffer->data = copy; buffer->capacity = capacity;
    }
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length; buffer->data[buffer->length] = 0;
    return 0;
}

static int string(struct buffer *buffer, const char *text)
{
    return append(buffer, text, strlen(text));
}

struct t4_screen *t4_screen_open(void)
{
    /* Opening this proc fd creates an independent tty open-file description:
     * O_NONBLOCK must not change stdin/stdout or the parent shell's flags. */
    int fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct t4_screen *screen = calloc(1, sizeof(*screen));
    if (!screen) { close(fd); return NULL; }
    screen->fd = fd; screen->invalid = 1;
    return screen;
}

static void free_rows(struct t4_screen *screen)
{
    for (unsigned at = 0; at < screen->rows; ++at) {
        free(screen->current[at].data); free(screen->previous[at].data);
    }
    free(screen->current); free(screen->previous);
}

int t4_screen_resize(struct t4_screen *screen, unsigned rows, unsigned columns)
{
    if (rows == screen->rows && columns == screen->columns) return 0;
    struct buffer *current = calloc(rows, sizeof(*current));
    struct buffer *previous = calloc(rows, sizeof(*previous));
    if (!current || !previous) { free(current); free(previous); return -1; }
    free_rows(screen); screen->current = current; screen->previous = previous;
    screen->rows = rows; screen->columns = columns; screen->invalid = 1;
    return 0;
}

void t4_screen_invalidate(struct t4_screen *screen) { screen->invalid = 1; }

int t4_screen_row(struct t4_screen *screen, unsigned row, const char *style)
{
    if (!row || row > screen->rows) { errno = EINVAL; return -1; }
    screen->row = row - 1;
    screen->current[screen->row].length = 0;
    return string(&screen->current[screen->row], style);
}

int t4_screen_append(struct t4_screen *screen, const void *data, size_t length)
{
    return append(&screen->current[screen->row], data, length);
}

void t4_screen_cursor(struct t4_screen *screen, unsigned row, unsigned column)
{
    screen->cursor_row = row; screen->cursor_column = column;
}

int t4_screen_busy(const struct t4_screen *screen) { return screen->sent < screen->pending.length; }

int t4_screen_drain(struct t4_screen *screen)
{
    /* A bounded number of writes keeps even a continuously writable terminal
     * from monopolizing the TCP event loop. EAGAIN is normal backpressure. */
    for (unsigned count = 0; count < 4 && t4_screen_busy(screen); ++count) {
        ssize_t written = write(screen->fd, screen->pending.data + screen->sent,
                                screen->pending.length - screen->sent);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (written <= 0) { if (!written) errno = EIO; return -1; }
        screen->sent += (size_t)written;
    }
    if (!t4_screen_busy(screen)) screen->sent = screen->pending.length = 0;
    return 0;
}

int t4_screen_control(struct t4_screen *screen, const char *text)
{
    if (string(&screen->pending, text) < 0) return -1;
    return t4_screen_drain(screen);
}

int t4_screen_submit(struct t4_screen *screen)
{
    if (t4_screen_busy(screen)) { errno = EBUSY; return -1; }
    int changed = screen->invalid || screen->cursor_row != screen->old_row
                  || screen->cursor_column != screen->old_column;
    for (unsigned at = 0; at < screen->rows; ++at) {
        struct buffer *current = &screen->current[at], *previous = &screen->previous[at];
        if (!screen->invalid && current->length == previous->length
            && (!current->length || !memcmp(current->data, previous->data, current->length))) continue;
        if (!screen->pending.length && string(&screen->pending, "\033[?25l") < 0) return -1;
        char position[48];
        int length = snprintf(position, sizeof(position), "\033[%u;1H", at + 1);
        if (append(&screen->pending, position, (size_t)length) < 0
            || append(&screen->pending, current->data, current->length) < 0
            || string(&screen->pending, "\033[K") < 0) return -1;
        /* The final row image is immutable until the queued bytes drain. */
        previous->length = 0;
        if (append(previous, current->data, current->length) < 0) return -1;
        changed = 1;
    }
    if (changed) {
        char position[64];
        int length = snprintf(position, sizeof(position), "\033[%u;%uH\033[?25h",
                              screen->cursor_row, screen->cursor_column);
        if (append(&screen->pending, position, (size_t)length) < 0) return -1;
    }
    screen->old_row = screen->cursor_row; screen->old_column = screen->cursor_column;
    screen->invalid = 0;
    return t4_screen_drain(screen);
}

void t4_screen_close(struct t4_screen *screen)
{
    if (!screen) return;
    /* Normal exits flush immediately. A stopped/unread terminal must not hang
     * shutdown forever; allow a small bounded grace period for mode restore. */
    for (unsigned at = 0; at < 5 && t4_screen_busy(screen); ++at) {
        struct pollfd descriptor = {screen->fd, POLLOUT, 0};
        if (poll(&descriptor, 1, 20) > 0 && t4_screen_drain(screen) < 0) break;
    }
    close(screen->fd); free_rows(screen); free(screen->pending.data); free(screen);
}
```

### 附录 17：src/client/state.c

```c
#include "task4/client_state.h"
#include "task4/files.h"
#include "task4/text.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SESSION_LIMIT 10000U
#define HISTORY_HEADER 64U

static int private_directory(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        errno = EACCES; return -1;
    }
    return 0;
}

static int session_id(const char *id)
{
    if (!id || strlen(id) != 14 || strncmp(id, "session-", 8)) return 0;
    for (size_t i = 8; i < 14; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'Z') ||
              (id[i] >= 'a' && id[i] <= 'z'))) return 0;
    return 1;
}

/* Walk each component through a directory descriptor: existing symlinks and
 * regular files cannot become parents of automatically managed client state. */
static int directory_open(const char *path, int create)
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char copy[PATH_MAX];
    strcpy(copy, path);
    int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (create) {
            if (mkdirat(fd, part, 0700) == 0) {
                if (fsync(fd) < 0) goto fail;
            } else if (errno != EEXIST) goto fail;
        }
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) goto fail;
        close(fd);
        fd = child;
    }
    return fd;
fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

int t4_client_state_new(const char *session_dir, char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !out || !capacity) { errno = EINVAL; return -1; }
    out[0] = 0;
    char temporary[PATH_MAX];
    int n = snprintf(temporary, sizeof(temporary), "%s/session-XXXXXX", session_dir);
    static const char checkpoint[] = "/checkpoint.ckpt";
    if (n < 0 || (size_t)n >= sizeof(temporary) ||
        (size_t)n + sizeof(checkpoint) > capacity) { errno = ENAMETOOLONG; return -1; }
    int parent = directory_open(session_dir, 1);
    if (parent < 0) return -1;
    if (private_directory(parent) < 0) { int saved = errno; close(parent); errno = saved; return -1; }
    if (!mkdtemp(temporary)) { int saved = errno; close(parent); errno = saved; return -1; }
    if (fsync(parent) < 0) {
        int saved = errno;
        (void)rmdir(temporary);
        close(parent);
        errno = saved;
        return -1;
    }
    close(parent);
    memcpy(out, temporary, (size_t)n);
    memcpy(out + n, checkpoint, sizeof(checkpoint));
    return 0;
}

/* Metadata is informational only. Stale PID text is ignored unless flock says
 * that another client currently holds this exact lock inode. */
static const char *session_status(int directory, long *owner)
{
    *owner = 0;
    int fd = openat(directory, "checkpoint.ckpt.lock",
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat lock_metadata;
        if (fstat(fd, &lock_metadata) < 0 || !S_ISREG(lock_metadata.st_mode) ||
            lock_metadata.st_nlink != 1 || lock_metadata.st_uid != geteuid()) {
            close(fd);
            return "invalid-lock";
        }
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                close(fd);
                return "unavailable";
            }
            char metadata[64], extra;
            ssize_t n = pread(fd, metadata, sizeof(metadata) - 1, 0);
            if (n > 0) {
                metadata[n] = 0;
                long pid;
                if (sscanf(metadata, "pid=%ld %c", &pid, &extra) == 1 && pid > 0) *owner = pid;
            }
            close(fd);
            return "active";
        }
        close(fd);
    } else if (errno != ENOENT) return errno == ELOOP ? "invalid-lock" : "unavailable";
    struct stat st;
    if (fstatat(directory, "checkpoint.ckpt", &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? "uninitialized" : "unavailable";
    return S_ISREG(st.st_mode) ? "saved" : "invalid-file";
}

static int session_compare(const void *left, const void *right)
{
    const struct t4_client_session *a = left, *b = right;
    return strcmp(a->id, b->id);
}

int t4_client_state_scan(const char *session_dir, t4_client_session_fn visit, void *opaque)
{
    if (!visit) { errno = EINVAL; return -1; }
    int fd = directory_open(session_dir, 0);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        return 0;
    }
    if (private_directory(fd) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    int result = 0;
    size_t count = 0, capacity = 0;
    struct t4_client_session *sessions = NULL;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (!session_id(entry->d_name)) continue;
        int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) continue;
        if (private_directory(child) < 0) { close(child); continue; }
        long owner;
        const char *status = session_status(child, &owner);
        close(child);
        if (count == SESSION_LIMIT) { errno = EOVERFLOW; result = -1; break; }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > SESSION_LIMIT) next = SESSION_LIMIT;
            void *p = realloc(sessions, next * sizeof(*sessions));
            if (!p) { result = -1; break; }
            sessions = p; capacity = next;
        }
        struct t4_client_session *item = &sessions[count];
        snprintf(item->id, sizeof(item->id), "%.14s", entry->d_name);
        snprintf(item->status, sizeof(item->status), "%s", status);
        item->owner = owner;
        int n = snprintf(item->path, sizeof(item->path), "%s/%s/checkpoint.ckpt", session_dir, item->id);
        if (n < 0 || (size_t)n >= sizeof(item->path)) { errno = ENAMETOOLONG; result = -1; break; }
        ++count;
    }
    int saved = errno;
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    if (!result) {
        if (count > 1) qsort(sessions, count, sizeof(*sessions), session_compare);
        for (size_t i = 0; i < count; ++i) {
            if (visit(&sessions[i], opaque) < 0) { result = -1; saved = errno; break; }
        }
    }
    free(sessions);
    errno = saved;
    return result;
}

static int session_print(const struct t4_client_session *item, void *opaque)
{
    size_t *count = opaque;
    if (printf("checkpoint=%s status=%s", item->path, item->status) < 0 ||
        (item->owner && printf(" pid=%ld", item->owner) < 0) || putchar('\n') == EOF) return -1;
    ++*count;
    return 0;
}

int t4_client_state_list(const char *session_dir)
{
    size_t count = 0;
    if (t4_client_state_scan(session_dir, session_print, &count) < 0) return -1;
    return !count && printf("No client sessions in %s\n", session_dir) < 0 ? -1 : 0;
}

/* Split a leaf from its securely opened real parent. Explicit checkpoints may
 * live outside the managed root, but no path component may be a symlink. */
static int path_parent(const char *path, char leaf[NAME_MAX + 1])
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *name = slash ? slash + 1 : path;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
    if (strlen(name) > NAME_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(leaf, name);
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return directory_open(parent, 0);
}

static int regular_owned(int fd, off_t maximum)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0022) || st.st_size < 0 || st.st_size > maximum) {
        errno = EINVAL; return -1;
    }
    return 0;
}

int t4_client_state_resolve(const char *session_dir, const char *id_or_path,
                          char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !out || !capacity || !id_or_path || !*id_or_path) {
        errno = EINVAL; return -1;
    }
    out[0] = 0;
    char candidate[PATH_MAX], leaf[NAME_MAX + 1];
    int n = session_id(id_or_path)
        ? snprintf(candidate, sizeof(candidate), "%s/%s/checkpoint.ckpt", session_dir, id_or_path)
        : snprintf(candidate, sizeof(candidate), "%s", id_or_path);
    if (n < 0 || (size_t)n >= sizeof(candidate) || (size_t)n >= capacity) {
        errno = ENAMETOOLONG; return -1;
    }
    int parent = path_parent(candidate, leaf);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int result = fd < 0 ? -1 : regular_owned(fd, (off_t)T4_MAX_RESPONSE * 20 + T4_MAX_PROMPT + 256);
    int saved = errno;
    if (fd >= 0) close(fd);
    close(parent);
    if (!result) memcpy(out, candidate, (size_t)n + 1);
    errno = saved;
    return result;
}

static int random_name(char *out, size_t size, const char *prefix)
{
    unsigned char bytes[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int saved = n ? errno : EIO; close(fd); errno = saved; return -1; }
        offset += (size_t)n;
    }
    close(fd);
    int n = snprintf(out, size, "%s%02x%02x%02x%02x%02x%02x%02x%02x", prefix,
                     bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int t4_client_state_delete(const char *session_dir, const char *id, char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !session_id(id) || !out || !capacity) {
        errno = EINVAL; return -1;
    }
    out[0] = 0;
    char destination[64], prefix[32];
    snprintf(prefix, sizeof(prefix), "%s-", id);
    if (random_name(destination, sizeof(destination), prefix) < 0) return -1;
    int n = snprintf(out, capacity, "%s/.trash/%s", session_dir, destination);
    if (n < 0 || (size_t)n >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    int parent = -1, child = -1, trash = -1, lock = -1, result = -1;
    parent = directory_open(session_dir, 0);
    if (parent < 0 || private_directory(parent) < 0) goto cleanup;
    child = openat(parent, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0 || private_directory(child) < 0) goto cleanup;
    /* A symlink checkpoint must not be used as a way to affect external data. */
    struct stat checkpoint;
    if (fstatat(child, "checkpoint.ckpt", &checkpoint, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(checkpoint.st_mode) || checkpoint.st_uid != geteuid() || checkpoint.st_nlink != 1) {
            errno = EINVAL; goto cleanup;
        }
    } else if (errno != ENOENT) goto cleanup;
    lock = openat(child, "checkpoint.ckpt.lock",
                  O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || regular_owned(lock, 1024) < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) goto cleanup;
    if (mkdirat(parent, ".trash", 0700) < 0 && errno != EEXIST) goto cleanup;
    trash = openat(parent, ".trash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (trash < 0 || private_directory(trash) < 0) goto cleanup;
    struct stat before, current, held, named;
    if (fstat(child, &before) < 0 || fstatat(parent, id, &current, AT_SYMLINK_NOFOLLOW) < 0 ||
        fstat(lock, &held) < 0 || fstatat(child, "checkpoint.ckpt.lock", &named, AT_SYMLINK_NOFOLLOW) < 0)
        goto cleanup;
    if (before.st_dev != current.st_dev || before.st_ino != current.st_ino ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino) { errno = ESTALE; goto cleanup; }
    /* Linux renameat2 avoids replacing even an unexpectedly created empty
     * destination. The lock remains held across rename and directory fsync. */
    if (syscall(SYS_renameat2, parent, id, trash, destination, 1U) < 0) goto cleanup;
    if (fsync(trash) < 0 || fsync(parent) < 0) {
        int saved = errno;
        (void)syscall(SYS_renameat2, trash, destination, parent, id, 1U);
        errno = saved; goto cleanup;
    }
    result = 0;
cleanup:
    {
        int saved = errno;
        if (lock >= 0) close(lock);
        if (trash >= 0) close(trash);
        if (child >= 0) close(child);
        if (parent >= 0) close(parent);
        if (result < 0) out[0] = 0;
        errno = saved;
        return result;
    }
}

static int token_valid(const char *token)
{
    if (!token || strlen(token) != 32) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t length)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}

static int history_directory(const char *path, int create)
{
    char leaf[NAME_MAX + 1], name[NAME_MAX + 1];
    int parent = path_parent(path, leaf);
    if (parent < 0) return -1;
    int n = snprintf(name, sizeof(name), "%s.history", leaf);
    int fd = -1;
    if (n < 0 || (size_t)n >= sizeof(name)) { errno = ENAMETOOLONG; goto cleanup; }
    if (create && mkdirat(parent, name, 0700) == 0) {
        if (fsync(parent) < 0) goto cleanup;
    } else if (create && errno != EEXIST) goto cleanup;
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && private_directory(fd) < 0) { int saved = errno; close(fd); fd = -1; errno = saved; }
cleanup:
    { int saved = errno; close(parent); errno = saved; return fd; }
}

static int exact_read(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EINVAL; return -1; }
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int text_valid(const unsigned char *p, uint32_t length)
{
    for (uint32_t offset = 0; offset < length;) {
        unsigned width = t4_character_width(p[offset]);
        if (!width || width > length - offset || !t4_character_valid(p + offset, width)) return 0;
        offset += width;
    }
    return 1;
}

static int history_load(int directory, const char *name, struct t4_checkpoint *c)
{
    memset(c, 0, sizeof(*c));
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = -1;
    unsigned char header[HISTORY_HEADER];
    uint32_t fields[6];
    if (regular_owned(fd, HISTORY_HEADER + T4_MAX_PROMPT + T4_MAX_RESPONSE) < 0 ||
        exact_read(fd, header, sizeof(header)) < 0) goto cleanup;
    if (memcmp(header, "T4HIST1\n", 8)) { errno = EINVAL; goto cleanup; }
    memcpy(c->session, header + 8, 32);
    memcpy(fields, header + 40, sizeof(fields));
    c->request = ntohl(fields[0]); c->prompt_length = ntohl(fields[1]);
    c->offset = ntohl(fields[2]); c->done = ntohl(fields[3]); c->file_lines = ntohl(fields[4]);
    if (!token_valid(c->session) || !c->request || c->request > SESSION_LIMIT ||
        c->prompt_length > T4_MAX_PROMPT || c->offset > T4_MAX_RESPONSE ||
        (c->done != 1 && c->done != 2)) { errno = EINVAL; goto cleanup; }
    struct stat st;
    if (fstat(fd, &st) < 0) goto cleanup;
    if (st.st_size != (off_t)(HISTORY_HEADER + c->prompt_length + c->offset)) { errno = EINVAL; goto cleanup; }
    c->answer = malloc((size_t)c->offset + 1);
    if (!c->answer || exact_read(fd, c->prompt, c->prompt_length) < 0 ||
        exact_read(fd, c->answer, c->offset) < 0) goto cleanup;
    c->answer[c->offset] = 0;
    uint32_t hash = hash_bytes(UINT32_C(2166136261), header, 60);
    hash = hash_bytes(hash, c->prompt, c->prompt_length);
    hash = hash_bytes(hash, c->answer, c->offset);
    if (hash != ntohl(fields[5]) || !text_valid(c->prompt, c->prompt_length) ||
        !text_valid(c->answer, c->offset)) { errno = EINVAL; goto cleanup; }
    result = 0;
cleanup:
    {
        int saved = errno;
        close(fd);
        if (result < 0) { free(c->answer); c->answer = NULL; }
        errno = saved; return result;
    }
}

static int history_same(int directory, const char *name, const struct t4_checkpoint *c)
{
    struct t4_checkpoint old;
    if (history_load(directory, name, &old) < 0) return -1;
    int same = !strcmp(old.session, c->session) && old.request == c->request &&
        old.prompt_length == c->prompt_length && old.offset == c->offset &&
        old.done == c->done && old.file_lines == c->file_lines &&
        !memcmp(old.prompt, c->prompt, c->prompt_length) &&
        (!c->offset || !memcmp(old.answer, c->answer, c->offset));
    free(old.answer);
    if (!same) { errno = EEXIST; return -1; }
    return 0;
}

int t4_client_history_archive(const char *path, const struct t4_checkpoint *c)
{
    if (!c || !token_valid(c->session) || !c->request || c->request > SESSION_LIMIT ||
        (c->done != 1 && c->done != 2) || c->prompt_length > T4_MAX_PROMPT ||
        c->offset > T4_MAX_RESPONSE || (c->offset && !c->answer) ||
        !text_valid(c->prompt, c->prompt_length) || !text_valid(c->answer, c->offset)) {
        errno = EINVAL; return -1;
    }
    int directory = history_directory(path, 1);
    if (directory < 0) return -1;
    char name[32], temporary[64] = "";
    snprintf(name, sizeof(name), "turn-%08u.ckpt", c->request);
    struct stat st;
    int result = -1, fd = -1;
    if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        result = history_same(directory, name, c); goto cleanup;
    }
    if (errno != ENOENT || random_name(temporary, sizeof(temporary), ".pending-") < 0) goto cleanup;
    fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { temporary[0] = 0; goto cleanup; }
    unsigned char header[HISTORY_HEADER];
    uint32_t fields[6] = {htonl(c->request), htonl(c->prompt_length), htonl(c->offset),
                          htonl(c->done), htonl(c->file_lines), 0};
    memcpy(header, "T4HIST1\n", 8); memcpy(header + 8, c->session, 32);
    memcpy(header + 40, fields, sizeof(fields));
    uint32_t hash = hash_bytes(UINT32_C(2166136261), header, 60);
    hash = hash_bytes(hash, c->prompt, c->prompt_length);
    hash = hash_bytes(hash, c->answer, c->offset);
    fields[5] = htonl(hash); memcpy(header + 60, &fields[5], 4);
    if (t4_write_all(fd, header, sizeof(header)) < 0 ||
        t4_write_all(fd, c->prompt, c->prompt_length) < 0 ||
        t4_write_all(fd, c->answer, c->offset) < 0 || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    /* Atomic publish without replacing existing content, including symlinks.
     * Pending files from an interrupted write are ignored by enumeration. */
    if (syscall(SYS_renameat2, directory, temporary, directory, name, 1U) < 0) {
        if (errno == EEXIST) result = history_same(directory, name, c);
        goto cleanup;
    }
    temporary[0] = 0;
    result = fsync(directory);
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (*temporary) (void)unlinkat(directory, temporary, 0);
        close(directory);
        errno = saved; return result;
    }
}

static int request_compare(const void *a, const void *b)
{
    uint32_t left = *(const uint32_t *)a, right = *(const uint32_t *)b;
    return left > right ? 1 : left < right ? -1 : 0;
}

int t4_client_history_scan(const char *path, const char *session,
                         t4_client_history_fn visit, void *opaque)
{
    if (!token_valid(session) || !visit) { errno = EINVAL; return -1; }
    int fd = history_directory(path, 0);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    uint32_t *requests = malloc(SESSION_LIMIT * sizeof(*requests));
    if (!requests) { closedir(directory); return -1; }
    size_t count = 0;
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (strlen(entry->d_name) != 18 || strncmp(entry->d_name, "turn-", 5) ||
            strcmp(entry->d_name + 13, ".ckpt")) continue;
        uint32_t request = 0;
        for (size_t i = 5; i < 13; ++i) {
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') { request = 0; break; }
            request = request * 10 + (unsigned)(entry->d_name[i] - '0');
        }
        if (!request || request > SESSION_LIMIT || count == SESSION_LIMIT) {
            errno = EINVAL; result = -1; break;
        }
        requests[count++] = request;
    }
    if (!result && count > 1) qsort(requests, count, sizeof(*requests), request_compare);
    for (size_t i = 0; !result && i < count; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "turn-%08u.ckpt", requests[i]);
        struct t4_checkpoint c;
        if (history_load(fd, name, &c) < 0) { result = -1; break; }
        if (c.request != requests[i] || strcmp(c.session, session)) { errno = EINVAL; result = -1; }
        else result = visit(&c, opaque);
        free(c.answer);
    }
    int saved = errno;
    free(requests);
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    errno = saved; return result;
}

void t4_client_state_lock_error(const char *path, int error)
{
    fprintf(stderr, "[client] checkpoint open/lock %s: ", path ? path : "(unspecified)");
    if (error == EWOULDBLOCK || error == EAGAIN) {
        fputs("checkpoint is in use: another client owns this checkpoint, possibly paused. Continue in its terminal, "
              "or start a new client for an independent session. Resume after the owner exits.\n", stderr);
    } else if (error == EINVAL) {
        fputs("invalid or damaged checkpoint/lock data. Keep this file for inspection; "
              "start a new client to create a separate session.\n", stderr);
    } else if (error == ENOENT) {
        fputs("file or parent directory is missing. --resume requires an existing checkpoint; "
              "a new client creates its own session directory.\n", stderr);
    } else if (error == EACCES || error == EPERM || error == EROFS) {
        fputs("cannot read or write this path. Check directory/file permissions and filesystem access.\n", stderr);
    } else if (error == ENOTDIR || error == ELOOP) {
        fputs("a path component is not a directory or is a symbolic link. "
              "Choose a regular checkpoint in a real directory.\n", stderr);
    } else {
        fprintf(stderr, "%s. Check the checkpoint path and storage.\n", strerror(error));
    }
}
```

### 附录 18：src/client/status.c

```c
#include "task4/status.h"

#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn/jsmn.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void bind_strings(struct t4_client_status *status)
{
    status->ui.phase = status->phase;
    status->ui.model = status->model;
}

void t4_status_init(struct t4_client_status *status)
{
    memset(status, 0, sizeof(*status));
    strcpy(status->phase, "waiting");
    bind_strings(status);
}

static int equals(const char *json, const jsmntok_t *token, const char *key)
{
    return token->type == JSMN_STRING && (size_t)(token->end - token->start) == strlen(key)
           && !memcmp(json + token->start, key, strlen(key));
}

/* Model and phase labels are short UTF-8 strings. Decode JSON escapes rather
 * than displaying backslashes, and reject truncated labels/control characters. */
static int label(const char *json, const jsmntok_t *token, char *out, size_t capacity)
{
    if (token->type != JSMN_STRING) return -1;
    size_t used = 0;
    for (int at = token->start; at < token->end;) {
        unsigned char byte = (unsigned char)json[at++];
        if (byte == '\\') {
            if (at == token->end) return -1;
            byte = (unsigned char)json[at++];
            if (byte == 'u') {
                if (token->end - at < 4) return -1;
                unsigned code = 0;
                for (int n = 0; n < 4; ++n) {
                    unsigned char digit = (unsigned char)json[at++];
                    unsigned value = digit >= '0' && digit <= '9' ? digit - '0' :
                                     digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                                     digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : 16;
                    if (value == 16) return -1;
                    code = code * 16 + value;
                }
                if (code < 32 || code == 127 || (code >= 0xd800 && code <= 0xdfff)) return -1;
                unsigned char utf8[3];
                size_t width;
                if (code < 128) { utf8[0] = (unsigned char)code; width = 1; }
                else if (code < 2048) {
                    utf8[0] = 0xc0U | (code >> 6); utf8[1] = 0x80U | (code & 63U); width = 2;
                } else {
                    utf8[0] = 0xe0U | (code >> 12); utf8[1] = 0x80U | ((code >> 6) & 63U);
                    utf8[2] = 0x80U | (code & 63U); width = 3;
                }
                if (used + width >= capacity) return -1;
                memcpy(out + used, utf8, width); used += width;
                continue;
            }
            if (byte != '"' && byte != '\\' && byte != '/') return -1;
        }
        if (byte < 32 || byte == 127 || used + 1 >= capacity) return -1;
        out[used++] = (char)byte;
    }
    out[used] = 0;
    return 0;
}

static int number(const char *json, const jsmntok_t *token, double *out, int integer)
{
    int size = token->end - token->start;
    char buffer[64], *end;
    if (token->type != JSMN_PRIMITIVE || size <= 0 || size >= (int)sizeof(buffer)) return -1;
    memcpy(buffer, json + token->start, (size_t)size); buffer[size] = 0;
    if (buffer[0] < '0' || buffer[0] > '9') return -1;
    if (integer) {
        for (int at = 0; at < size; ++at) if (buffer[at] < '0' || buffer[at] > '9') return -1;
    }
    errno = 0;
    double value = strtod(buffer, &end);
    if (errno || *end || !isfinite(value) || value < 0 || value > UINT32_MAX) return -1;
    *out = value;
    return 0;
}

int t4_status_update(struct t4_client_status *status, const char *json, size_t length)
{
    jsmn_parser parser;
    jsmntok_t tokens[256];
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, length, tokens, 256);
    if (count < 1 || tokens[0].type != JSMN_OBJECT) return -1;
    for (size_t at = (size_t)tokens[0].end; at < length; ++at)
        if (json[at] != ' ' && json[at] != '\n' && json[at] != '\r' && json[at] != '\t') return -1;
    struct t4_client_status next = *status;
    for (int at = 1; at < count;) {
        const jsmntok_t *key = &tokens[at++];
        if (key->type != JSMN_STRING || at == count) return -1;
        const jsmntok_t *value = &tokens[at];
        double converted;
        if (equals(json, key, "phase")) {
            if (label(json, value, next.phase, sizeof(next.phase)) < 0) return -1;
        } else if (equals(json, key, "model")) {
            if (label(json, value, next.model, sizeof(next.model)) < 0) return -1;
        } else if (equals(json, key, "prompt_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.prompt_tokens = (uint32_t)converted;
        } else if (equals(json, key, "context_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.context_tokens = (uint32_t)converted;
        } else if (equals(json, key, "generated_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.generated_tokens = (uint32_t)converted;
        } else if (equals(json, key, "tokens_per_second")) {
            if (number(json, value, &converted, 0) < 0) return -1;
            next.ui.tokens_per_second = converted;
        } else if (equals(json, key, "elapsed_seconds")) {
            if (number(json, value, &converted, 0) < 0) return -1;
            next.ui.elapsed_seconds = converted;
        }
        int end = value->end;
        do { ++at; } while (at < count && tokens[at].start < end);
    }
    *status = next;
    bind_strings(status);
    return 0;
}
```

### 附录 19：src/client/terminal.c

```c
#define _XOPEN_SOURCE 700

#include "task4/terminal.h"
#include "task4/text.h"
#include "task4/view.h"
#include "task4/screen.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define HISTORY 50U
#define LIMIT 4096U

struct t4_terminal {
    struct termios original, editing;
    unsigned rows, columns;
    unsigned char draft[LIMIT + 1], backup[LIMIT + 1], input[4096], utf8[4];
    size_t length, cursor, view, backup_length, backup_cursor;
    size_t input_at, input_length;
    unsigned utf8_have, utf8_width;
    char prompt[128], escape[64];
    size_t escape_length;
    char *history[HISTORY];
    unsigned history_count, history_at;
    int invalid, paste, suspended, discard_escape, mouse_remaining;
    unsigned char mouse[3];
    struct t4_view *view_state;
    struct t4_screen *screen;
    int64_t last_paint;
};

static int output(struct t4_terminal *terminal, const void *data, size_t length)
{ return t4_screen_append(terminal->screen, data, length); }
static int command(struct t4_terminal *terminal, const char *text)
{ return output(terminal, text, strlen(text)); }

static size_t next(const unsigned char *text, size_t at, size_t length)
{
    unsigned width = t4_character_width(text[at]);
    return at + (width && width <= length - at ? width : 1);
}

static size_t previous(const unsigned char *text, size_t at)
{
    if (!at) return 0;
    --at;
    while (at && (text[at] & 0xc0U) == 0x80U) --at;
    return at;
}

static wchar_t wide(const unsigned char *text, size_t width)
{
    mbstate_t state = {0};
    wchar_t result;
    size_t consumed = mbrtowc(&result, (const char *)text, width, &state);
    return consumed == (size_t)-1 || consumed == (size_t)-2 ? L'?' : result;
}

static unsigned cells(const unsigned char *text, size_t from, size_t until)
{
    unsigned total = 0;
    while (from < until) {
        size_t after = next(text, from, until);
        int width = wcwidth(wide(text + from, after - from));
        total += width < 0 ? 1U : (unsigned)width;
        from = after;
    }
    return total;
}

static int geometry(struct t4_terminal *terminal)
{
    struct winsize size = {0};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0) return -1;
    terminal->rows = size.ws_row ? size.ws_row : 24;
    terminal->columns = size.ws_col ? size.ws_col : 80;
    /* Bound screen memory even if a peer sends an unreasonable winsize. */
    if (terminal->rows > 512) terminal->rows = 512;
    if (terminal->columns > 2048) terminal->columns = 2048;
    if (t4_screen_resize(terminal->screen, terminal->rows, terminal->columns) < 0) return -1;
    if (terminal->view_state) t4_view_resize(terminal->view_state, terminal->rows, terminal->columns);
    return 0;
}

static int paint(struct t4_terminal *terminal)
{
    if (t4_view_render(terminal->view_state, terminal->screen) < 0) return -1;
    if (terminal->rows < 4 || terminal->columns < 12) {
        t4_screen_cursor(terminal->screen, 1, 1);
        return t4_screen_submit(terminal->screen);
    }
    size_t prompt_length = strlen(terminal->prompt), shown_prompt = 0;
    unsigned prompt_cells = 0;
    while (shown_prompt < prompt_length) {
        size_t after = next((const unsigned char *)terminal->prompt, shown_prompt, prompt_length);
        unsigned width = cells((const unsigned char *)terminal->prompt, shown_prompt, after);
        if (prompt_cells + width > terminal->columns / 3) break;
        shown_prompt = after; prompt_cells += width;
    }
    unsigned available = terminal->columns - prompt_cells - 2;
    if (terminal->view > terminal->cursor) terminal->view = terminal->cursor;
    while (cells(terminal->draft, terminal->view, terminal->cursor) >= available)
        terminal->view = next(terminal->draft, terminal->view, terminal->length);
    size_t end = terminal->view;
    unsigned visible = 0;
    while (end < terminal->length) {
        size_t after = next(terminal->draft, end, terminal->length);
        unsigned width = cells(terminal->draft, end, after);
        if (visible + width > available) break;
        visible += width; end = after;
    }
    if (t4_screen_row(terminal->screen, terminal->rows, t4_view_style(terminal->view_state, T4_VIEW_ACCENT)) < 0
        || output(terminal, terminal->prompt, shown_prompt) < 0
        || command(terminal, t4_view_style(terminal->view_state, T4_VIEW_TEXT)) < 0
        || command(terminal, terminal->view ? "<" : " ") < 0
        || output(terminal, terminal->draft + terminal->view, end - terminal->view) < 0
        || command(terminal, end < terminal->length ? ">" : " ") < 0) return -1;
    t4_screen_cursor(terminal->screen, terminal->rows, prompt_cells + 2
                     + cells(terminal->draft, terminal->view, terminal->cursor));
    return t4_screen_submit(terminal->screen);
}

static int refresh(struct t4_terminal *terminal, int force)
{
    if (terminal->suspended) return 0;
    if (t4_screen_drain(terminal->screen) < 0) return -1;
    if (t4_screen_busy(terminal->screen)) return 0;
    if (!force && !t4_view_is_dirty(terminal->view_state)) return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    int64_t milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (!force && milliseconds - terminal->last_paint < 34) return 0;
    if (paint(terminal) < 0) return -1;
    terminal->last_paint = milliseconds;
    return 0;
}

static int draw(struct t4_terminal *terminal)
{
    t4_view_dirty(terminal->view_state);
    return refresh(terminal, 0);
}

static int layout(struct t4_terminal *terminal)
{
    /* Save only private mouse modes that we change, and restore them on exit.
     * Terminals lacking xterm's save/restore simply ignore those controls; we
     * explicitly disable mouse reporting before returning to the shell. */
    if (t4_screen_control(terminal->screen, "\033[?1000;1006;1007s\033[?1049h\033[r\033[?2004h\033[?1007l\033[?1000h\033[?1006h\033[?25h") < 0) return -1;
    t4_screen_invalidate(terminal->screen);
    t4_view_resize(terminal->view_state, terminal->rows, terminal->columns);
    return refresh(terminal, 1);
}

struct t4_terminal *t4_terminal_open(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) return NULL;
    struct t4_terminal *terminal = calloc(1, sizeof(*terminal));
    if (!terminal) return NULL;
    terminal->view_state = t4_view_open();
    if (!terminal->view_state) { free(terminal); return NULL; }
    terminal->screen = t4_screen_open();
    if (!terminal->screen) { t4_view_close(terminal->view_state); free(terminal); return NULL; }
    (void)setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1) (void)setlocale(LC_CTYPE, "C.UTF-8");
    if (tcgetattr(STDIN_FILENO, &terminal->original) < 0 || geometry(terminal) < 0) {
        t4_screen_close(terminal->screen); t4_view_close(terminal->view_state); free(terminal); return NULL;
    }
    terminal->editing = terminal->original;
    terminal->editing.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | IEXTEN);
    terminal->editing.c_lflag |= ISIG;
    terminal->editing.c_iflag &= (tcflag_t)~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
#ifdef IUTF8
    terminal->editing.c_iflag |= IUTF8;
#endif
    terminal->editing.c_cc[VMIN] = 0; terminal->editing.c_cc[VTIME] = 0;
    strcpy(terminal->prompt, "question> ");
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal->editing) < 0) {
        t4_screen_close(terminal->screen); t4_view_close(terminal->view_state); free(terminal); return NULL;
    }
    if (layout(terminal) < 0) { t4_terminal_close(terminal); return NULL; }
    return terminal;
}

int t4_terminal_prompt(struct t4_terminal *terminal, const char *prompt)
{
    if (!terminal || !prompt) { errno = EINVAL; return -1; }
    size_t length = strlen(prompt), copied = 0, at = 0;
    while (at < length) {
        size_t after = next((const unsigned char *)prompt, at, length);
        if (copied + after - at >= sizeof(terminal->prompt)) break;
        wchar_t character = wide((const unsigned char *)prompt + at, after - at);
        if (character >= 32 && character != 127 && !(character >= 128 && character <= 159)) {
            memcpy(terminal->prompt + copied, prompt + at, after - at); copied += after - at;
        }
        at = after;
    }
    terminal->prompt[copied] = 0;
    return draw(terminal);
}

int t4_terminal_write(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal || (!data && length)) { errno = EINVAL; return -1; }
    if (t4_view_notice(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}

int t4_terminal_resize(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (geometry(terminal) < 0) return -1;
    return refresh(terminal, 1);
}

int t4_terminal_tick(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    return refresh(terminal, 0);
}
int t4_terminal_flush(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    return refresh(terminal, 1);
}
int t4_terminal_reset(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    for (unsigned at = 0; at < terminal->history_count; ++at) free(terminal->history[at]);
    terminal->history_count = terminal->history_at = 0;
    terminal->backup_length = terminal->backup_cursor = 0; terminal->backup[0] = 0;
    terminal->input_at = terminal->input_length = 0;
    terminal->discard_escape = terminal->mouse_remaining = 0;
    t4_view_reset(terminal->view_state);
    t4_terminal_clear_input(terminal);
    return refresh(terminal, 1);
}
int t4_terminal_session(struct t4_terminal *terminal, const char *session, const char *model)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_session(terminal->view_state, session, model) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_begin_turn(struct t4_terminal *terminal, uint32_t request, const char *prompt, uint32_t offset)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_begin(terminal->view_state, request, prompt, offset) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_thinking(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_thinking(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_answer(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_answer(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_status(struct t4_terminal *terminal, const struct t4_ui_status *status)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_status(terminal->view_state, status) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_end_turn(struct t4_terminal *terminal, const char *state, const char *summary)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_end(terminal->view_state, state, summary) < 0) return -1;
    return refresh(terminal, 1);
}
int t4_terminal_theme(struct t4_terminal *terminal, int dark)
{
    if (!terminal) { errno = EINVAL; return -1; }
    t4_view_theme(terminal->view_state, dark);
    return refresh(terminal, 1);
}
int t4_terminal_thinking_visible(struct t4_terminal *terminal, int show)
{
    if (!terminal) { errno = EINVAL; return -1; }
    t4_view_thinking_visible(terminal->view_state, show);
    return refresh(terminal, 1);
}

void t4_terminal_clear_input(struct t4_terminal *terminal)
{
    if (!terminal) return;
    /* Explicit cancellation of an unfinished paste also drops already-read
     * paste bytes. Enter otherwise retains subsequent buffered line events. */
    if (terminal->paste || terminal->escape_length) terminal->input_at = terminal->input_length;
    terminal->length = terminal->cursor = terminal->view = 0;
    terminal->draft[0] = 0; terminal->invalid = 0;
    terminal->utf8_have = terminal->utf8_width = 0;
    terminal->escape_length = 0; terminal->paste = 0;
    terminal->discard_escape = terminal->mouse_remaining = 0;
    terminal->history_at = terminal->history_count;
    (void)draw(terminal);
}

static void remove_range(struct t4_terminal *terminal, size_t from, size_t until)
{
    memmove(terminal->draft + from, terminal->draft + until, terminal->length - until + 1);
    terminal->length -= until - from; terminal->cursor = from;
    if (terminal->view > from) terminal->view = from;
    terminal->invalid = 0;
}

static void insert(struct t4_terminal *terminal, const unsigned char *text, size_t length)
{
    if (length > LIMIT - terminal->length) { terminal->invalid = 1; return; }
    memmove(terminal->draft + terminal->cursor + length, terminal->draft + terminal->cursor,
            terminal->length - terminal->cursor + 1);
    memcpy(terminal->draft + terminal->cursor, text, length);
    terminal->cursor += length; terminal->length += length;
}

static void history_move(struct t4_terminal *terminal, int backward)
{
    if (backward && terminal->history_at == terminal->history_count) {
        memcpy(terminal->backup, terminal->draft, terminal->length + 1);
        terminal->backup_length = terminal->length; terminal->backup_cursor = terminal->cursor;
    }
    if (backward && terminal->history_at) --terminal->history_at;
    else if (!backward && terminal->history_at < terminal->history_count) ++terminal->history_at;
    else return;
    if (terminal->history_at == terminal->history_count) {
        memcpy(terminal->draft, terminal->backup, terminal->backup_length + 1);
        terminal->length = terminal->backup_length; terminal->cursor = terminal->backup_cursor;
    } else {
        strcpy((char *)terminal->draft, terminal->history[terminal->history_at]);
        terminal->length = terminal->cursor = strlen((char *)terminal->draft);
    }
    terminal->view = 0; terminal->invalid = 0;
}

int t4_terminal_history(struct t4_terminal *terminal, const char *prompt)
{
    if (!terminal || !prompt || strlen(prompt) > LIMIT) { errno = EINVAL; return -1; }
    if (!*prompt || (terminal->history_count
        && !strcmp(terminal->history[terminal->history_count - 1], prompt))) return 0;
    char *copy = strdup(prompt);
    if (!copy) return -1;
    if (terminal->history_count == HISTORY) {
        free(terminal->history[0]);
        memmove(terminal->history, terminal->history + 1, (HISTORY - 1) * sizeof(char *));
        --terminal->history_count;
    }
    terminal->history[terminal->history_count++] = copy;
    terminal->history_at = terminal->history_count;
    return 0;
}

static void text_byte(struct t4_terminal *terminal, unsigned char byte)
{
    if (terminal->utf8_have) {
        if ((byte & 0xc0U) != 0x80U) {
            terminal->invalid = 1; terminal->utf8_have = 0;
            text_byte(terminal, byte); return;
        }
        terminal->utf8[terminal->utf8_have++] = byte;
        if (terminal->utf8_have == terminal->utf8_width) {
            if (t4_character_valid(terminal->utf8, terminal->utf8_width))
                insert(terminal, terminal->utf8, terminal->utf8_width);
            else terminal->invalid = 1;
            terminal->utf8_have = 0;
        }
    } else if (byte < 128) {
        if (byte >= 32 && byte != 127) insert(terminal, &byte, 1);
    } else {
        terminal->utf8_width = t4_character_width(byte);
        if (!terminal->utf8_width) terminal->invalid = 1;
        else { terminal->utf8[0] = byte; terminal->utf8_have = 1; }
    }
}

static void mouse_scroll(struct t4_terminal *terminal, unsigned button)
{
    /* Modifier bits do not turn a wheel into a keyboard history event. */
    button &= ~28U;
    if (button == 64 || button == 65)
        t4_view_scroll(terminal->view_state, button == 64, 3);
}

static void mouse_action(struct t4_terminal *terminal)
{
    const char *at = terminal->escape + 3;
    unsigned values[3];
    for (unsigned index = 0; index < 3; ++index) {
        if (*at < '0' || *at > '9') return;
        unsigned value = 0;
        while (*at >= '0' && *at <= '9') {
            if (value > 100000U) return;
            value = value * 10U + (unsigned)(*at++ - '0');
        }
        values[index] = value;
        if (index < 2 && *at++ != ';') return;
    }
    if (*at != 'M' || at[1] || !values[1] || !values[2]) return;
    mouse_scroll(terminal, values[0]);
}

static void escape_action(struct t4_terminal *terminal)
{
    const char *key = terminal->escape;
    if (!strncmp(key, "\033[<", 3)) { mouse_action(terminal); return; }
    if (!strcmp(key, "\033[201~")) { terminal->paste = 0; return; }
    if (terminal->paste) {
        for (size_t at = 1; at < terminal->escape_length; ++at)
            text_byte(terminal, (unsigned char)key[at]);
        return;
    }
    if (!strcmp(key, "\033[200~")) terminal->paste = 1;
    else if (!strcmp(key, "\033[5~")) t4_view_page(terminal->view_state, 1);
    else if (!strcmp(key, "\033[6~")) t4_view_page(terminal->view_state, 0);
    else if (!strcmp(key, "\033[A") || !strcmp(key, "\033OA")) history_move(terminal, 1);
    else if (!strcmp(key, "\033[B") || !strcmp(key, "\033OB")) history_move(terminal, 0);
    else if (!strcmp(key, "\033[D") || !strcmp(key, "\033OD")) terminal->cursor = previous(terminal->draft, terminal->cursor);
    else if ((!strcmp(key, "\033[C") || !strcmp(key, "\033OC")) && terminal->cursor < terminal->length)
        terminal->cursor = next(terminal->draft, terminal->cursor, terminal->length);
    else if (!strcmp(key, "\033[H") || !strcmp(key, "\033OH") || !strcmp(key, "\033[1~") || !strcmp(key, "\033[7~")) terminal->cursor = 0;
    else if (!strcmp(key, "\033[F") || !strcmp(key, "\033OF") || !strcmp(key, "\033[4~") || !strcmp(key, "\033[8~")) terminal->cursor = terminal->length;
    else if (!strcmp(key, "\033[3~") && terminal->cursor < terminal->length)
        remove_range(terminal, terminal->cursor, next(terminal->draft, terminal->cursor, terminal->length));
}

/* Return 1 for Enter, 2 for Ctrl+D on an empty draft, otherwise 0. */
static int key(struct t4_terminal *terminal, unsigned char byte)
{
    if (terminal->mouse_remaining) {
        terminal->mouse[3 - terminal->mouse_remaining--] = byte;
        if (!terminal->mouse_remaining && terminal->mouse[0] >= 32
            && terminal->mouse[1] >= 33 && terminal->mouse[2] >= 33)
            mouse_scroll(terminal, (unsigned)terminal->mouse[0] - 32);
        return 0;
    }
    if (terminal->discard_escape) {
        if (byte == 27) { terminal->discard_escape = 0; terminal->escape_length = 0; return key(terminal, byte); }
        if (byte == 'M' || byte == 'm' || byte == '\r' || byte == '\n') terminal->discard_escape = 0;
        return 0;
    }
    if (terminal->escape_length) {
        if (byte == 27) { terminal->escape_length = 0; return key(terminal, byte); }
        int mouse = terminal->escape_length >= 3 && !strncmp(terminal->escape, "\033[<", 3);
        if (terminal->escape_length + 1 >= sizeof(terminal->escape)) {
            terminal->escape_length = 0;
            terminal->discard_escape = mouse && byte != 'M' && byte != 'm';
            return 0;
        }
        terminal->escape[terminal->escape_length++] = (char)byte;
        terminal->escape[terminal->escape_length] = 0;
        if (!strcmp(terminal->escape, "\033[M")) {
            terminal->escape_length = 0; terminal->mouse_remaining = 3; return 0;
        }
        if (mouse) {
            if (byte == 'M' || byte == 'm') {
                escape_action(terminal); terminal->escape_length = 0;
            } else if (byte == '\r' || byte == '\n') terminal->escape_length = 0;
            return 0;
        }
        if (terminal->escape_length == 2 && byte != '[' && byte != 'O') {
            terminal->escape_length = 0;
            if (terminal->paste) text_byte(terminal, byte);
            else return key(terminal, byte);
        } else if (terminal->escape_length >= 3 && byte >= '@' && byte <= '~') {
            escape_action(terminal); terminal->escape_length = 0;
        }
        return 0;
    }
    if (byte == 27) { terminal->escape[0] = 27; terminal->escape[1] = 0; terminal->escape_length = 1; return 0; }
    if (terminal->paste) {
        if (byte == '\n' || byte == '\r' || byte == '\t') byte = ' ';
        text_byte(terminal, byte); return 0;
    }
    if (byte == '\r' || byte == '\n') {
        if (terminal->utf8_have) terminal->invalid = 1;
        return 1;
    }
    if (byte == 12) { t4_screen_invalidate(terminal->screen); (void)t4_terminal_flush(terminal); return 0; }
    if (byte == 4) {
        if (!terminal->length) return 2;
        if (terminal->cursor < terminal->length)
            remove_range(terminal, terminal->cursor, next(terminal->draft, terminal->cursor, terminal->length));
    } else if ((byte == 127 || byte == 8) && terminal->cursor)
        remove_range(terminal, previous(terminal->draft, terminal->cursor), terminal->cursor);
    else if (byte == 1) terminal->cursor = 0;
    else if (byte == 5) terminal->cursor = terminal->length;
    else if (byte == 21) remove_range(terminal, 0, terminal->cursor);
    else if (byte == 11) remove_range(terminal, terminal->cursor, terminal->length);
    else if (byte == 23) {
        size_t from = terminal->cursor;
        while (from) {
            size_t before = previous(terminal->draft, from);
            if (!iswspace(wide(terminal->draft + before, from - before))) break;
            from = before;
        }
        while (from) {
            size_t before = previous(terminal->draft, from);
            if (iswspace(wide(terminal->draft + before, from - before))) break;
            from = before;
        }
        remove_range(terminal, from, terminal->cursor);
    } else if (byte == '\t') { unsigned char space = ' '; insert(terminal, &space, 1); }
    else text_byte(terminal, byte);
    return 0;
}

int t4_terminal_pending(const struct t4_terminal *terminal)
{
    return terminal && terminal->input_at < terminal->input_length;
}

int t4_terminal_read(struct t4_terminal *terminal, unsigned char line[4097], uint32_t *length)
{
    if (!terminal || !line || !length) { errno = EINVAL; return -1; }
    int edited = 0;
    for (;;) {
        if (!t4_terminal_pending(terminal)) {
            struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&descriptor, 1, 0);
            if (ready < 0) return errno == EINTR ? 0 : -1;
            if (!ready) return edited ? draw(terminal) : 0;
            ssize_t count = read(STDIN_FILENO, terminal->input, sizeof(terminal->input));
            if (count < 0) return errno == EINTR || errno == EAGAIN ? 0 : -1;
            if (!count) return descriptor.revents & POLLHUP ? 2 : (edited ? draw(terminal) : 0);
            terminal->input_at = 0; terminal->input_length = (size_t)count;
        }
        int action = key(terminal, terminal->input[terminal->input_at++]);
        edited = 1;
        if (action == 2) return 2;
        if (action == 1) {
            int invalid = terminal->invalid;
            *length = (uint32_t)terminal->length;
            memcpy(line, terminal->draft, terminal->length + 1);
            if (!invalid) (void)t4_terminal_history(terminal, (const char *)terminal->draft);
            t4_terminal_clear_input(terminal);
            return invalid ? -2 : 1;
        }
    }
}

int t4_terminal_suspend(struct t4_terminal *terminal)
{
    if (!terminal || terminal->suspended) return 0;
    int result = tcsetattr(STDIN_FILENO, TCSANOW, &terminal->original);
    if (t4_screen_control(terminal->screen, "\033[0m\033[?25h\033[?1000l\033[?1006l\033[?1007l\033[?1000;1006;1007r\033[?2004l\033[r\033[?1049l") < 0) result = -1;
    terminal->suspended = 1;
    return result;
}

int t4_terminal_resume(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (!terminal->suspended) return t4_terminal_resize(terminal);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal->editing) < 0 || geometry(terminal) < 0) return -1;
    terminal->suspended = 0;
    return layout(terminal);
}

void t4_terminal_close(struct t4_terminal *terminal)
{
    if (!terminal) return;
    (void)t4_terminal_suspend(terminal);
    for (unsigned at = 0; at < terminal->history_count; ++at) free(terminal->history[at]);
    t4_view_close(terminal->view_state);
    t4_screen_close(terminal->screen);
    free(terminal);
}
```

### 附录 20：src/client/view.c

```c
#define _XOPEN_SOURCE 700

#include "task4/view.h"
#include "task4/text.h"
#include "task4/screen.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define TEXT_BUDGET (1024U * 1024U)
#define ITEM_BUDGET 128U

struct text {
    unsigned char *bytes;
    size_t length, capacity;
    unsigned char pending[4];
    unsigned pending_length, pending_width;
};
struct item {
    struct item *previous, *next;
    int turn, trimmed;
    uint32_t request, resumed;
    struct text prompt, thinking, answer;
    char phase[40], model[96], summary[192];
    uint32_t prompt_tokens, context_tokens, generated_tokens;
    double rate, elapsed;
};
struct t4_view {
    struct item *first, *last, *active;
    size_t bytes, items, total_lines, top_line;
    unsigned rows, columns, height;
    int dirty, following, dark, thinking_visible;
    char session[96], model[96];
};

const char *t4_view_style(const struct t4_view *view, enum t4_view_style style)
{
    static const char *const light[] = {
        "\033[0;38;2;0;0;0;48;2;248;249;251m",
        "\033[0;38;2;102;109;121;48;2;248;249;251m",
        "\033[1;38;2;49;66;96;48;2;248;249;251m",
        "\033[0;38;2;199;204;214;48;2;248;249;251m"
    };
    static const char *const dark[] = {
        "\033[0;38;2;227;230;237;48;2;22;25;31m",
        "\033[0;38;2;151;159;174;48;2;22;25;31m",
        "\033[1;38;2;162;189;235;48;2;22;25;31m",
        "\033[0;38;2;66;73;86;48;2;22;25;31m"
    };
    return view->dark ? dark[style] : light[style];
}

static size_t character(const unsigned char *bytes, size_t at, size_t length, int *columns)
{
    unsigned width = t4_character_width(bytes[at]);
    if (!width || width > length - at || !t4_character_valid(bytes + at, width)) {
        *columns = 1; return 1;
    }
    mbstate_t state = {0};
    wchar_t decoded;
    size_t result = mbrtowc(&decoded, (const char *)bytes + at, width, &state);
    *columns = result == (size_t)-1 || result == (size_t)-2 ? 1 : wcwidth(decoded);
    if (*columns < 0) *columns = 1;
    return width;
}

static int reserve(struct text *text, size_t extra)
{
    if (extra > SIZE_MAX - text->length - 1) { errno = EOVERFLOW; return -1; }
    size_t needed = text->length + extra + 1;
    if (needed <= text->capacity) return 0;
    size_t capacity = text->capacity ? text->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    void *replacement = realloc(text->bytes, capacity);
    if (!replacement) return -1;
    text->bytes = replacement; text->capacity = capacity;
    return 0;
}

/* Transcript text cannot move the cursor or inject ANSI/OSC commands. */
static int append(struct t4_view *view, struct text *text, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    if (!data && length) { errno = EINVAL; return -1; }
    for (size_t at = 0; at < length;) {
        const unsigned char *piece;
        unsigned width;
        if (!text->pending_length) {
            width = t4_character_width(bytes[at]);
            if (width <= 1) { piece = bytes + at++; width = 1; }
            else {
                text->pending_width = width;
                text->pending[0] = bytes[at++]; text->pending_length = 1;
                continue;
            }
        } else {
            if ((bytes[at] & 0xc0U) != 0x80U) {
                piece = (const unsigned char *)"?"; width = 1;
                text->pending_length = 0;
            } else {
                text->pending[text->pending_length++] = bytes[at++];
                if (text->pending_length != text->pending_width) continue;
                piece = text->pending; width = text->pending_width;
                text->pending_length = 0;
            }
        }
        unsigned value = piece[0];
        if (width == 2 && piece[0] == 0xc2) value = piece[1];
        if ((width == 1 && value < 32 && value != '\n' && value != '\r' && value != '\t')
            || (width == 1 && value == 127) || (width == 2 && piece[0] == 0xc2 && value >= 128 && value <= 159)) {
            continue;
        }
        if (reserve(text, width) < 0) return -1;
        if (t4_character_valid(piece, width)) {
            memcpy(text->bytes + text->length, piece, width); text->length += width;
            if (width == 1 && value == '\t') text->bytes[text->length - 1] = ' ';
            view->bytes += width;
        } else { text->bytes[text->length++] = '?'; ++view->bytes; }
        text->bytes[text->length] = 0;
    }
    view->dirty = 1;
    return 0;
}

static void label(char *target, size_t capacity, const char *source)
{
    size_t used = 0, length = source ? strlen(source) : 0;
    for (size_t at = 0; at < length;) {
        int columns;
        size_t width = character((const unsigned char *)source, at, length, &columns);
        (void)columns;
        unsigned char first = (unsigned char)source[at];
        if (used + width >= capacity) break;
        if (first >= 32 && first != 127 && t4_character_valid((const unsigned char *)source + at, width)
            && !(width == 2 && first == 0xc2 && (unsigned char)source[at + 1] >= 128 && (unsigned char)source[at + 1] <= 159)) {
            memcpy(target + used, source + at, width); used += width;
        } else if (first == '\n' || first == '\r' || first == '\t') target[used++] = ' ';
        at += width;
    }
    target[used] = 0;
}

static void destroy_item(struct t4_view *view, struct item *item)
{
    if (item->previous) item->previous->next = item->next; else view->first = item->next;
    if (item->next) item->next->previous = item->previous; else view->last = item->previous;
    view->bytes -= item->prompt.length + item->thinking.length + item->answer.length;
    free(item->prompt.bytes); free(item->thinking.bytes); free(item->answer.bytes); free(item);
    --view->items;
}

static void trim(struct t4_view *view)
{
    while (view->bytes > TEXT_BUDGET || view->items > ITEM_BUDGET) {
        struct item *old = view->first;
        while (old && old == view->active) old = old->next;
        if (old) { destroy_item(view, old); view->top_line = 0; continue; }
        struct item *item = view->active;
        if (!item) break;
        struct text *text = item->thinking.length > item->answer.length ? &item->thinking : &item->answer;
        if (!text->length) break;
        size_t drop = view->bytes - TEXT_BUDGET;
        if (drop < text->length / 4) drop = text->length / 4;
        if (drop > text->length) drop = text->length;
        while (drop < text->length && (text->bytes[drop] & 0xc0U) == 0x80U) ++drop;
        memmove(text->bytes, text->bytes + drop, text->length - drop + 1);
        text->length -= drop; view->bytes -= drop; item->trimmed = 1;
    }
}

static struct item *new_item(struct t4_view *view, int turn)
{
    struct item *item = calloc(1, sizeof(*item));
    if (!item) return NULL;
    item->turn = turn; item->previous = view->last;
    if (view->last) view->last->next = item; else view->first = item;
    view->last = item; ++view->items; view->dirty = 1;
    return item;
}

struct t4_view *t4_view_open(void)
{
    struct t4_view *view = calloc(1, sizeof(*view));
    if (view) { view->following = 1; view->thinking_visible = 1; view->dirty = 1; }
    return view;
}

void t4_view_close(struct t4_view *view)
{
    if (!view) return;
    while (view->first) destroy_item(view, view->first);
    free(view);
}

void t4_view_reset(struct t4_view *view)
{
    while (view->first) destroy_item(view, view->first);
    view->active = NULL; view->top_line = view->total_lines = 0;
    view->session[0] = view->model[0] = 0;
    view->following = 1; view->dirty = 1;
}

void t4_view_resize(struct t4_view *view, unsigned rows, unsigned columns)
{
    view->rows = rows; view->columns = columns; view->dirty = 1;
}
void t4_view_dirty(struct t4_view *view) { view->dirty = 1; }
int t4_view_is_dirty(const struct t4_view *view) { return view->dirty; }

int t4_view_notice(struct t4_view *view, const void *data, size_t length)
{
    if (!length) return 0;
    struct item *item = view->last;
    if (!item || item->turn) item = new_item(view, 0);
    if (!item || append(view, &item->prompt, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_session(struct t4_view *view, const char *session, const char *model)
{
    label(view->session, sizeof(view->session), session);
    if (model && *model) label(view->model, sizeof(view->model), model);
    view->dirty = 1; return 0;
}

int t4_view_begin(struct t4_view *view, uint32_t request, const char *prompt, uint32_t offset)
{
    struct item *item = new_item(view, 1);
    if (!item) return -1;
    item->request = request; item->resumed = offset;
    label(item->phase, sizeof(item->phase), offset ? "resuming" : "processing");
    label(item->model, sizeof(item->model), view->model);
    view->active = item; view->following = 1;
    if (append(view, &item->prompt, prompt, prompt ? strlen(prompt) : 0) < 0) return -1;
    trim(view); return 0;
}

int t4_view_thinking(struct t4_view *view, const void *data, size_t length)
{
    if (!view->active) return t4_view_notice(view, data, length);
    label(view->active->phase, sizeof(view->active->phase), "thinking");
    if (append(view, &view->active->thinking, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_answer(struct t4_view *view, const void *data, size_t length)
{
    if (!view->active) return t4_view_notice(view, data, length);
    label(view->active->phase, sizeof(view->active->phase), "answering");
    if (append(view, &view->active->answer, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_status(struct t4_view *view, const struct t4_ui_status *status)
{
    if (!status) { errno = EINVAL; return -1; }
    if (status->model && *status->model) label(view->model, sizeof(view->model), status->model);
    struct item *item = view->active;
    if (item) {
        if (status->phase && *status->phase) label(item->phase, sizeof(item->phase), status->phase);
        if (status->model && *status->model) label(item->model, sizeof(item->model), status->model);
        item->prompt_tokens = status->prompt_tokens; item->context_tokens = status->context_tokens;
        item->generated_tokens = status->generated_tokens;
        item->rate = isfinite(status->tokens_per_second) && status->tokens_per_second >= 0 ? status->tokens_per_second : 0;
        item->elapsed = isfinite(status->elapsed_seconds) && status->elapsed_seconds >= 0 ? status->elapsed_seconds : 0;
    }
    view->dirty = 1; return 0;
}

int t4_view_end(struct t4_view *view, const char *state, const char *summary)
{
    if (view->active) {
        label(view->active->phase, sizeof(view->active->phase), state);
        label(view->active->summary, sizeof(view->active->summary), summary);
    }
    view->dirty = 1; return 0;
}

void t4_view_theme(struct t4_view *view, int dark) { view->dark = dark != 0; view->dirty = 1; }
void t4_view_thinking_visible(struct t4_view *view, int show) { view->thinking_visible = show != 0; view->dirty = 1; }

void t4_view_page(struct t4_view *view, int backward)
{
    size_t step = view->height > 2 ? view->height - 2 : 1;
    t4_view_scroll(view, backward, step);
}

void t4_view_scroll(struct t4_view *view, int backward, size_t step)
{
    size_t last = view->total_lines > view->height ? view->total_lines - view->height : 0;
    if (view->following) view->top_line = last;
    if (backward) {
        view->top_line = view->top_line > step ? view->top_line - step : 0;
        view->following = 0;
    } else {
        view->top_line = view->top_line + step < last ? view->top_line + step : last;
        view->following = view->top_line == last;
    }
    view->dirty = 1;
}

struct layout { struct t4_view *view; struct t4_screen *screen; size_t line, start, end; unsigned top, width; int paint, error; };

static void row_at(struct layout *layout, const unsigned char *text, size_t length,
                   enum t4_view_style style, unsigned indent)
{
    size_t number = layout->line++;
    if (!layout->paint || number < layout->start || number >= layout->end || layout->error) return;
    if (t4_screen_row(layout->screen, layout->top + (unsigned)(number - layout->start),
                      t4_view_style(layout->view, style)) < 0) { layout->error = 1; return; }
    for (unsigned at = 0; at < indent; ++at)
        if (t4_screen_append(layout->screen, " ", 1) < 0) { layout->error = 1; return; }
    if (length && t4_screen_append(layout->screen, text, length) < 0) layout->error = 1;
}

static void wrapped(struct layout *layout, const unsigned char *text, size_t length,
                    enum t4_view_style style, unsigned indent)
{
    size_t start = 0, at = 0;
    unsigned used = 0, available = layout->width > indent + 1 ? layout->width - indent - 1 : 1;
    while (at < length) {
        if (text[at] == '\n' || text[at] == '\r') {
            row_at(layout, text + start, at - start, style, indent);
            if (text[at++] == '\r' && at < length && text[at] == '\n') ++at;
            start = at; used = 0; continue;
        }
        int columns;
        size_t width = character(text, at, length, &columns);
        if (text[at] == '\t') columns = 1;
        if (used + (unsigned)columns > available && at > start) {
            row_at(layout, text + start, at - start, style, indent);
            start = at; used = 0; continue;
        }
        used += (unsigned)columns; at += width;
    }
    if (start < length || !length) row_at(layout, text ? text + start : NULL, length - start, style, indent);
}

static void line_text(struct layout *layout, const char *text, enum t4_view_style style)
{
    wrapped(layout, (const unsigned char *)text, strlen(text), style, 1);
}

static void transcript(struct layout *layout)
{
    struct t4_view *view = layout->view;
    char text[512];
    for (struct item *item = view->first; item; item = item->next) {
        if (!item->turn) { wrapped(layout, item->prompt.bytes, item->prompt.length, T4_VIEW_MUTED, 1); continue; }
        (void)snprintf(text, sizeof(text), "YOU  /  %u%s", item->request, item->resumed ? "  (resumed)" : "");
        line_text(layout, text, T4_VIEW_ACCENT);
        wrapped(layout, item->prompt.bytes, item->prompt.length, T4_VIEW_TEXT, 2);
        line_text(layout, "", T4_VIEW_TEXT);
        line_text(layout, item->model[0] ? item->model : "ASSISTANT", T4_VIEW_ACCENT);
        if (item->trimmed) line_text(layout, "[earlier display text trimmed; durable checkpoint is complete]", T4_VIEW_MUTED);
        if (item->thinking.length && view->thinking_visible) {
            line_text(layout, "Thinking", T4_VIEW_MUTED);
            wrapped(layout, item->thinking.bytes, item->thinking.length, T4_VIEW_MUTED, 2);
            if (item->answer.length) line_text(layout, "", T4_VIEW_TEXT);
        } else if (item->thinking.length) line_text(layout, "[thinking hidden  /  /thinking to show]", T4_VIEW_MUTED);
        size_t first = 0;
        while (first < item->answer.length && (item->answer.bytes[first] == '\n' || item->answer.bytes[first] == '\r')) ++first;
        if (first < item->answer.length)
            wrapped(layout, item->answer.bytes + first, item->answer.length - first, T4_VIEW_TEXT, 2);
        (void)snprintf(text, sizeof(text), "%s  /  %u tok  /  %.1f tok/s  /  %.1fs  /  ctx %u/%u%s%s",
                       item->phase, item->generated_tokens, item->rate, item->elapsed,
                       item->prompt_tokens, item->context_tokens,
                       item->summary[0] ? "  /  " : "", item->summary);
        line_text(layout, text, T4_VIEW_MUTED);
        line_text(layout, "", T4_VIEW_TEXT);
    }
}

static int fixed_row(struct t4_view *view, struct t4_screen *screen, unsigned row, const char *text, enum t4_view_style style)
{
    if (t4_screen_row(screen, row, t4_view_style(view, style)) < 0) return -1;
    size_t length = strlen(text), at = 0;
    unsigned used = 0;
    while (at < length) {
        int columns;
        size_t width = character((const unsigned char *)text, at, length, &columns);
        if (used + (unsigned)columns >= view->columns) break;
        used += (unsigned)columns; at += width;
    }
    return t4_screen_append(screen, text, at);
}

int t4_view_render(struct t4_view *view, struct t4_screen *screen)
{
    if (view->rows < 4 || view->columns < 12) {
        for (unsigned row = 1; row <= view->rows; ++row)
            if (fixed_row(view, screen, row, "", T4_VIEW_TEXT) < 0) return -1;
        int result = fixed_row(view, screen, 1, "Enlarge window to edit", T4_VIEW_MUTED);
        if (result == 0) view->dirty = 0;
        return result;
    }
    int compact = view->columns < 52 || view->rows < 12;
    unsigned header = compact ? 1 : 2;
    view->height = view->rows - header - (compact ? 2U : 3U);
    struct layout layout = {.view = view, .screen = screen, .top = header + 1, .width = view->columns};
    transcript(&layout); view->total_lines = layout.line;
    size_t last = layout.line > view->height ? layout.line - view->height : 0;
    if (view->following) view->top_line = last;
    if (view->top_line > last) view->top_line = last;
    char text[512];
    (void)snprintf(text, sizeof(text), " TASK4  /  %s", view->model[0] ? view->model : "local conversation");
    if (fixed_row(view, screen, 1, text, T4_VIEW_ACCENT) < 0) return -1;
    if (!compact) {
        struct item *item = view->active;
        (void)snprintf(text, sizeof(text), " session %.12s  /  context %u / %u  /  %s",
                       view->session[0] ? view->session : "connecting", item ? item->prompt_tokens : 0,
                       item ? item->context_tokens : 0, view->following ? "live" : "scrollback - PgDn returns to live");
        if (fixed_row(view, screen, 2, text, T4_VIEW_MUTED) < 0) return -1;
    }
    layout.line = 0; layout.paint = 1; layout.start = view->top_line; layout.end = layout.start + view->height;
    transcript(&layout);
    if (layout.error) return -1;
    size_t shown = layout.line > layout.start ? layout.line - layout.start : 0;
    if (shown > view->height) shown = view->height;
    for (unsigned at = (unsigned)shown; at < view->height; ++at)
        if (fixed_row(view, screen, header + 1 + at, "", T4_VIEW_TEXT) < 0) return -1;
    if (!compact) {
        size_t count = view->columns - 1;
        char *rule = malloc(count + 1);
        if (!rule) return -1;
        memset(rule, '-', count); rule[count] = 0;
        int result = fixed_row(view, screen, view->rows - 2, rule, T4_VIEW_BORDER);
        free(rule); if (result < 0) return -1;
    }
    struct item *item = view->active;
    int idle = !item || !strcmp(item->phase, "completed") || !strcmp(item->phase, "cancelled") || !strcmp(item->phase, "error");
    (void)snprintf(text, sizeof(text), " %s%s  |  Enter send  Ctrl+C %s  Wheel/PgUp scroll  /help",
                   view->following ? "" : "SCROLLBACK  /  ", item && item->phase[0] ? item->phase : "ready",
                   idle ? "clear" : "stop");
    if (fixed_row(view, screen, view->rows - 1, text, T4_VIEW_MUTED) < 0) return -1;
    view->dirty = 0;
    return 0;
}
```

### 附录 21：src/net/discovery.c

```c
#include "task4/discovery.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_LIMIT (PATH_MAX + 160U)
#define ENDPOINT_FILE "server.endpoint"
#define LEASE_FILE "server.lock"

struct record { struct t4_endpoint endpoint; uint64_t device, inode; };

/* Newly created components are private; openat refuses symlink components. */
static int directory_open(const char *path, int create)
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char copy[PATH_MAX];
    strcpy(copy, path);
    int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (create && mkdirat(fd, part, 0700) == 0) {
            if (fsync(fd) < 0) goto fail;
        } else if (create && errno != EEXIST) goto fail;
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) goto fail;
        close(fd); fd = child;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) goto fail;
    if (st.st_uid != geteuid() || (st.st_mode & 0022)) { errno = EACCES; goto fail; }
    return fd;
fail:
    {
        int saved = errno;
        close(fd); errno = saved; return -1;
    }
}

static int regular(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) {
        errno = EINVAL; return -1;
    }
    return 0;
}

/* O_NONBLOCK prevents a malicious FIFO from hanging before fstat rejects it. */
static int read_fd(int fd, char *buffer, size_t capacity)
{
    if (regular(fd) < 0 || lseek(fd, 0, SEEK_SET) < 0) return -1;
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) break;
        used += (size_t)count;
    }
    char extra;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count < 0) return -1;
    if (count || memchr(buffer, 0, used)) { errno = EINVAL; return -1; }
    buffer[used] = 0; return 0;
}

static int read_at(int directory, const char *name, char *buffer, size_t capacity)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = read_fd(fd, buffer, capacity), saved = errno;
    close(fd); errno = saved; return result;
}

static int hex(const char *text, size_t width)
{
    if (!text || strlen(text) != width) return 0;
    for (size_t at = 0; at < width; ++at)
        if (!((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f'))) return 0;
    return 1;
}

static int lines(char *buffer, char **fields, unsigned count)
{
    char *at = buffer;
    for (unsigned n = 0; n < count; ++n) {
        fields[n] = at;
        char *end = strchr(at, '\n');
        if (!end) { errno = EINVAL; return -1; }
        *end = 0; at = end + 1;
    }
    if (*at) { errno = EINVAL; return -1; }
    return 0;
}

static int read_instance(int fd, char instance[33])
{
    char buffer[64], *fields[2];
    if (read_fd(fd, buffer, sizeof(buffer)) < 0 || lines(buffer, fields, 2) < 0) return -1;
    if (strcmp(fields[0], "T4LEASE1") || !hex(fields[1], 32)) { errno = EINVAL; return -1; }
    memcpy(instance, fields[1], 33); return 0;
}

static int identity(char *text, uint64_t *device, uint64_t *inode)
{
    char *space = strchr(text, ' ');
    if (!space || strchr(space + 1, ' ')) { errno = EINVAL; return -1; }
    *space = 0;
    char *parts[2] = {text, space + 1};
    uint64_t *out[2] = {device, inode};
    for (unsigned n = 0; n < 2; ++n) {
        size_t length = strlen(parts[n]);
        if (!length || length > 16 || !hex(parts[n], length)) { errno = EINVAL; return -1; }
        char *end;
        errno = 0;
        uintmax_t value = strtoumax(parts[n], &end, 16);
        if (errno || *end || value > UINT64_MAX) { errno = EINVAL; return -1; }
        *out[n] = (uint64_t)value;
    }
    return 0;
}

static int read_record(int directory, const char *name, struct record *record)
{
    char buffer[RECORD_LIMIT], *fields[6];
    if (read_at(directory, name, buffer, sizeof(buffer)) < 0 || lines(buffer, fields, 6) < 0) return -1;
    struct in_addr ipv4;
    if (strcmp(fields[0], "T4ENDPOINT1") || inet_pton(AF_INET, fields[1], &ipv4) != 1 ||
        strlen(fields[1]) >= sizeof(record->endpoint.host) || fields[3][0] != '/' ||
        strlen(fields[3]) >= sizeof(record->endpoint.state) || strchr(fields[3], '\r') ||
        !hex(fields[5], 32) || identity(fields[4], &record->device, &record->inode) < 0) {
        errno = EINVAL; return -1;
    }
    uint32_t port = 0;
    for (const char *at = fields[2]; *at; ++at) {
        if (*at < '0' || *at > '9' || port > 6553) { errno = EINVAL; return -1; }
        port = port * 10 + (unsigned)(*at - '0');
    }
    if (!port || port > 65535) { errno = EINVAL; return -1; }
    strcpy(record->endpoint.host, fields[1]); record->endpoint.port = (uint16_t)port;
    strcpy(record->endpoint.state, fields[3]); memcpy(record->endpoint.instance, fields[5], 33);
    return 0;
}

static int equal(const struct record *left, const struct record *right)
{
    return left->device == right->device && left->inode == right->inode &&
        left->endpoint.port == right->endpoint.port && !strcmp(left->endpoint.host, right->endpoint.host) &&
        !strcmp(left->endpoint.state, right->endpoint.state) && !strcmp(left->endpoint.instance, right->endpoint.instance);
}

static int held(int lease)
{
    if (regular(lease) < 0) return -1;
    if (flock(lease, LOCK_EX | LOCK_NB) == 0) {
        (void)flock(lease, LOCK_UN); errno = ENOENT; return -1;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
    return -1;
}

static int current(int directory, int lease, const struct record *expected, struct record *out)
{
    char first[33], last[33];
    struct stat st;
    if (held(lease) < 0 || read_instance(lease, first) < 0 || fstat(directory, &st) < 0 ||
        read_record(directory, ENDPOINT_FILE, out) < 0) return -1;
    if (out->device != (uint64_t)st.st_dev || out->inode != (uint64_t)st.st_ino ||
        strcmp(first, out->endpoint.instance) || (expected && !equal(expected, out))) {
        errno = ENOENT; return -1;
    }
    /* A master may die and restart while files are read. Both lease checks and
     * nonce snapshots must still describe the same running instance. */
    if (held(lease) < 0 || read_instance(lease, last) < 0) return -1;
    if (strcmp(first, last)) { errno = ENOENT; return -1; }
    return 0;
}

static int live(const struct record *expected, const char *state, struct record *out, const char *token)
{
    int directory = directory_open(state, 0);
    if (directory < 0) return -1;
    int lease = openat(directory, LEASE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int result = -1;
    if (lease >= 0 && current(directory, lease, expected, out) == 0) {
        result = 0;
        char *absolute = realpath(state, NULL);
        if (!absolute) result = -1;
        else {
            if (strcmp(absolute, out->endpoint.state)) { errno = EINVAL; result = -1; }
            free(absolute);
        }
        if (!result && token && *token) {
            int session = openat(directory, token, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat st;
            if (session < 0) result = -1;
            else {
                if (fstat(session, &st) < 0 || st.st_uid != geteuid()) { errno = EACCES; result = -1; }
                close(session);
            }
        }
    }
    int saved = errno;
    if (lease >= 0) close(lease);
    close(directory); errno = saved; return result;
}

static int write_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t count = write(fd, text, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        text += count; length -= (size_t)count;
    }
    return 0;
}

static int atomic_file(int directory, const char *name, const char *text, const char *instance)
{
    char temporary[96];
    int size = snprintf(temporary, sizeof(temporary), ".endpoint-%ld-%s", (long)getpid(), instance);
    if (size < 0 || (size_t)size >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = write_all(fd, text, strlen(text));
    if (!result) result = fsync(fd);
    int saved = errno;
    if (close(fd) < 0 && !result) { result = -1; saved = errno; }
    if (!result && renameat(directory, temporary, directory, name) < 0) { result = -1; saved = errno; }
    if (!result && fsync(directory) < 0) { result = -1; saved = errno; }
    if (result) (void)unlinkat(directory, temporary, 0);
    errno = saved; return result;
}

void t4_service_child_close(struct t4_service *service)
{
    if (!service) return;
    if (service->lease_fd >= 0) close(service->lease_fd);
    if (service->state_fd >= 0) close(service->state_fd);
    if (service->registry_fd >= 0) close(service->registry_fd);
    service->lease_fd = service->state_fd = service->registry_fd = -1;
}

int t4_service_acquire(const char *state, struct t4_service *service, struct t4_endpoint *existing)
{
    if (!service || !existing) { errno = EINVAL; return -1; }
    memset(service, 0, sizeof(*service));
    service->lease_fd = service->state_fd = service->registry_fd = -1;
    service->state_fd = directory_open(state, 1);
    if (service->state_fd < 0) return -1;
    char *absolute = realpath(state, NULL);
    if (!absolute) goto fail;
    if (strlen(absolute) >= sizeof(service->state) || strchr(absolute, '\n') || strchr(absolute, '\r')) {
        free(absolute); errno = EINVAL; goto fail;
    }
    strcpy(service->state, absolute); free(absolute);
    struct stat st;
    if (fstat(service->state_fd, &st) < 0) goto fail;
    int canonical = directory_open(service->state, 0);
    if (canonical < 0) goto fail;
    struct stat checked;
    int verified = fstat(canonical, &checked), saved = errno;
    close(canonical);
    if (verified < 0) { errno = saved; goto fail; }
    if (checked.st_dev != st.st_dev || checked.st_ino != st.st_ino) { errno = EINVAL; goto fail; }
    service->device = (uint64_t)st.st_dev; service->inode = (uint64_t)st.st_ino;
    service->lease_fd = openat(service->state_fd, LEASE_FILE,
        O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (service->lease_fd < 0 || regular(service->lease_fd) < 0) goto fail;
    if (flock(service->lease_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) goto fail;
        struct record record;
        if (current(service->state_fd, service->lease_fd, NULL, &record) < 0 ||
            strcmp(record.endpoint.state, service->state)) { errno = EBUSY; goto fail; }
        *existing = record.endpoint;
        t4_service_child_close(service); return 1;
    }
    unsigned char random[16];
    size_t used = 0;
    while (used < sizeof(random)) {
        ssize_t count = getrandom(random + used, sizeof(random) - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; goto fail; }
        used += (size_t)count;
    }
    for (size_t at = 0; at < sizeof(random); ++at)
        (void)snprintf(service->instance + 2 * at, 3, "%02x", random[at]);
    char metadata[64];
    (void)snprintf(metadata, sizeof(metadata), "T4LEASE1\n%s\n", service->instance);
    if (ftruncate(service->lease_fd, 0) < 0 || lseek(service->lease_fd, 0, SEEK_SET) < 0 ||
        write_all(service->lease_fd, metadata, strlen(metadata)) < 0 || fsync(service->lease_fd) < 0) goto fail;
    if (unlinkat(service->state_fd, ENDPOINT_FILE, 0) < 0 && errno != ENOENT) goto fail;
    if (fsync(service->state_fd) < 0) goto fail;
    return 0;
fail:
    {
        int saved = errno;
        t4_service_child_close(service); errno = saved; return -1;
    }
}

int t4_service_publish(struct t4_service *service, const char *registry, const char *host, uint16_t port)
{
    if (!service || service->lease_fd < 0 || service->state_fd < 0 || !port || !host) { errno = EINVAL; return -1; }
    struct in_addr ipv4;
    if (inet_pton(AF_INET, host, &ipv4) != 1) { errno = EINVAL; return -1; }
    const char *advertised = !strcmp(host, "0.0.0.0") ? "127.0.0.1" : host;
    char record[RECORD_LIMIT];
    int count = snprintf(record, sizeof(record), "T4ENDPOINT1\n%s\n%u\n%s\n%" PRIx64 " %" PRIx64 "\n%s\n",
        advertised, port, service->state, service->device, service->inode, service->instance);
    if (count < 0 || (size_t)count >= sizeof(record)) { errno = ENAMETOOLONG; return -1; }
    int directory = directory_open(registry, 1);
    if (directory < 0) return -1;
    count = snprintf(service->entry, sizeof(service->entry), "%" PRIx64 "-%" PRIx64 ".endpoint",
                     service->device, service->inode);
    if (count < 0 || (size_t)count >= sizeof(service->entry)) { close(directory); errno = ENAMETOOLONG; return -1; }
    if (service->registry_fd >= 0) close(service->registry_fd);
    service->registry_fd = directory;
    if (atomic_file(service->state_fd, ENDPOINT_FILE, record, service->instance) < 0) return -1;
    service->published = 1;
    return atomic_file(directory, service->entry, record, service->instance);
}

static void remove_owned(int directory, const char *name, const struct t4_service *service)
{
    if (directory < 0 || !*name) return;
    struct record record;
    if (read_record(directory, name, &record) == 0 && record.device == service->device &&
        record.inode == service->inode && !strcmp(record.endpoint.state, service->state) &&
        !strcmp(record.endpoint.instance, service->instance)) {
        if (unlinkat(directory, name, 0) == 0) (void)fsync(directory);
    }
}

void t4_service_close(struct t4_service *service)
{
    if (!service) return;
    if (service->published && service->lease_fd >= 0) {
        remove_owned(service->registry_fd, service->entry, service);
        remove_owned(service->state_fd, ENDPOINT_FILE, service);
    }
    t4_service_child_close(service); service->published = 0;
}

int t4_service_discover(const char *registry, const char *explicit_state,
                        const char *token, struct t4_endpoint *endpoint)
{
    if (!endpoint || (token && *token && !hex(token, 32))) { errno = EINVAL; return -1; }
    struct record record;
    if (explicit_state && *explicit_state) {
        if (live(NULL, explicit_state, &record, token) < 0) return -1;
        *endpoint = record.endpoint; return 0;
    }
    int fd = directory_open(registry, 0);
    if (fd < 0) return -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    unsigned found = 0;
    int result = 0, saved = ENOENT;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) { result = -1; saved = errno; } break; }
        size_t length = strlen(entry->d_name);
        if (length < 10 || length >= 80 || strcmp(entry->d_name + length - 9, ".endpoint")) continue;
        struct record advertised;
        if (read_record(fd, entry->d_name, &advertised) < 0) continue;
        char name[80];
        (void)snprintf(name, sizeof(name), "%" PRIx64 "-%" PRIx64 ".endpoint", advertised.device, advertised.inode);
        if (strcmp(name, entry->d_name) || live(&advertised, advertised.endpoint.state, &record, token) < 0) continue;
        *endpoint = record.endpoint;
        if (++found > 1) { result = -1; saved = EEXIST; break; }
    }
    if (!result && !found) result = -1;
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    if (result) errno = saved;
    return result;
}

int t4_service_validate(const struct t4_endpoint *endpoint)
{
    if (!endpoint || !hex(endpoint->instance, 32)) { errno = EINVAL; return -1; }
    struct record record;
    if (live(NULL, endpoint->state, &record, NULL) < 0) return -1;
    if (record.endpoint.port != endpoint->port || strcmp(record.endpoint.host, endpoint->host) ||
        strcmp(record.endpoint.state, endpoint->state) || strcmp(record.endpoint.instance, endpoint->instance)) {
        errno = ENOENT; return -1;
    }
    return 0;
}
```

### 附录 22：src/net/protocol.c

```c
#define _POSIX_C_SOURCE 200809L

#include "task4/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int deadline_after(int milliseconds, struct timespec *deadline)
{
    if (milliseconds <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) == -1) {
        return -1;
    }
    deadline->tv_sec += milliseconds / 1000;
    deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int wait_ready(int fd, short events, const struct timespec *deadline)
{
    struct timespec now;
    struct pollfd descriptor = { .fd = fd, .events = events, .revents = 0 };
    int64_t remaining;
    int result;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    remaining = (int64_t)(deadline->tv_sec - now.tv_sec) * INT64_C(1000000000)
              + deadline->tv_nsec - now.tv_nsec;
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    /* Round upward: a sub-millisecond remainder must still wait for data. */
    remaining = (remaining + 999999) / 1000000;
    result = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
    if (result == -1) {
        return -1;
    }
    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
        errno = EBADF;
        return -1;
    }
    /* recv/send and SO_ERROR report the precise hangup or socket error. */
    return 0;
}

static int transfer(int fd, void *buffer, size_t length, int writing,
                    const struct timespec *deadline, int boundary_eof)
{
    unsigned char *bytes = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count;

        if (wait_ready(fd, writing ? POLLOUT : POLLIN, deadline) == -1) {
            return -1;
        }
        count = writing ? send(fd, bytes + done, length - done,
                               MSG_NOSIGNAL | MSG_DONTWAIT)
                        : recv(fd, bytes + done, length - done, MSG_DONTWAIT);
        if (count > 0) {
            done += (size_t)count;
        } else if (count == 0) {
            if (!writing && done == 0 && boundary_eof) {
                return 0;
            }
            errno = writing ? EPIPE : ECONNRESET;
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    return 1;
}

int t4_send(int fd, const struct t4_frame *frame, int idle_ms)
{
    uint32_t header[5];
    struct timespec deadline;

    if (frame == NULL || frame->length > T4_MAX_FRAME) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    header[0] = htonl(T4_MAGIC);
    header[1] = htonl(frame->type);
    header[2] = htonl(frame->request_id);
    header[3] = htonl(frame->offset);
    header[4] = htonl(frame->length);
    if (transfer(fd, header, sizeof(header), 1, &deadline, 0) == -1) {
        return -1;
    }
    if (transfer(fd, (void *)frame->data, frame->length, 1, &deadline, 0) == -1) {
        return -1;
    }
    return 1;
}

int t4_recv(int fd, struct t4_frame *frame, int idle_ms)
{
    uint32_t header[5];
    struct timespec deadline;
    int result;

    if (frame == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    result = transfer(fd, header, sizeof(header), 0, &deadline, 1);
    if (result != 1) {
        return result;
    }
    if (ntohl(header[0]) != T4_MAGIC || ntohl(header[4]) > T4_MAX_FRAME) {
        errno = EPROTO;
        return -1;
    }
    frame->type = ntohl(header[1]);
    frame->request_id = ntohl(header[2]);
    frame->offset = ntohl(header[3]);
    frame->length = ntohl(header[4]);
    result = transfer(fd, frame->data, frame->length, 0, &deadline, 0);
    if (result == -1) {
        return -1;
    }
    /* Convenience terminator; the framing layer accepts embedded NUL bytes. */
    frame->data[frame->length] = '\0';
    return 1;
}

static int configure_socket(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int enabled = 1;

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1
        || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
        || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                      sizeof(enabled)) == -1) {
        return -1;
    }
    return 0;
}

int t4_connect(const char *host, uint16_t port, int idle_ms)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct timespec deadline;
    char service[6];
    int connected = -1;
    int saved_error = ECONNREFUSED;
    int lookup;

    if (host == NULL || *host == '\0' || port == 0) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    lookup = getaddrinfo(host, service, &hints, &addresses);
    if (lookup != 0) {
        if (lookup != EAI_SYSTEM) {
            errno = EHOSTUNREACH;
        }
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);
        int error = 0;
        socklen_t error_length = sizeof(error);

        if (fd == -1) {
            saved_error = errno;
            continue;
        }
        if (configure_socket(fd) == -1) {
            saved_error = errno;
            (void)close(fd);
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        if (errno == EINPROGRESS) {
            if (wait_ready(fd, POLLOUT, &deadline) == 0
                && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error,
                              &error_length) == 0) {
                if (error == 0) {
                    connected = fd;
                    break;
                }
                errno = error;
            }
        }
        saved_error = errno;
        (void)close(fd);
        if (saved_error == EINTR || saved_error == ETIMEDOUT) {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (connected == -1) {
        errno = saved_error;
    }
    return connected;
}

int t4_listen(const char *bind_ipv4, uint16_t port, uint16_t *actual_port)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int enabled = 1;
    int fd;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ipv4 == NULL ? "127.0.0.1" : bind_ipv4,
                  &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (configure_socket(fd) == -1
        || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                      sizeof(enabled)) == -1
        || bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1
        || listen(fd, 64) == -1
        || getsockname(fd, (struct sockaddr *)&address, &address_length) == -1) {
        int saved_error = errno;

        (void)close(fd);
        errno = saved_error;
        return -1;
    }
    if (actual_port != NULL) {
        *actual_port = ntohs(address.sin_port);
    }
    return fd;
}

int t4_uint(const char *text, uint32_t minimum, uint32_t maximum,
            uint32_t *value)
{
    uint32_t parsed = 0;
    const unsigned char *cursor = (const unsigned char *)text;

    if (text == NULL || *text == '\0' || value == NULL || minimum > maximum) {
        errno = EINVAL;
        return -1;
    }
    while (*cursor != '\0') {
        uint32_t digit;

        if (*cursor < '0' || *cursor > '9') {
            errno = EINVAL;
            return -1;
        }
        digit = (uint32_t)(*cursor - '0');
        if (parsed > UINT32_MAX / 10U
            || (parsed == UINT32_MAX / 10U && digit > UINT32_MAX % 10U)) {
            errno = ERANGE;
            return -1;
        }
        parsed = parsed * 10U + digit;
        cursor++;
    }
    if (parsed < minimum || parsed > maximum) {
        errno = ERANGE;
        return -1;
    }
    *value = parsed;
    return 0;
}
```

### 附录 23：src/server/main.c

```c
#include "task4/backend.h"
#include "task4/discovery.h"
#include "task4/protocol.h"
#include "task4/store.h"
#include "task4/text.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_signal(int signo) { stopped = signo; }

struct configuration {
    const char *bind, *state, *adapter, *model_config, *discovery;
    char instance[33];
    uint32_t port, idle, capacity;
    const struct t4_backend *backend;
};

static int signals(void)
{
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGTERM, &action, NULL) < 0 ||
           sigaction(SIGINT, &action, NULL) < 0 ? -1 : 0;
}

static int send_event(int fd, uint32_t type, uint32_t request, uint32_t offset,
                      const void *data, uint32_t length, int idle)
{
    struct t4_frame frame = {.type = type, .request_id = request,
                             .offset = offset, .length = length};
    if (length) memcpy(frame.data, data, length);
    return t4_send(fd, &frame, idle);
}

static void send_error(int fd, const char *message, int idle)
{
    (void)send_event(fd, T4_ERROR, 0, 0, message, (uint32_t)strlen(message), idle);
}

static int replay(int client, int response, uint32_t request, uint32_t from,
                  uint32_t until, int idle)
{
    if (lseek(response, from, SEEK_SET) < 0) return -1;
    while (from < until && !stopped) {
        unsigned char character[4];
        if (read(response, character, 1) != 1) return -1;
        unsigned width = t4_character_width(character[0]);
        if (!width || width > until - from) { errno = EPROTO; return -1; }
        if (width > 1 && read(response, character + 1, width - 1) != (ssize_t)(width - 1)) return -1;
        if (!t4_character_valid(character, width)) { errno = EPROTO; return -1; }
        if (send_event(client, T4_DELTA, request, from, character, width, idle) < 0) return -1;
        from += width;
    }
    return stopped ? -1 : 0;
}

static int valid_text(const unsigned char *data, uint32_t length)
{
    for (uint32_t i = 0; i < length;) {
        unsigned width = t4_character_width(data[i]);
        if (!width || width > length - i || !t4_character_valid(data + i, width)) return 0;
        i += width;
    }
    return 1;
}

/* 0 completed, 1 cancelled, -1 failure, -2 error already sent by model adapter. */
static int generate(int client, struct t4_session *session, const char *directory,
                    uint32_t request, uint32_t offset, const struct configuration *cfg,
                    uint32_t *finished_offset)
{
    char path[PATH_MAX];
    struct t4_generation generation = {.pid = -1, .fd = -1};
    int response = -1, result = -1, status, exit_fd = -1;
    if (t4_path(path, directory, "response.txt") < 0) return -1;
    response = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (response < 0 || cfg->backend->start(cfg->adapter, directory, session->context, cfg->model_config,
                                              offset, &generation) < 0) goto cleanup;
    /* Linux pidfd is pollable even if a child closes stdout before exiting.
     * A blocking waitpid at output EOF would make CANCEL stop responding. */
    exit_fd = (int)syscall(SYS_pidfd_open, generation.pid, 0);
    if (exit_fd < 0) goto cleanup;
    printf("[worker %ld] session=%s turn=%u generator=%ld resume=%u\n",
           (long)getpid(), session->token, request, (long)generation.pid, offset);
    int model_end = 0, output_closed = 0;
    for (;;) {
        if (output_closed) {
            pid_t waited = waitpid(generation.pid, &status, WNOHANG);
            if (waited == generation.pid) { generation.pid = -1; break; }
            if (waited < 0 && errno != EINTR) goto cleanup;
        }
        struct pollfd descriptors[3] = {
            {generation.fd, POLLIN, 0}, {client, POLLIN, 0}, {exit_fd, POLLIN, 0}};
        int ready = poll(descriptors, 3, 1000);
        if (stopped) goto cleanup;
        if (ready < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (!ready) {
            /* 心跳只表示连接存活，不能推进恢复游标或伪造生成进度。 */
            if (send_event(client, T4_HEARTBEAT, request, offset, NULL, 0, (int)cfg->idle) < 0)
                goto cleanup;
            continue;
        }
        if (descriptors[1].revents) {
            struct t4_frame control;
            if (t4_recv(client, &control, (int)cfg->idle) != 1) goto cleanup;
            if (control.type != T4_CANCEL || control.request_id != request || control.length) {
                errno = EPROTO; goto cleanup;
            }
            /* Stop inference before acknowledging cancellation or accepting another turn. */
            t4_generation_stop(&generation);
            if (t4_turn_cancel(directory) < 0 || t4_event(session, "cancelled", request, offset) < 0 ||
                send_event(client, T4_CANCELLED, request, offset, NULL, 0, (int)cfg->idle) < 0)
                goto cleanup;
            result = 1;
            goto cleanup;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            struct t4_frame event;
            int got = t4_recv(generation.fd, &event, 1500);
            if (!got) {
                close(generation.fd); generation.fd = -1;
                output_closed = 1;
                continue;
            }
            if (got < 0 || event.request_id || model_end) { errno = EPROTO; goto cleanup; }
            if (event.type == T4_END && !event.length && event.offset == offset) {
                model_end = 1;
                continue; /* Require a successful child exit as well as its END event. */
            }
            if (event.type == T4_ERROR) {
                if (valid_text(event.data, event.length))
                    send_error(client, (char *)event.data, (int)cfg->idle);
                result = -2;
                goto cleanup;
            }
            if ((event.type == T4_THINK || event.type == T4_STATUS) &&
                event.length && valid_text(event.data, event.length)) {
                if (send_event(client, event.type, request,
                        event.type == T4_THINK ? event.offset : offset,
                        event.data, event.length, (int)cfg->idle) < 0) goto cleanup;
                continue;
            }
            if (event.type != T4_DELTA || event.offset != offset || event.length < 1 ||
                event.length > 4 || t4_character_width(event.data[0]) != event.length ||
                !t4_character_valid(event.data, event.length) ||
                offset > T4_MAX_RESPONSE - event.length) { errno = EPROTO; goto cleanup; }
            if (t4_write_all(response, event.data, event.length) < 0 || fsync(response) < 0 ||
                send_event(client, T4_DELTA, request, offset, event.data, event.length,
                           (int)cfg->idle) < 0) goto cleanup;
            offset += event.length;
            continue;
        } else if (descriptors[0].revents) { errno = EIO; goto cleanup; }
        else if (!output_closed && descriptors[2].revents) {
            /* A descendant retaining stdout must not outlive its generator. */
            errno = EIO; goto cleanup;
        }
    }
    if (!model_end ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) { errno = EIO; goto cleanup; }
    if (t4_turn_done(directory) < 0 || t4_event(session, "completed", request, offset) < 0 ||
        send_event(client, T4_END, request, offset, NULL, 0, (int)cfg->idle) < 0) goto cleanup;
    result = 0;
cleanup:
    {
        int saved = errno;
        *finished_offset = offset;
        t4_generation_stop(&generation);
        if (exit_fd >= 0) close(exit_fd);
        if (response >= 0) close(response);
        errno = saved;
        return result;
    }
}

static int worker(int client, const struct configuration *cfg)
{
    struct t4_session session = {.lock_fd = -1, .log_fd = -1};
    struct t4_frame frame;
    int result = 1, idle = (int)cfg->idle;
    uint32_t last_request = 0, last_offset = 0, last_terminal = 0;
    if (t4_recv(client, &frame, idle) != 1) goto protocol_error;
    if (frame.type == T4_PROBE) {
        if (frame.request_id || frame.offset || frame.length ||
            send_event(client, T4_SERVICE, 0, 0, cfg->instance, 32, idle) < 0) goto protocol_error;
        /* Discovery does not create a session or acquire its conversation lock. */
        if (t4_recv(client, &frame, idle) != 1) goto cleanup;
    }
    if (frame.type != T4_HELLO ||
        frame.request_id || frame.offset || (frame.length != 0 && frame.length != 32) ||
        memchr(frame.data, 0, frame.length)) goto protocol_error;
    frame.data[frame.length] = 0;
    if (t4_session_open(cfg->state, (char *)frame.data, &session) < 0) {
        send_error(client, "session unavailable (unknown token, busy lock or storage error)", idle);
        goto cleanup;
    }
    if (send_event(client, T4_WELCOME, 0, 0, session.token, 32, idle) < 0) goto cleanup;
    printf("[worker %ld] connected session=%s\n", (long)getpid(), session.token);
    while (!stopped) {
        int received = t4_recv(client, &frame, idle);
        if (!received) { result = 0; break; }
        if (received != 1) goto cleanup;
        /* A CANCEL can cross END in flight. Return the existing terminal state. */
        if (frame.type == T4_CANCEL && last_terminal && frame.request_id == last_request &&
            !frame.length) {
            if (send_event(client, last_terminal, last_request, last_offset, NULL, 0, idle) < 0)
                goto cleanup;
            continue;
        }
        if (frame.type != T4_BEGIN || !frame.request_id || frame.length) goto protocol_error;
        uint32_t request = frame.request_id, resume = frame.offset, length = 0;
        unsigned char prompt[T4_MAX_PROMPT + 1];
        /* 输入可拆为多个 INPUT 帧；只在 COMMIT 后开始生成。 */
        for (;;) {
            if (t4_recv(client, &frame, idle) != 1 || frame.request_id != request) goto protocol_error;
            if (frame.type == T4_COMMIT) {
                if (frame.length || frame.offset != length) goto protocol_error;
                break;
            }
            if (frame.type != T4_INPUT || !frame.length || frame.offset != length ||
                frame.length > T4_MAX_PROMPT - length || memchr(frame.data, 0, frame.length))
                goto protocol_error;
            memcpy(prompt + length, frame.data, frame.length);
            length += frame.length;
        }
        prompt[length] = 0;
        char directory[PATH_MAX], response_path[PATH_MAX];
        uint32_t cached;
        int done;
        if (t4_turn_open(&session, request, prompt, length, directory, &cached, &done,
                         cfg->model_config, cfg->backend) < 0 ||
            resume > cached || t4_path(response_path, directory, "response.txt") < 0) {
            send_error(client, "request rejected (sequence, changed prompt, offset or storage)", idle);
            goto cleanup;
        }
        int response = open(response_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (response < 0) goto cleanup;
        int replayed = replay(client, response, request, resume, cached, idle);
        close(response);
        if (replayed < 0) goto cleanup;
        if (done) {
            last_terminal = done == 2 ? T4_CANCELLED : T4_END;
            if (send_event(client, last_terminal, request, cached, NULL, 0, idle) < 0) goto cleanup;
        } else {
            int generated = generate(client, &session, directory, request, cached, cfg, &cached);
            if (generated < 0) {
                if (!stopped && generated != -2)
                    send_error(client, "generation interrupted; checkpoint retained; see generator.log", idle);
                goto cleanup;
            }
            last_terminal = generated == 1 ? T4_CANCELLED : T4_END;
        }
        last_request = request; last_offset = cached;
    }
    goto cleanup;
protocol_error:
    send_error(client, "invalid protocol frame or input", idle);
cleanup:
    if (session.log_fd >= 0) (void)t4_event(&session, "disconnected", 0, 0);
    t4_session_close(&session);
    close(client);
    return stopped ? 128 + stopped : result;
}

static int configuration(int argc, char **argv, struct configuration *cfg)
{
    *cfg = (struct configuration){.bind = "127.0.0.1", .state = "state/server",
        .adapter = "src/model/adapter.py", .model_config = "config/model.json", .discovery = "state/services", .port = 0,
        .idle = 600000, .capacity = 32, .backend = &t4_model_backend};
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return -1;
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--bind")) cfg->bind = value;
        else if (!strcmp(key, "--state")) cfg->state = value;
        else if (!strcmp(key, "--adapter")) cfg->adapter = value;
        else if (!strcmp(key, "--model-config")) cfg->model_config = value;
        else if (!strcmp(key, "--discovery-dir")) cfg->discovery = value;
        else if (!strcmp(key, "--backend")) {
            if (strcmp(value, "llama")) return -1;
        }
        else if (!strcmp(key, "--port")) { if (t4_uint(value, 0, 65535, &cfg->port) < 0) return -1; }
        else if (!strcmp(key, "--idle-ms")) { if (t4_uint(value, 100, 3600000, &cfg->idle) < 0) return -1; }
        else if (!strcmp(key, "--max-clients")) { if (t4_uint(value, 1, 64, &cfg->capacity) < 0) return -1; }
        else return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct configuration cfg;
    if (configuration(argc, argv, &cfg) < 0) {
        fprintf(stderr, "Usage: %s [--bind IPv4] [--port 0..65535] [--state DIR]\n"
                        "  [--adapter PATH]\n"
                        "  [--backend llama] [--model-config config/model.json]\n"
                        "  [--discovery-dir DIR] [--idle-ms 100..3600000] [--max-clients 1..64]\n"
                        "Default port: auto (kernel-assigned); clients discover via state/services.\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (signals() < 0) { perror("sigaction"); return 1; }
    struct t4_service service;
    struct t4_endpoint existing;
    int acquired = t4_service_acquire(cfg.state, &service, &existing);
    if (acquired < 0) { perror("server state/lease (another server may still be starting)"); return 1; }
    if (acquired == 1) {
        printf("[server] already running on %s:%u for state=%s; reuse with stream_client --server-state '%s'\n",
               existing.host, existing.port, existing.state, existing.state);
        return 0;
    }
    memcpy(cfg.instance, service.instance, sizeof(cfg.instance));
    cfg.state = service.state;
    uint16_t port;
    int listener = t4_listen(cfg.bind, (uint16_t)cfg.port, &port);
    if (listener < 0) {
        perror("listen (omit --port, or use --port 0, for automatic allocation)");
        t4_service_close(&service); return 1;
    }
    if (t4_service_publish(&service, cfg.discovery, cfg.bind, port) < 0) {
        perror("publish service endpoint"); close(listener); t4_service_close(&service); return 1;
    }
    pid_t children[64] = {0};
    printf("[server] pid=%ld listening on %s:%u backend=%s",
           (long)getpid(), cfg.bind, port, cfg.backend->name);
    printf(" model-config=%s (native token speed)\n", cfg.model_config);
    printf("[server] discovery=%s state=%s; clients may omit --port\n", cfg.discovery, cfg.state);
    while (!stopped) {
        for (uint32_t i = 0; i < cfg.capacity; ++i) {
            if (children[i] && waitpid(children[i], NULL, WNOHANG) == children[i]) children[i] = 0;
        }
        struct pollfd descriptor = {listener, POLLIN, 0};
        int ready = poll(&descriptor, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll listener"); break; }
        if (!ready) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) { if (errno == EAGAIN || errno == EINTR) continue; perror("accept"); break; }
        int flags = fcntl(client, F_GETFL), enabled = 1;
        if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(client, F_SETFD, FD_CLOEXEC) < 0 ||
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
            perror("configure accepted socket"); close(client); continue;
        }
        uint32_t slot = 0;
        /* poll 等待期间旧 worker 也可能结束；接受新连接后再次回收。 */
        for (uint32_t i = 0; i < cfg.capacity; ++i)
            if (children[i] && waitpid(children[i], NULL, WNOHANG) == children[i]) children[i] = 0;
        while (slot < cfg.capacity && children[slot]) ++slot;
        if (slot == cfg.capacity) { send_error(client, "server at connection capacity", 100); close(client); continue; }
        pid_t parent = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(1);
            close(listener);
            t4_service_child_close(&service);
            int result = worker(client, &cfg);
            _exit(result);
        }
        close(client);
        if (pid < 0) { perror("fork worker"); continue; }
        children[slot] = pid;
    }
    close(listener);
    /* 终止时回收所有工作进程；工作进程负责结束并回收自己的生成器。 */
    for (uint32_t i = 0; i < cfg.capacity; ++i) if (children[i]) kill(children[i], SIGTERM);
    for (uint32_t i = 0; i < cfg.capacity; ++i) if (children[i]) {
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    }
    printf("[server] all workers reaped; state retained.\n");
    t4_service_close(&service);
    return stopped ? 0 : 1;
}
```

### 附录 24：src/storage/checkpoint.c

```c
#include "task4/checkpoint.h"
#include "task4/files.h"
#include "task4/text.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int t4_checkpoint_lock(const char *path)
{
    char lock[PATH_MAX];
    int n = snprintf(lock, sizeof(lock), "%s.lock", path);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    int fd = open(lock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    /* Only the lock owner may update this informational PID. A paused/live
     * owner's lock fails above and is never changed or unlinked here. */
    struct stat metadata;
    if (fstat(fd, &metadata) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1) {
        close(fd); errno = EINVAL; return -1;
    }
    char owner[64];
    n = snprintf(owner, sizeof(owner), "pid=%ld\n", (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(owner) || lseek(fd, 0, SEEK_SET) < 0 ||
        t4_write_all(fd, owner, (size_t)n) < 0 || ftruncate(fd, n) < 0 || fsync(fd) < 0) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}

/* FNV-1a 检测残缺/损坏记录，不作为安全校验或网络认证。 */
static uint32_t checksum(const void *header, const void *body, size_t length)
{
    const unsigned char *p = header, *b = body;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < 12; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ b[i]) * UINT32_C(16777619);
    return hash;
}

static int record_write(int fd, uint32_t type, uint32_t offset, const void *body, uint32_t length)
{
    unsigned char record[20];
    uint32_t header[4] = {htonl(type), htonl(offset), htonl(length), 0};
    header[3] = htonl(checksum(header, body, length));
    memcpy(record, header, sizeof(header));
    if (length) memcpy(record + sizeof(header), body, length);
    return t4_write_all(fd, record, sizeof(header) + length);
}

static int steer_path(char target[PATH_MAX], const char *path)
{
    int n = snprintf(target, PATH_MAX, "%s.steer", path);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int parent_path(char parent[PATH_MAX], const char *path)
{
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return 0;
}

/* The main checkpoint lock also covers this sidecar. A queued correction is
 * durable before CANCEL is sent, and disappears only after the next turn's
 * checkpoint has atomically replaced the previous turn. */
static int steer_remove(const char *path, struct t4_checkpoint *c)
{
    char target[PATH_MAX], parent[PATH_MAX];
    if (steer_path(target, path) < 0 || parent_path(parent, path) < 0) return -1;
    if (unlink(target) < 0 && errno != ENOENT) return -1;
    if (t4_directory_sync(parent) < 0) return -1;
    c->queued_request = c->queued_length = 0;
    return 0;
}

static int steer_load(const char *path, struct t4_checkpoint *c)
{
    char target[PATH_MAX];
    if (steer_path(target, path) < 0) return -1;
    int fd = open(target, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char magic[32], token[64], numbers[128], extra;
    int result = -1;
    if (!fgets(magic, sizeof(magic), f) || strcmp(magic, "T4STEER1\n") ||
        !fgets(token, sizeof(token), f) || strlen(token) != 33 || token[32] != '\n' ||
        memcmp(token, c->session, 32) || !fgets(numbers, sizeof(numbers), f) ||
        sscanf(numbers, "%u %u %c", &c->queued_request, &c->queued_length, &extra) != 2 ||
        !c->queued_request || c->queued_request > 10000 || !c->queued_length || c->queued_length > T4_MAX_PROMPT ||
        fread(c->queued_prompt, 1, c->queued_length, f) != c->queued_length ||
        memchr(c->queued_prompt, 0, c->queued_length) || fgetc(f) != EOF || ferror(f)) {
        errno = EINVAL;
        goto cleanup;
    }
    c->queued_prompt[c->queued_length] = 0;
    if (c->queued_request != c->request) {
        /* Crash after replacing the checkpoint but before deleting the queue. */
        if (c->queued_request + 1 != c->request || c->queued_length != c->prompt_length ||
            memcmp(c->queued_prompt, c->prompt, c->prompt_length)) { errno = EINVAL; goto cleanup; }
        result = steer_remove(path, c);
    } else result = 0;
cleanup:
    {
        int saved = errno;
        fclose(f);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_queue(const char *path, struct t4_checkpoint *c,
                        const unsigned char *prompt, uint32_t length)
{
    if (!c->request || !length || length > T4_MAX_PROMPT || memchr(prompt, 0, length) ||
        c->queued_length) { errno = EINVAL; return -1; }
    char target[PATH_MAX], temporary[PATH_MAX], parent[PATH_MAX], header[160];
    if (steer_path(target, path) < 0 || parent_path(parent, path) < 0) return -1;
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", target);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    int result = -1;
    n = snprintf(header, sizeof(header), "T4STEER1\n%s\n%u %u\n", c->session, c->request, length);
    if (n < 0 || (size_t)n >= sizeof(header) || t4_write_all(fd, header, (size_t)n) < 0 ||
        t4_write_all(fd, prompt, length) < 0 || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    if (rename(temporary, target) < 0 || t4_directory_sync(parent) < 0) goto cleanup;
    c->queued_request = c->request; c->queued_length = length;
    memcpy(c->queued_prompt, prompt, length); c->queued_prompt[length] = 0;
    result = 0;
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (result < 0) unlink(temporary);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_load(const char *path, struct t4_checkpoint *c)
{
    c->session[0] = 0;
    c->request = c->offset = c->prompt_length = c->file_lines = 0;
    c->queued_request = c->queued_length = 0;
    c->done = 1; c->journal_loaded = 0;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    FILE *f = fdopen(fd, "r+");
    if (!f) { close(fd); return -1; }
    char magic[32], token[64], numbers[128], extra;
    int result = -1;
    if (!fgets(magic, sizeof(magic), f) || strcmp(magic, "T4CHECK2\n") ||
        !fgets(token, sizeof(token), f) || strlen(token) != 33 || token[32] != '\n' ||
        !fgets(numbers, sizeof(numbers), f) ||
        sscanf(numbers, "%u %u %u %c", &c->request, &c->prompt_length, &c->file_lines, &extra) != 3 ||
        c->prompt_length > T4_MAX_PROMPT || c->request > 10000) goto invalid;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) goto invalid;
    memcpy(c->session, token, 32); c->session[32] = 0;
    if (fread(c->prompt, 1, c->prompt_length, f) != c->prompt_length ||
        memchr(c->prompt, 0, c->prompt_length)) goto invalid;
    c->prompt[c->prompt_length] = 0;
    c->done = 0;
    off_t last_good = ftello(f);
    if (last_good < 0) goto cleanup;
    for (;;) {
        uint32_t header[4];
        unsigned char body[4];
        size_t n = fread(header, 1, sizeof(header), f);
        if (ferror(f)) goto cleanup;
        if (!n) break;
        if (n != sizeof(header)) goto torn_tail;
        uint32_t type = ntohl(header[0]), offset = ntohl(header[1]), length = ntohl(header[2]);
        if (length > 4 || offset != c->offset || c->done) goto invalid;
        if (fread(body, 1, length, f) != length) {
            if (ferror(f)) goto cleanup;
            goto torn_tail;
        }
        if (ntohl(header[3]) != checksum(header, body, length)) goto invalid;
        if (type == T4_DELTA) {
            if (!t4_character_valid(body, length) || length > T4_MAX_RESPONSE - c->offset) goto invalid;
            memcpy(c->answer + c->offset, body, length);
            c->offset += length;
        } else if ((type == T4_END || type == T4_CANCELLED) && !length)
            c->done = type == T4_END ? 1U : 2U;
        else goto invalid;
        last_good = ftello(f);
        if (last_good < 0) goto cleanup;
    }
    goto loaded;
torn_tail:
    /* 崩溃写出半条记录时，只回退最后一条，不承认半个 UTF-8 字符。 */
    if (ftruncate(fd, last_good) < 0 || fsync(fd) < 0) goto cleanup;
loaded:
    c->saved_request = c->request; c->saved_offset = c->offset; c->saved_done = c->done;
    c->journal_loaded = 1;
    result = 0;
    goto cleanup;
invalid:
    errno = EINVAL;
cleanup:
    {
        int saved = errno;
        fclose(f);
        errno = saved;
        return result == 0 ? steer_load(path, c) : result;
    }
}

static int checkpoint_replace(const char *path, struct t4_checkpoint *c)
{
    char temp[PATH_MAX], parent[PATH_MAX], header[160];
    int n = snprintf(temp, sizeof(temp), "%s.tmp-XXXXXX", path);
    if (n < 0 || n >= PATH_MAX || strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    n = snprintf(header, sizeof(header), "T4CHECK2\n%s\n%u %u %u\n",
                 c->session, c->request, c->prompt_length, c->file_lines);
    int result = -1;
    if (n < 0 || (size_t)n >= sizeof(header) ||
        t4_write_all(fd, header, (size_t)n) < 0 ||
        t4_write_all(fd, c->prompt, c->prompt_length) < 0) goto cleanup;
    /* 新轮次通常 offset=0；必要时重建也保留完整历史前缀。 */
    for (uint32_t offset = 0; offset < c->offset;) {
        unsigned width = t4_character_width(c->answer[offset]);
        if (!width || width > c->offset - offset ||
            !t4_character_valid(c->answer + offset, width) ||
            record_write(fd, T4_DELTA, offset, c->answer + offset, width) < 0) goto cleanup;
        offset += width;
    }
    if ((c->done && record_write(fd, c->done == 2 ? T4_CANCELLED : T4_END,
                                 c->offset, NULL, 0) < 0) || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    if (rename(temp, path) < 0 || t4_directory_sync(parent) < 0) goto cleanup;
    result = 0;
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (result < 0) unlink(temp);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_save(const char *path, struct t4_checkpoint *c)
{
    int result;
    if (!c->journal_loaded || c->request != c->saved_request) result = checkpoint_replace(path, c);
    else {
        if (c->offset == c->saved_offset && c->done == c->saved_done) return 0;
        if (c->offset < c->saved_offset || c->saved_done) { errno = EINVAL; return -1; }
        int fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -1;
        result = 0;
        uint32_t n = c->offset - c->saved_offset;
        if (n && (n > 4 || !t4_character_valid(c->answer + c->saved_offset, n) ||
                  record_write(fd, T4_DELTA, c->saved_offset, c->answer + c->saved_offset, n) < 0)) result = -1;
        if (!result && c->done && record_write(fd, c->done == 2 ? T4_CANCELLED : T4_END,
                                             c->offset, NULL, 0) < 0) result = -1;
        if (!result) result = fsync(fd);
        int saved = errno;
        if (close(fd) < 0 && !result) { result = -1; saved = errno; }
        errno = saved;
    }
    if (!result) {
        c->journal_loaded = 1; c->saved_request = c->request;
        c->saved_offset = c->offset; c->saved_done = c->done;
        if (c->queued_length && c->request == c->queued_request + 1)
            result = steer_remove(path, c);
    }
    /* A failed write/fsync/close may still have changed the journal. Its last
     * disk offset is now uncertain: a same-process retry must atomically
     * rebuild from the complete in-memory prefix, never append the record
     * again. Also retry a failed steering cleanup even when offset/done are
     * unchanged; otherwise the fast path could leave an old correction live. */
    if (result < 0) c->journal_loaded = 0;
    return result;
}
```

### 附录 25：src/storage/files.c

```c
#include "task4/files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int t4_path(char result[PATH_MAX], const char *directory, const char *name)
{
    int n = snprintf(result, PATH_MAX, "%s/%s", directory, name);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int t4_write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

int t4_directory_sync(const char *directory)
{
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = fsync(fd);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}
```

### 附录 26：src/storage/session.c

```c
#include "task4/store.h"
#include "task4/backend.h"
#include "task4/protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int make_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int token_valid(const char *token)
{
    if (strlen(token) != 32) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}

int t4_event(struct t4_session *s, const char *event, uint32_t request, uint32_t offset)
{
    char record[256];
    int n = snprintf(record, sizeof(record), "pid=%ld event=%s request=%u offset=%u\n",
                     (long)getpid(), event, request, offset);
    if (n < 0 || (size_t)n >= sizeof(record)) { errno = EOVERFLOW; return -1; }
    return t4_write_all(s->log_fd, record, (size_t)n) < 0 ? -1 : fsync(s->log_fd);
}

int t4_session_open(const char *root, const char *token, struct t4_session *s)
{
    char path[PATH_MAX];
    memset(s, 0, sizeof(*s));
    s->lock_fd = s->log_fd = -1;
    if (make_directory(root) < 0) return -1;
    if (*token) {
        if (!token_valid(token)) { errno = EINVAL; return -1; }
        memcpy(s->token, token, 33);
        if (t4_path(s->directory, root, token) < 0) return -1;
        struct stat st;
        if (lstat(s->directory, &st) < 0) return -1;
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    } else {
        unsigned char random[16];
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        size_t have = 0;
        while (have < sizeof(random)) {
            ssize_t n = read(fd, random + have, sizeof(random) - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return -1; }
            have += (size_t)n;
        }
        close(fd);
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(s->token + 2 * i, 3, "%02x", random[i]);
        if (t4_path(s->directory, root, s->token) < 0 ||
            mkdir(s->directory, 0700) < 0 || t4_directory_sync(root) < 0) return -1;
    }
    if (t4_path(path, s->directory, "session.lock") < 0) goto fail;
    s->lock_fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->lock_fd < 0 || flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) goto fail;
    if (t4_path(path, s->directory, "events.log") < 0) goto fail;
    s->log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->log_fd < 0 || t4_path(s->context, s->directory, "context.txt") < 0 ||
        t4_event(s, "connected", 0, 0) < 0) goto fail;
    return 0;
fail:
    {
        int saved = errno;
        t4_session_close(s);
        errno = saved;
        return -1;
    }
}

void t4_session_close(struct t4_session *s)
{
    if (s->log_fd >= 0) close(s->log_fd);
    if (s->lock_fd >= 0) close(s->lock_fd);
    s->log_fd = s->lock_fd = -1;
}

static int turn_path(char path[PATH_MAX], const char *session, uint32_t id)
{
    char name[32];
    snprintf(name, sizeof(name), "turn-%08u", id);
    return t4_path(path, session, name);
}

static int copy_file(int output, const char *path)
{
    unsigned char buffer[4096];
    int input = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0) return -1;
    int result = 0;
    for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && t4_write_all(output, buffer, (size_t)n) < 0)) {
            result = -1; break;
        }
        if (!n) break;
    }
    close(input);
    return result;
}

/* 历史从已完成轮次重建，避免断电发生于追加历史时造成重复对话。 */
static int context_build(struct t4_session *s, uint32_t request)
{
    int fd = open(s->context, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = -1;
    for (uint32_t i = 1; i < request; ++i) {
        char directory[PATH_MAX], path[PATH_MAX];
        if (turn_path(directory, s->directory, i) < 0 ||
            t4_path(path, directory, "cancelled") < 0) goto cleanup;
        /* Explicit cancellation is terminal, but partial output is not history. */
        if (access(path, F_OK) == 0) continue;
        if (t4_path(path, directory, "done") < 0 || access(path, F_OK) < 0) goto cleanup;
        if (t4_write_all(fd, "user: ", 6) < 0 ||
            t4_path(path, directory, "prompt.txt") < 0 || copy_file(fd, path) < 0 ||
            t4_write_all(fd, "\nassistant: ", 12) < 0 ||
            t4_path(path, directory, "response.txt") < 0 || copy_file(fd, path) < 0 ||
            t4_write_all(fd, "\n", 1) < 0) goto cleanup;
    }
    result = fsync(fd);
cleanup:
    close(fd);
    return result;
}

static void staging_remove(const char *directory)
{
    const char *names[] = {"prompt.txt", "response.txt", "backend.txt", "model-config.json"};
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (t4_path(path, directory, names[i]) == 0) unlink(path);
    rmdir(directory);
}

int t4_turn_open(struct t4_session *s, uint32_t request,
                 const unsigned char *prompt, uint32_t length,
                 char directory[PATH_MAX], uint32_t *cached, int *done,
                 const char *model_config, const struct t4_backend *backend)
{
    char path[PATH_MAX];
    struct stat st;
    if (!request || request > 10000 || length > T4_MAX_PROMPT ||
        memchr(prompt, 0, length)) { errno = EINVAL; return -1; }
    if (context_build(s, request) < 0 || turn_path(directory, s->directory, request) < 0)
        return -1;
    if (lstat(directory, &st) < 0) {
        if (errno != ENOENT) return -1;
        char staging[PATH_MAX];
        if (t4_path(staging, s->directory, ".pending-XXXXXX") < 0 || !mkdtemp(staging)) return -1;
        int fd = -1, result = -1;
        if (t4_path(path, staging, "prompt.txt") < 0) goto staged;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || t4_write_all(fd, prompt, length) < 0 || fsync(fd) < 0) goto staged;
        close(fd); fd = -1;
        if (backend->prepare(model_config, staging) < 0 ||
            t4_path(path, staging, "backend.txt") < 0) goto staged;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || t4_write_all(fd, backend->name, strlen(backend->name)) < 0 || fsync(fd) < 0)
            goto staged;
        close(fd); fd = -1;
        if (t4_path(path, staging, "response.txt") < 0) goto staged;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || fsync(fd) < 0) goto staged;
        close(fd); fd = -1;
        if (t4_directory_sync(staging) < 0 || rename(staging, directory) < 0 ||
            t4_directory_sync(s->directory) < 0) goto staged;
        result = 0;
staged:
        if (fd >= 0) close(fd);
        if (result < 0) { int saved = errno; staging_remove(staging); errno = saved; return -1; }
    } else if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    /* Resume only turns explicitly created for this model backend. */
    if (t4_path(path, directory, "backend.txt") < 0) return -1;
    int identity = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (identity < 0) return -1;
    {
        char name[64];
        ssize_t size = read(identity, name, sizeof(name));
        close(identity);
        if (size != (ssize_t)strlen(backend->name) || memcmp(name, backend->name, (size_t)size)) {
            errno = EINVAL; return -1;
        }
    }
    if (t4_path(path, directory, "prompt.txt") < 0) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    unsigned char original[T4_MAX_PROMPT + 1];
    if (fd < 0) return -1;
    ssize_t n = read(fd, original, sizeof(original));
    close(fd);
    if (n != (ssize_t)length || memcmp(original, prompt, length)) { errno = EINVAL; return -1; }
    if (t4_path(path, directory, "response.txt") < 0 || lstat(path, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > T4_MAX_RESPONSE) {
        errno = EFBIG; return -1;
    }
    *cached = (uint32_t)st.st_size;
    if (t4_path(path, directory, "done") < 0) return -1;
    *done = access(path, F_OK) == 0;
    if (t4_path(path, directory, "cancelled") < 0) return -1;
    if (access(path, F_OK) == 0) *done = 2;
    return t4_event(s, *done ? "replay" : "generate", request, *cached);
}

static int turn_terminal(const char *directory, const char *marker)
{
    char path[PATH_MAX];
    if (t4_path(path, directory, marker) < 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = fsync(fd);
    close(fd);
    return result < 0 ? -1 : t4_directory_sync(directory);
}

int t4_turn_done(const char *directory) { return turn_terminal(directory, "done"); }
int t4_turn_cancel(const char *directory) { return turn_terminal(directory, "cancelled"); }
```

### 附录 27：src/model/adapter.py

```python
#!/usr/bin/env python3
"""One turn's durable streaming adapter for an already-running local model.

The C worker owns sessions, client sockets, cancellation and response.txt.
This process owns model templates, raw generation, thinking and model metrics.
Its stdout is exclusively binary T4 frames; diagnostics go to stderr.
"""

import argparse
import json
import math
from pathlib import Path
import re
import sys
import time

from journal import Journal, atomic_bytes, atomic_json
from stream import ThinkingParser
from transport import (DELTA, END, ERROR, THINK, MAX_RAW, MAX_RESPONSE,
                       Frames, LocalServer, checked_text, normalize_config)


def read_text(path, limit=MAX_RESPONSE):
    with Path(path).open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{path}: text exceeds {limit} bytes")
    return checked_text(data.decode("utf-8", "strict"), str(path))


def history_messages(turn):
    """Use completed structured turns; cancelled answers never become context."""
    match = re.fullmatch(r"turn-([0-9]{8})", turn.name)
    if not match:
        raise ValueError("turn directory must be named turn-00000001, etc.")
    current = int(match.group(1))
    messages = []
    total = 0
    for prior in sorted(turn.parent.iterdir()):
        identity = re.fullmatch(r"turn-([0-9]{8})", prior.name)
        if (identity is None or int(identity.group(1)) >= current
                or not prior.is_dir() or not (prior / "done").is_file()
                or (prior / "cancelled").exists()):
            continue
        question = read_text(prior / "prompt.txt", 4096)
        answer = read_text(prior / "response.txt")
        total += len(question.encode("utf-8")) + len(answer.encode("utf-8"))
        if total > MAX_RAW:
            raise ValueError("completed history exceeds 32 MiB; start a new session")
        messages.extend(({"role": "user", "content": question},
                         {"role": "assistant", "content": answer}))
    messages.append({"role": "user", "content": read_text(turn / "prompt.txt", 4096)})
    return messages


def model_identity(properties):
    model_path = checked_text(properties.get("model_path"), "llama-server model_path")
    template = checked_text(properties.get("chat_template"), "llama-server chat_template")
    identity = {"model_path": model_path, "chat_template": template,
                "build_info": properties.get("build_info", "unknown")}
    default = properties.get("default_generation_settings", {})
    capacity = default.get("n_ctx") if isinstance(default, dict) else None
    if type(capacity) is int and capacity > 0:
        identity["context_tokens"] = capacity
    # Same path with a replaced GGUF must not silently change a resumed turn.
    try:
        metadata = Path(model_path).stat()
    except (FileNotFoundError, OSError):
        pass
    else:
        identity["model_file"] = {"size": metadata.st_size, "mtime_ns": metadata.st_mtime_ns}
    return identity


def context_capacity_compatible(saved, current):
    """Accept unchanged identity/config, or only an explicit capacity increase.

    Existing turns still use their frozen context ceiling. A larger runtime
    must never silently change sampling, model, template or other identities.
    """
    if saved == current:
        return True
    if not isinstance(saved, dict) or not isinstance(current, dict):
        return False
    old_capacity, new_capacity = saved.get("context_tokens"), current.get("context_tokens")
    return (type(old_capacity) is int and type(new_capacity) is int
            and 0 < old_capacity <= new_capacity
            and {key: value for key, value in saved.items() if key != "context_tokens"}
            == {key: value for key, value in current.items() if key != "context_tokens"})


def snapshot_for(turn, config, server):
    path = turn / "model-snapshot.json"
    if path.exists():
        document = json.loads(read_text(path, MAX_RAW))
        if not isinstance(document, dict) or document.get("version") != 1:
            raise ValueError("unsupported model snapshot")
        if not context_capacity_compatible(document.get("config"), config):
            raise ValueError("model config changed during this turn; restore the saved config or start a new turn")
        checked_text(document.get("prompt"), "frozen prompt")
        if document.get("messages", [])[-1:] != [{"role": "user", "content": read_text(turn / "prompt.txt", 4096)}]:
            raise ValueError("prompt changed after the model template was frozen")
    else:
        identity = model_identity(server.json_request("/props"))
        messages = history_messages(turn)
        rendered = server.json_request("/apply-template", {
            "messages": messages, "model": config["model"],
            "add_generation_prompt": True, "reasoning_format": "none",
            "chat_template_kwargs": {"enable_thinking": config["thinking"]},
        })
        prompt = checked_text(rendered.get("prompt"), "rendered prompt")
        # User text can mention tags; only the rendered assistant prefill opens reasoning.
        initial_thinking = prompt.rstrip().endswith("<think>")
        ThinkingParser(config["thinking"], initial_thinking)
        document = {"version": 1, "config": config, "identity": identity,
                    "messages": messages, "prompt": prompt,
                    "initial_thinking": initial_thinking}
        atomic_json(path, document)
    # Derived readable evidence can be rebuilt if the process died after snapshot fsync.
    atomic_bytes(turn / "rendered-prompt.txt", document["prompt"].encode("utf-8"))
    return document


def sanitized_timings(event):
    raw = event.get("timings", {})
    if not isinstance(raw, dict):
        return {}
    return {key: value for key, value in raw.items()
            if type(value) in (int, float) and math.isfinite(value)}


REPLAY_COUNTS = ("generated_tokens", "prompt_tokens", "context_tokens",
                 "attempt_max_tokens", "raw_prefix_bytes")
REPLAY_DURATIONS = ("elapsed_seconds", "first_content_seconds", "tokens_per_second")


def completed_metrics(turn, finished, final_bytes):
    """Restore observed completion statistics without timing the disk replay.

    New journals contain the final attempt's statistics before END is emitted.
    Older journals can use a completed derived metrics file or native timings;
    a missing count or duration remains absent, rather than becoming zero.
    """
    result = {}
    try:
        saved = json.loads(read_text(turn / "metrics.json", 65536))
    except (OSError, ValueError):
        saved = {}
    if not isinstance(saved, dict) or saved.get("phase") != "completed" or saved.get("final_bytes") != final_bytes:
        saved = {}
    durable = finished.get("metrics", {})
    for source in (saved, durable if isinstance(durable, dict) else {}):
        for key in REPLAY_COUNTS:
            value = source.get(key)
            if type(value) is int and value >= 0:
                result[key] = value
        for key in REPLAY_DURATIONS:
            value = source.get(key)
            if type(value) in (int, float) and math.isfinite(value) and value >= 0:
                result[key] = value
            elif key == "first_content_seconds" and key in source and value is None:
                result[key] = None
    timings = sanitized_timings(finished)
    count = timings.get("predicted_n")
    if type(count) is int and count >= 0:
        result["generated_tokens"] = count
    speed = timings.get("predicted_per_second")
    if type(speed) in (int, float) and speed >= 0:
        result["tokens_per_second"] = speed
    return result


class TurnAdapter:
    def __init__(self, turn, context, offset, config, frames, runtime_config=None):
        self.turn, self.context, self.offset = turn, context, offset
        self.config, self.frames = config, frames
        self.runtime_config = runtime_config
        self.final_offset = 0
        self.thinking_offset = 0
        self.metrics = {"phase": "loading", "model": config["model"],
                        "endpoint": config["endpoint"], "thinking": config["thinking"],
                        "resumed_at": offset, "generated_tokens": 0}

    def metrics_save(self, phase, **fields):
        self.metrics.update(phase=phase, final_bytes=self.final_offset,
                            thinking_bytes=self.thinking_offset, **fields)
        atomic_json(self.turn / "metrics.json", self.metrics)

    def emit(self, pieces, thinking_file):
        for kind, content in pieces:
            size = len(content.encode("utf-8"))
            if kind == "thinking":
                if self.thinking_offset + size > MAX_RESPONSE:
                    raise ValueError("thinking exceeds 16 MiB")
                thinking_file.write(content.encode("utf-8"))
                thinking_file.flush()
                # This is derived evidence; the raw journal was fsynced first.
                self.thinking_offset = self.frames.chunks(THINK, self.thinking_offset, content)
            else:
                if self.final_offset + size > MAX_RESPONSE:
                    raise ValueError("final answer exceeds 16 MiB")
                for character in content:
                    self.frames.send(DELTA, self.final_offset, character)
                    self.final_offset += len(character.encode("utf-8"))

    def execute(self):
        transport_config = self.config
        if self.runtime_config is not None:
            # This separate transport copy never enters model-snapshot.json;
            # the full frozen config still has its original strict comparison.
            transport_config = {**self.config, "endpoint": self.runtime_config["endpoint"]}
        server = LocalServer(transport_config)
        self.frames.status(phase="loading", model=self.config["model"], resumed_at=self.offset)
        frozen = snapshot_for(self.turn, self.config, server)
        journal = Journal(self.turn / "raw-generation.jsonl")
        thinking_file = None
        stream = None
        try:
            parser = ThinkingParser(self.config["thinking"], frozen["initial_thinking"])
            thoughts, answers, raw = [], [], []
            for record in journal.records:
                raw.append(record["text"])
                for kind, content in parser.feed(record["text"]):
                    (thoughts if kind == "thinking" else answers).append(content)
            if journal.finished:
                answers.extend(content for kind, content in parser.finish() if kind == "final")
            answer = "".join(answers).encode("utf-8")
            thought = "".join(thoughts)
            if len(answer) > MAX_RESPONSE or len(thought.encode("utf-8")) > MAX_RESPONSE:
                raise ValueError("derived model text exceeds 16 MiB")
            response = read_text(self.turn / "response.txt").encode("utf-8")
            if len(response) != self.offset or response != answer[:self.offset]:
                raise ValueError("response.txt does not match the durable raw-generation prefix")
            if self.offset > len(answer):
                raise ValueError("resume offset exceeds the durable answer")
            suffix = answer[self.offset:].decode("utf-8", "strict")
            atomic_bytes(self.turn / "thinking.txt", thought.encode("utf-8"))
            self.frames.status(phase="replaying", thinking_reset=True,
                               durable_answer_bytes=len(answer), durable_raw_bytes=journal.raw_bytes)
            self.thinking_offset = self.frames.chunks(THINK, 0, thought)
            self.final_offset = self.offset
            for character in suffix:
                self.frames.send(DELTA, self.final_offset, character)
                self.final_offset += len(character.encode("utf-8"))
            if journal.finished:
                self.metrics.pop("generated_tokens", None)
                self.metrics.update(completed_metrics(self.turn, journal.finished, len(answer)))
                self.metrics_save("completed", stop_type=journal.finished["stop_type"], replay_only=True,
                                  timings=sanitized_timings(journal.finished))
                self.frames.status(**self.metrics)
                self.frames.send(END, self.final_offset)
                return
            # Never render another template on resume, and never resample old bytes.
            # Fresh inference begins strictly after the persisted raw continuation.
            identity = model_identity(server.json_request("/props"))
            if not context_capacity_compatible(frozen["identity"], identity):
                raise ValueError("local model, build, template or context changed; restore the frozen runtime before resuming")
            self.metrics["endpoint"] = getattr(server, "endpoint", self.config["endpoint"])
            prompt = frozen["prompt"] + "".join(raw)
            tokens = server.json_request("/tokenize", {"content": prompt, "add_special": True,
                                                       "parse_special": True}).get("tokens")
            if not isinstance(tokens, list) or any(type(token) is not int for token in tokens):
                raise ValueError("/tokenize returned an invalid token list")
            # A resource upgrade permits old snapshots to reconnect, but the
            # old turn's budget stays frozen; only new turns use the larger cap.
            frozen_capacity = frozen["config"]["context_tokens"]
            capacity = min(frozen_capacity, identity.get("context_tokens", frozen_capacity))
            remaining = capacity - len(tokens) - 8
            if remaining <= 0:
                raise ValueError(f"context exhausted: {len(tokens)} prompt tokens, capacity {capacity}; /stop this turn and start a new session")
            budget = min(self.config["max_tokens"], remaining)
            payload = {"prompt": prompt, "stream": True, "n_predict": budget,
                       "cache_prompt": True, "return_progress": True,
                       "temperature": self.config["temperature"], "top_p": self.config["top_p"],
                       "top_k": self.config["top_k"], "min_p": self.config["min_p"],
                       "presence_penalty": self.config["presence_penalty"],
                       "model": self.config["model"]}
            self.metrics_save("processing", prompt_tokens=len(tokens), context_tokens=capacity,
                              attempt_max_tokens=budget, raw_prefix_bytes=journal.raw_bytes)
            self.frames.status(**self.metrics)
            thinking_file = (self.turn / "thinking.txt").open("ab")
            start = time.monotonic()
            first_content = None
            last_status = start
            generated_tokens = 0
            stream = server.completion(payload)
            for event in stream:
                if event is None:
                    break
                content = checked_text(event.get("content", ""), "model content")
                token_ids = event.get("tokens", [])
                # b11429 prompt-progress SSE includes a placeholder tokens=[0].
                # Empty generated text can still contain real sampled token IDs.
                token_count = (len(token_ids) if isinstance(token_ids, list)
                               and "prompt_progress" not in event else 0)
                if content:
                    if first_content is None:
                        first_content = time.monotonic()
                    journal.append({"kind": "raw", "text": content, "token_count": token_count})
                    self.emit(parser.feed(content), thinking_file)
                native_count = event.get("tokens_predicted")
                if type(native_count) is int and native_count >= 0:
                    generated_tokens = native_count
                else:
                    generated_tokens += token_count
                self.metrics["generated_tokens"] = generated_tokens
                now = time.monotonic()
                if now - last_status >= 1:
                    self.metrics_save("generating", elapsed_seconds=now - start,
                                      tokens_per_second=generated_tokens / max(now - start, 0.001))
                    self.frames.status(**self.metrics)
                    last_status = now
                if type(event.get("stop", False)) is not bool:
                    raise ValueError("llama-server stop flag is not boolean")
                if not event.get("stop", False):
                    continue
                stop_type = event.get("stop_type")
                timings = sanitized_timings(event)
                native_total = timings.get("predicted_n")
                if type(native_total) is int and native_total >= 0:
                    generated_tokens = native_total
                    self.metrics["generated_tokens"] = generated_tokens
                record = {"kind": "stopped", "stop_type": str(stop_type), "timings": timings}
                if event.get("truncated", False) or stop_type not in ("eos", "word"):
                    journal.append(record)
                    self.metrics_save("interrupted", stop_type=stop_type, timings=timings)
                    raise RuntimeError(f"generation stopped at {stop_type} (context_truncated={bool(event.get('truncated', False))}); raw checkpoint retained; rerun the same turn to continue, or /stop to cancel")
                try:
                    tail = parser.finish()
                except ValueError:
                    journal.append(record)
                    raise
                record["kind"] = "finished"
                elapsed = time.monotonic() - start
                completion_metrics = {
                    "elapsed_seconds": elapsed,
                    "first_content_seconds": None if first_content is None else first_content - start,
                    "generated_tokens": generated_tokens,
                    "tokens_per_second": timings.get("predicted_per_second", generated_tokens / max(elapsed, 0.001)),
                }
                # The journal is authoritative even if the adapter exits before
                # the derived metrics.json file can be replaced.
                record["metrics"] = {key: self.metrics[key] for key in REPLAY_COUNTS if key in self.metrics}
                record["metrics"].update(completion_metrics)
                journal.append(record)
                self.emit(tail, thinking_file)
                self.metrics_save("completed", stop_type=stop_type, timings=timings,
                                  **completion_metrics)
                self.frames.status(**self.metrics)
                self.frames.send(END, self.final_offset)
                return
            raise RuntimeError("model stream ended without an EOS/stop record; raw checkpoint retained for continuation")
        except Exception as error:
            self.metrics_save("error", error=str(error))
            raise
        finally:
            if stream is not None:
                stream.close()
            if thinking_file is not None:
                thinking_file.close()
            journal.close()


def main():
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument("--turn", required=True, type=Path)
    argument_parser.add_argument("--context", required=True, type=Path,
                                 help="reserved C interface; structured completed turns are authoritative")
    argument_parser.add_argument("--offset", required=True, type=int)
    argument_parser.add_argument("--config", required=True, type=Path)
    argument_parser.add_argument("--runtime-config", type=Path,
                                 help="current config supplies only transport endpoint; turn inference settings stay frozen")
    args = argument_parser.parse_args()
    frames = Frames()
    try:
        if not 0 <= args.offset <= MAX_RESPONSE:
            raise ValueError("offset must be 0..16777216 UTF-8 bytes")
        config = normalize_config(json.loads(read_text(args.config, 65536)))
        runtime_config = (normalize_config(json.loads(read_text(args.runtime_config, 65536)))
                          if args.runtime_config is not None else None)
        TurnAdapter(args.turn, args.context, args.offset, config, frames, runtime_config).execute()
        return 0
    except BrokenPipeError:
        print("model adapter: C worker disconnected; raw checkpoint retained", file=sys.stderr)
        return 1
    except Exception as error:
        message = str(error)
        print("model adapter: " + message, file=sys.stderr)
        try:
            frames.send(ERROR, args.offset, message.encode("utf-8", "replace")[:4096].decode("utf-8", "ignore"))
        except (OSError, ValueError):
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
```

### 附录 28：src/model/discovery.py

```python
"""Discover only compatible healthy loopback llama services.

The owned runtime registry stores transport location, not inference settings.
Every candidate is checked against live /health and /props. An explicit URL
always bypasses discovery. TASK4_MODEL_SERVICE_FILE isolates tests/custom runs.
"""

import http.client
import json
import os
from pathlib import Path
import stat
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / ".runtime" / "model-service.json"
LEGACY_ENDPOINT = "http://127.0.0.1:8081"
MAX_REGISTRY = 65536


def registry_path():
    return Path(os.environ.get("TASK4_MODEL_SERVICE_FILE", str(DEFAULT_REGISTRY))).absolute()


def owned_regular(descriptor, label):
    info = os.fstat(descriptor)
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_nlink != 1 or info.st_mode & 0o077):
        raise ValueError(f"{label} must be a private regular file owned by this user")


def runtime_directory(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
        raise ValueError("model discovery directory must be owned and not writable by other users")
    return path


def registry_records(document):
    """Read the original single-service format and the multi-service format."""
    if document is None:
        return []
    return [document] if document["version"] == 1 else document["services"]


def read_registry(path=None):
    path = Path(path) if path is not None else registry_path()
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    except FileNotFoundError:
        return None
    try:
        owned_regular(descriptor, "model service registry")
        with os.fdopen(descriptor, "rb", closefd=False) as source:
            content = source.read(MAX_REGISTRY + 1)
        if len(content) > MAX_REGISTRY:
            raise ValueError("model service registry is oversized")
        document = json.loads(content.decode("utf-8"))
        if not isinstance(document, dict) or document.get("version") not in (1, 2):
            raise ValueError("unsupported model service registry")
        if document["version"] == 2 and not isinstance(document.get("services"), list):
            raise ValueError("invalid model service registry list")
        from transport import normalize_config
        endpoints = set()
        for record in registry_records(document):
            if not isinstance(record, dict):
                raise ValueError("invalid model service registry record")
            endpoint = normalize_config({"endpoint": record.get("endpoint")})["endpoint"]
            if endpoint == "auto":
                raise ValueError("registry endpoint must be a concrete loopback URL")
            if endpoint in endpoints:
                raise ValueError("duplicate model service registry endpoint")
            record["endpoint"] = endpoint
            endpoints.add(endpoint)
        return document
    finally:
        os.close(descriptor)


def write_registry(path, document):
    path = Path(path)
    runtime_directory(path.parent)
    # Never follow/overwrite an unsafe pre-existing entry.
    read_registry(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write((json.dumps(document, ensure_ascii=False, sort_keys=True,
                                     indent=2, allow_nan=False) + "\n").encode("utf-8"))
            output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)
    finally:
        try: os.unlink(temporary)
        except FileNotFoundError: pass


def compatible_properties(config, properties):
    if not isinstance(properties, dict) or properties.get("model_alias") != config["model"]:
        return False
    path = properties.get("model_path")
    if not isinstance(path, str) or not path or not config["model_file"]:
        return False
    expected = Path(config["model_file"])
    if not expected.is_absolute(): expected = ROOT / expected
    if Path(path).resolve() != expected.resolve():
        return False
    settings = properties.get("default_generation_settings", {})
    capacity = settings.get("n_ctx") if isinstance(settings, dict) else None
    slots = properties.get("total_slots")
    return (type(capacity) is int and capacity >= config["context_tokens"]
            and type(slots) is int and slots >= config["slots"]
            and isinstance(properties.get("chat_template"), str)
            and bool(properties["chat_template"]))


def probe_service(endpoint, config, timeout=1.0):
    from transport import LocalServer, normalize_config
    concrete = normalize_config({**config, "endpoint": endpoint})
    if concrete["endpoint"] == "auto":
        raise ValueError("service probe needs a concrete loopback endpoint")
    server = LocalServer(concrete, timeout=timeout)
    try:
        if server.json_request("/health").get("status") != "ok": return None
        properties = server.json_request("/props")
        return properties if compatible_properties(config, properties) else None
    except (OSError, ValueError, RuntimeError, http.client.HTTPException):
        return None


def candidates(config, path=None, preferred=LEGACY_ENDPOINT):
    document = read_registry(path)
    endpoints = [record["endpoint"] for record in registry_records(document)]
    endpoints += [preferred] if preferred else []
    return list(dict.fromkeys(endpoints))


def resolve_endpoint(config, path=None, preferred=LEGACY_ENDPOINT):
    if config["endpoint"] != "auto":
        return config["endpoint"]
    for endpoint in candidates(config, path, preferred):
        if probe_service(endpoint, config) is not None:
            return endpoint
    raise RuntimeError("no compatible healthy local model service found; run bash scripts/model/serve.sh gpu")
```

### 附录 29：src/model/journal.py

```python
"""Durable, append-only raw-generation authority and atomic small snapshots."""

import json
import os
from pathlib import Path
import tempfile

from transport import MAX_RAW, checked_text


def atomic_bytes(path, content):
    path = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_json(path, document):
    atomic_bytes(path, (json.dumps(document, ensure_ascii=False, sort_keys=True,
                                 indent=2, allow_nan=False) + "\n").encode("utf-8"))


class Journal:
    """An incomplete final JSONL line is uncommitted and safely discarded.

    Newline-terminated malformed records are corruption and fail closed. Each
    chunk is fsynced before its corresponding display frames can be emitted.
    A 'finished' record is authoritative even if C did not create turn/done.
    """

    def __init__(self, path):
        self.path = Path(path)
        self.raw_bytes = 0
        self.finished = None
        self.last_stop = None
        self.records = self._load()
        self.output = self.path.open("ab", buffering=0)
        directory = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def _load(self):
        records = []
        if not self.path.exists():
            return records
        with self.path.open("r+b", buffering=0) as source:
            good = 0
            while True:
                line = source.readline(2 * 1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 2 * 1024 * 1024:
                    raise ValueError("raw journal line exceeds limit")
                if not line.endswith(b"\n"):
                    source.truncate(good)
                    os.fsync(source.fileno())
                    break
                record = json.loads(line.decode("utf-8"))
                if not isinstance(record, dict) or self.finished is not None:
                    raise ValueError("invalid raw journal record ordering")
                kind = record.get("kind")
                if kind == "raw":
                    text = checked_text(record.get("text"), "journal text")
                    self.raw_bytes += len(text.encode("utf-8"))
                    if self.raw_bytes > MAX_RAW:
                        raise ValueError("raw generation exceeds journal byte limit")
                    records.append(record)
                elif kind in ("stopped", "finished"):
                    if not isinstance(record.get("stop_type"), str):
                        raise ValueError("invalid raw journal stop record")
                    self.last_stop = record
                    if kind == "finished":
                        if record["stop_type"] not in ("eos", "word"):
                            raise ValueError("a token limit cannot be a finished journal")
                        self.finished = record
                else:
                    raise ValueError("unknown raw journal record")
                good = source.tell()
        return records

    def append(self, record):
        if self.finished is not None:
            raise ValueError("cannot append after finished generation")
        kind = record.get("kind")
        if kind == "raw":
            text = checked_text(record.get("text"), "model content")
            size = len(text.encode("utf-8"))
            if self.raw_bytes + size > MAX_RAW:
                raise ValueError("raw generation exceeds journal byte limit")
        payload = (json.dumps(record, ensure_ascii=False, separators=(",", ":"),
                              allow_nan=False) + "\n").encode("utf-8")
        if len(payload) > 2 * 1024 * 1024:
            raise ValueError("raw journal record exceeds byte limit")
        pending = memoryview(payload)
        while pending:
            count = self.output.write(pending)
            if not count:
                raise OSError("short raw journal write")
            pending = pending[count:]
        os.fsync(self.output.fileno())
        if kind == "raw":
            self.raw_bytes += size
            self.records.append(record)
        elif kind in ("stopped", "finished"):
            self.last_stop = record
            if kind == "finished":
                self.finished = record

    def close(self):
        self.output.close()
```

### 附录 30：src/model/stream.py

```python
"""Incremental separation of Qwen-style <think> markup from the final answer.

Only the opening reasoning block is interpreted. Literal markup in a final
answer is preserved. Pending marker prefixes stay in memory and in the raw
journal until later chunks disambiguate them.
"""

from transport import checked_text


class ThinkingParser:
    def __init__(self, thinking=True, initial_thinking=False):
        if initial_thinking and not thinking:
            raise ValueError("template opened <think> although thinking=false; check --jinja/template settings")
        self.mode = "thinking" if initial_thinking else ("prefix" if thinking else "final")
        self.pending = ""

    def feed(self, text):
        checked_text(text, "model content")
        self.pending += text
        if self.mode == "prefix":
            candidate = self.pending.lstrip()
            if candidate.startswith("<think>"):
                self.pending = candidate[len("<think>"):]
                self.mode = "thinking"
            elif not candidate or "<think>".startswith(candidate):
                return []
            else:
                self.mode = "final"
        if self.mode == "final":
            output, self.pending = self.pending, ""
            return [("final", output)] if output else []
        marker = "</think>"
        position = self.pending.find(marker)
        if position != -1:
            thought = self.pending[:position]
            answer = self.pending[position + len(marker):]
            self.pending = ""
            self.mode = "final"
            return [(kind, content) for kind, content in (("thinking", thought), ("final", answer)) if content]
        keep = 0
        for length in range(1, min(len(marker), len(self.pending) + 1)):
            if self.pending.endswith(marker[:length]):
                keep = length
        emit = self.pending[:-keep] if keep else self.pending
        self.pending = self.pending[-keep:] if keep else ""
        return [("thinking", emit)] if emit else []

    def finish(self):
        if self.mode == "thinking":
            raise ValueError("model stopped before </think>; raw checkpoint retained for continuation")
        text, self.pending = self.pending, ""
        self.mode = "final"
        return [("final", text)] if text else []
```

### 附录 31：src/model/transport.py

```python
"""Local-only llama.cpp HTTP and the generator's binary frame transport.

API reference: llama.cpp d81235049384534c167caea52b85a694f6103d14,
tools/server/README.md: /props, /apply-template, /tokenize and /completion.
http.client deliberately bypasses environment proxy settings and redirects.
"""

import http.client
import ipaddress
import json
import math
import os
import struct
from urllib.parse import urlsplit

MAGIC = 0x54345331
DELTA, END, ERROR, THINK, STATUS = 6, 7, 8, 10, 13
MAX_FRAME = 4096
MAX_RESPONSE = 16 * 1024 * 1024
MAX_RAW = 32 * 1024 * 1024


def checked_text(value, label):
    if not isinstance(value, str) or "\0" in value:
        raise ValueError(f"{label} must be NUL-free text")
    value.encode("utf-8", "strict")
    return value


def normalize_config(document):
    if not isinstance(document, dict):
        raise ValueError("model config must be a JSON object")
    defaults = {
        "endpoint": "http://127.0.0.1:8081", "model": "local-model",
        "thinking": True, "max_tokens": 4096, "context_tokens": 4096,
        "temperature": 0.6, "top_p": 0.95, "top_k": 20,
        "min_p": 0.0, "presence_penalty": 0.0,
        "model_file": "", "slots": 1,
    }
    unknown = set(document) - set(defaults)
    if unknown:
        raise ValueError("unknown model config fields: " + ", ".join(sorted(unknown)))
    config = {**defaults, **document}
    checked_text(config["model"], "model")
    checked_text(config["model_file"], "model_file")
    if not config["model"]:
        raise ValueError("model identity cannot be empty")
    checked_text(config["endpoint"], "endpoint")
    if config["endpoint"] == "auto":
        if not config["model_file"]:
            raise ValueError("automatic model discovery requires model_file")
    else:
        parsed = urlsplit(config["endpoint"])
        if (parsed.scheme != "http" or parsed.username is not None
                or parsed.password is not None or parsed.path not in ("", "/")
                or parsed.query or parsed.fragment or parsed.hostname is None):
            raise ValueError("endpoint must be auto or an http:// loopback IP and port, with no path/auth/query")
        try:
            address = ipaddress.ip_address(parsed.hostname)
            port = 80 if parsed.port is None else parsed.port
        except ValueError as error:
            raise ValueError("endpoint must use a literal loopback IP, never a hostname") from error
        if not address.is_loopback or "%" in parsed.hostname or not 1 <= port <= 65535:
            raise ValueError("only a literal loopback endpoint is permitted")
        host = f"[{address}]" if address.version == 6 else str(address)
        config["endpoint"] = f"http://{host}:{port}"
    if type(config["thinking"]) is not bool:
        raise ValueError("thinking must be true or false")
    for name, minimum, maximum in (("max_tokens", 1, 131072),
                                   ("context_tokens", 256, 1048576),
                                   ("top_k", 0, 10000), ("slots", 1, 64)):
        value = config[name]
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"{name} must be an integer in {minimum}..{maximum}")
    for name, minimum, maximum in (("temperature", 0.0, 2.0),
                                   ("top_p", 0.0, 1.0),
                                   ("min_p", 0.0, 1.0),
                                   ("presence_penalty", -2.0, 2.0)):
        value = config[name]
        if type(value) not in (int, float) or not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError(f"{name} must be a finite number in {minimum}..{maximum}")
    return config


class LocalServer:
    """Each request owns its connection; closing a stream cancels its inference."""

    def __init__(self, config, timeout=600):
        self.config = config
        self.endpoint = config["endpoint"]
        parsed = urlsplit(self.endpoint) if self.endpoint != "auto" else None
        self.host, self.port = (parsed.hostname, parsed.port) if parsed else (None, None)
        self.timeout = timeout

    def _request(self, path, document=None):
        if self.host is None:
            from discovery import resolve_endpoint
            self.endpoint = resolve_endpoint(self.config)
            parsed = urlsplit(self.endpoint)
            self.host, self.port = parsed.hostname, parsed.port
        connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        body = None if document is None else json.dumps(document, ensure_ascii=False, allow_nan=False).encode("utf-8")
        try:
            connection.request("GET" if document is None else "POST", path, body,
                               {"Content-Type": "application/json", "Connection": "close"})
            response = connection.getresponse()
            if response.status != 200:
                message = response.read(MAX_FRAME).decode("utf-8", "replace")
                raise RuntimeError(f"local llama-server {path}: HTTP {response.status}: {message}")
            return connection, response
        except BaseException:
            connection.close()
            raise

    def json_request(self, path, document=None):
        connection, response = self._request(path, document)
        try:
            body = response.read(MAX_RAW + 1)
            if len(body) > MAX_RAW:
                raise ValueError(f"{path} JSON response exceeds {MAX_RAW} bytes")
            result = json.loads(body.decode("utf-8"))
            if not isinstance(result, dict):
                raise ValueError(f"{path} must return a JSON object")
            return result
        finally:
            response.close()
            connection.close()

    def completion(self, document):
        connection, response = self._request("/completion", document)
        data = []
        event_bytes = 0
        try:
            while True:
                line = response.readline(1024 * 1024 + 1)
                if len(line) > 1024 * 1024:
                    raise ValueError("oversized SSE line")
                if not line:
                    if data:
                        yield self._event(data)
                    break
                line = line.rstrip(b"\r\n")
                if not line:
                    if data:
                        event = self._event(data)
                        data, event_bytes = [], 0
                        if event is None:
                            break
                        yield event
                elif line.startswith(b"data:"):
                    piece = line[5:]
                    if piece.startswith(b" "):
                        piece = piece[1:]
                    data.append(piece)
                    event_bytes += len(piece)
                    if event_bytes > 1024 * 1024:
                        raise ValueError("oversized SSE event")
                # SSE comments/pings and optional event/id fields carry no text.
        finally:
            response.close()
            connection.close()

    @staticmethod
    def _event(lines):
        data = b"\n".join(lines)
        if data == b"[DONE]":
            return None
        event = json.loads(data.decode("utf-8"))
        if not isinstance(event, dict):
            raise ValueError("SSE data must be a JSON object")
        if "error" in event:
            raise RuntimeError("local inference error: " + json.dumps(event["error"], ensure_ascii=False))
        return event


class Frames:
    def __init__(self, fd=1):
        self.fd = fd

    def send(self, kind, offset=0, text=""):
        payload = checked_text(text, "frame").encode("utf-8")
        if len(payload) > MAX_FRAME or not 0 <= offset <= 0xffffffff:
            raise ValueError("frame length or offset out of range")
        packet = struct.pack("!IIIII", MAGIC, kind, 0, offset, len(payload)) + payload
        remaining = memoryview(packet)
        while remaining:
            count = os.write(self.fd, remaining)
            if not count:
                raise BrokenPipeError("generator frame socket closed")
            remaining = remaining[count:]

    def chunks(self, kind, offset, text):
        pending, size = [], 0
        for character in checked_text(text, "stream"):
            width = len(character.encode("utf-8"))
            if size + width > MAX_FRAME:
                self.send(kind, offset, "".join(pending))
                offset += size
                pending, size = [], 0
            pending.append(character)
            size += width
        if pending:
            self.send(kind, offset, "".join(pending))
            offset += size
        return offset

    def status(self, **fields):
        self.send(STATUS, 0, json.dumps(fields, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
```

### 附录 32：Makefile

```makefile
CC = gcc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude -Ideps
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
DEPFLAGS = -MMD -MP
COMMON = build/net/protocol.o build/net/discovery.o build/storage/files.o
BACKENDS = build/backend/model.o build/backend/process.o
CLIENT = build/client/main.o build/client/terminal.o build/client/view.o build/client/screen.o build/client/status.o build/client/state.o build/storage/checkpoint.o
OBJECTS = $(COMMON) $(BACKENDS) $(CLIENT) build/storage/session.o build/server/main.o
TARGETS = bin/stream_server bin/stream_client

.PHONY: all verify clean
all: $(TARGETS)

bin/stream_server: build/server/main.o build/storage/session.o $(BACKENDS) $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

bin/stream_client: $(CLIENT) $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^$(if $(strip $(LDLIBS)), $(LDLIBS))

build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

build/tests/status_unit: tests/status_unit.c build/client/status.o include/task4/status.h include/task4/terminal.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/status_unit.c build/client/status.o $(LDLIBS)

build/tests/checkpoint_faults: tests/checkpoint_faults.c build/storage/checkpoint.o build/storage/files.o include/task4/checkpoint.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=write,--wrap=fsync,--wrap=close,--wrap=unlink -o $@ tests/checkpoint_faults.c build/storage/checkpoint.o build/storage/files.o $(LDLIBS)

verify: all build/tests/status_unit build/tests/checkpoint_faults
	./build/tests/status_unit
	./build/tests/checkpoint_faults
	python3 tests/verify.py
	python3 tests/verify_discovery.py
	python3 tests/verify_controls.py
	python3 tests/verify_terminal.py
	python3 tests/verify_tui.py
	python3 tests/verify_mouse_render.py
	python3 tests/client_sessions_unit.py
	python3 tests/verify_client_sessions.py
	python3 tests/verify_dashboard.py
	python3 tests/model_adapter_unit.py
	python3 tests/model_launch_unit.py
	python3 tests/verify_model.py

clean:
	rm -rf build bin

-include $(OBJECTS:.o=.d)
```

### 附录 33：config/model.json

```json
{
  "endpoint": "auto",
  "model": "Qwen3.5-4B",
  "model_file": ".runtime/models/Qwen3.5-4B-Q5_K_M.gguf",
  "thinking": true,
  "max_tokens": 4096,
  "context_tokens": 16384,
  "slots": 2,
  "temperature": 1.0,
  "top_p": 0.95,
  "top_k": 20,
  "min_p": 0.0,
  "presence_penalty": 1.5
}
```
