# 系统架构与方案 4 接口

本项目实现实验指导书方案 2 的并发流式文本交互。设计将客户端交互、网络帧、会话存储和文本来源分别组织，便于理解系统调用的协作关系，并为本地模型后端保留接口。

## 进程与数据路径

```text
stream_server 主进程
  ├─监听套接口，poll / accept，记录工作进程 PID
  ├─fork → 会话 A 工作进程
  │          ├─会话 A 的锁、日志、轮次与上下文
  │          └─fork + exec → text_generator A
  │                            stdout → pipe → 工作进程 → TCP → client A
  │                            stderr → 会话 A / 当前轮 / generator.log
  └─fork → 会话 B 工作进程
             ├─会话 B 的锁、日志、轮次与上下文
             └─fork + exec → text_generator B
                               stdout → pipe → 工作进程 → TCP → client B
```

主进程只负责接受连接与管理工作进程。每个工作进程在自己的套接口上执行握手、收集提问、重放回答或启动生成器，通过 `poll` 同时等待生成管道和客户端连接状态。一个客户端断开后，只停止、回收其工作进程关联的生成器；主进程和其他工作进程继续运行。

服务端容量默认 32 个连接，最多 64 个。已结束的工作进程通过 `waitpid(..., WNOHANG)` 回收后释放容量；服务器收到 SIGINT 或 SIGTERM 时关闭监听，通知工作进程结束并回收它们。Linux 的父进程死亡信号用于使工作进程和生成器随所属父进程退出，生成器停止函数负责关闭管道、发信号与 `waitpid` 回收。

## 模块边界

| 模块 | 对外接口或主要调用 | 责任 |
| --- | --- | --- |
| `src/client/main.c` | `t4_connect`、`t4_send`、`t4_recv`、checkpoint 接口 | 读取一行问题、分帧发送、验证回答序号/偏移、持久化后显示 |
| `src/server/main.c` | `t4_listen`、session 接口、backend 接口 | 接入并发连接、执行协议状态机、转发完整码点、心跳和回收 |
| `src/net/protocol.c` | `include/task4/protocol.h` | 网络字节序帧、部分收发、单调时钟期限、连接与监听 |
| `src/storage/session.c` | `include/task4/store.h` | 独立会话目录、非阻塞文件锁、问题/回答/日志及上下文重建 |
| `src/storage/checkpoint.c` | `include/task4/checkpoint.h` | 客户端恢复元数据、回答追加记录与文件锁 |
| `src/storage/files.c` | `include/task4/files.h` | 两侧共有的路径拼接、完整文件写入和目录同步；不依赖会话或生成后端 |
| `src/backend/corpus.c` | `include/task4/backend.h` | 随机选择并固定本轮语料、管道与生成器启动、停止和回收 |
| `src/generator/main.c` | stdin/stdout/stderr 和命令行 | 读取语料并逐码点输出，支持码点速率与字节恢复偏移 |
| `include/task4/text.h` | `t4_character_width`、`t4_character_valid` | 检查完整 UTF-8 码点，防止截断、过长编码、代理项与越界值 |

头文件由 `include/task4/` 统一提供，调用方用 `#include "task4/xxx.h"` 明确接口所属。Makefile 将目标文件放入与源文件相对应的 `build/` 子目录，通过 `-MMD -MP` 生成头文件依赖。三个可执行文件统一输出到 `bin/`，运行数据不进入构建目录。

客户端链接 `client/main`、`storage/checkpoint`、`net/protocol` 与 `storage/files`，服务端链接 `server/main`、`storage/session`、`backend/corpus`、`net/protocol` 与 `storage/files`。`checkpoint` 与 `session` 都通过 `files.h` 使用公共文件操作，因此客户端不需要链接服务端的会话或语料后端模块。独立生成程序链接 `generator/main` 与 `net/protocol`，借用其中的严格数字解析接口。

`data/demo/corpus/` 保存自动演示使用的短语料，默认运行仍选择 `data/corpus/`。`scripts/demo.py` 启动真实 C 程序，使用临时状态并重录并发、流式、恢复和隔离日志；`scripts/render_report.py` 保留报告正文，按当前头文件、源码、Makefile 和数据文件更新统计与源码附录。这些 Python 脚本服务于检查和文档维护，不进入 C 程序调用链。

## 会话与文件组织

一次首次连接由服务器生成 16 字节随机值，转换成 32 位小写十六进制 token。客户端保存该 token；重连时发送它，服务器打开原会话。一个会话同时只允许一个连接持有 `session.lock`，不同会话的锁和目录独立。

默认状态结构：

```text
state/
├── a.ckpt                       # 客户端 A：元数据、问题与回答追加记录
├── a.ckpt.lock                  # 客户端 A 的文件锁
└── server/
    ├── <session-A-token>/
    │   ├── session.lock
    │   ├── events.log           # 接入、生成/重放、完成、断开事件
    │   ├── context.txt          # 下一轮开始前，由已完成轮次重建
    │   ├── turn-00000001/
    │   │   ├── prompt.txt       # 本轮提问原文
    │   │   ├── source.txt       # 固定的随机语料快照
    │   │   ├── response.txt     # 服务端已持久化的回答前缀
    │   │   ├── generator.log    # 本轮生成器 stderr，重启时追加
    │   │   └── done             # 生成成功完成的持久标志
    │   └── turn-00000002/
    └── <session-B-token>/
```

语料从 `data/corpus/` 中的普通 `.txt` 文件分别随机选择；目录扫描使用蓄水池抽样，并通过系统随机源取样。新轮次先在会话目录下建立暂存目录，保存问题、复制所选语料并创建空回答文件，再将完成的目录发布为 `turn-000000NN`。恢复时继续使用该轮 `source.txt`，即使原语料库后来改变，也不会改变这一轮回答。

`context.txt` 在每轮处理前从该会话之前已经完成的轮次重建，包含 `user:` 与 `assistant:` 文本，不读其他 token 的目录。当前语料后端忽略提问及上下文内容；这个文件证明上下文按会话组织，并为将来的模型适配提供路径。正在生成的半段回答不会被当作已完成的上一轮历史。

## 流式处理与低速生成

客户端按行收集输入，然后以 `BEGIN → INPUT… → COMMIT` 发送。当前每个 `INPUT` 帧最多 128 字节，UTF-8 问题可能在帧之间拆开，但服务器先完整拼接再处理；只有 `COMMIT` 后才开始生成。因此当前交互是“分帧传输一条完整问题”，没有逐键输入时即时生成的行为。

生成器逐个读取并验证 UTF-8 码点，`fwrite` 后立即 `fflush(stdout)`。服务器从管道一次可能读到多个字节，也可能读到半个码点，使用小型组装缓冲区只在码点完整时生成 `DELTA`。客户端验证每个 `DELTA`，落盘后立即写 stdout 并 flush。TCP 使用 `TCP_NODELAY`，不等待整篇回答凑齐。

`--rate` 是语料输出的 Unicode 码点/秒，用 `nanosleep` 表达演示节奏；管道和网络等待使用 `poll`，没有以 sleep 轮询数据到达。`--rate 0` 去除演示节奏。语言模型可能一次输出多字节文本、出现较长首 token 等待或生成停顿；帧协议允许每个回答码点继续传输，工作进程在约 1 秒没有管道数据时发送心跳，避免客户端把暂时没有文字当作整轮失败。

每次帧收发使用 `CLOCK_MONOTONIC` 的独立期限，期限覆盖该帧报头与正文的全部部分收发，收到部分字节不会重置期限。它限制无法完成的帧等待；整轮生成没有固定总时限。客户端默认帧期限 10 秒且至少 1.5 秒，服务端默认 600 秒。心跳不增加回答偏移、不写入正文、不作为生成速度统计。

## 断点恢复与持久化顺序

服务端以完整码点为单位先追加 `response.txt` 并 `fsync`，然后发送 `DELTA`。客户端先将完整 `DELTA` 及其偏移保存到 checkpoint 的追加记录并同步，再显示文字。客户端 checkpoint 是一个同时包含轮次元数据和追加记录的文件：新轮次用临时文件、`fsync`、`rename` 与目录同步替换它，该轮回答逐码点追加；不在每个码点到达时重写全部回答，回答保存工作量随输出字节数线性增长。

重连后客户端发送 token、原请求编号、原问题以及已保存的回答字节偏移。服务器检查轮次顺序、问题与原始 `prompt.txt` 完全一致、偏移没有超过持久回答长度，然后从客户端偏移重放 `response.txt` 的后缀。若该轮已有 `done`，重放后发送 `END`；否则从服务端已持久化长度启动生成器，继续读取固定语料。

客户端追加记录若在中断时留下不完整尾部，只将完整、验证通过的记录作为恢复依据，并截去该尾部再继续追加；完整但校验错误的记录会被拒绝，不当成普通中断尾部忽略。恢复游标必须位于 UTF-8 码点边界，不能把半个中文字符当成已经完成。服务端、客户端两侧状态须共同保留。

终端显示与持久化文件不能形成一个原子事务：若客户端在文件同步后、显示前退出，重连不会再次显示那段已保存文字；完整正文仍保留在恢复记录中。这里保证的是持久正文和传输恢复的连续性，终端输出历史由终端自身保存。

## 客户端 checkpoint 格式

文件版本为 `T4CHECK2`，文件头按以下顺序保存；字段间的 LF 是文件格式的一部分，问题正文由长度界定：

```text
T4CHECK2\n
<32 字节 session token>\n
<request> <prompt_length> <file_lines>\n
<prompt_length 字节原问题>
<二进制追加记录>…
```

`file_lines` 记录文件输入已消费的行数。普通 `--input` 恢复先跳过这些行，再恢复未完成回答并继续后续问题；要求使用同一份未修改的输入文件。当前没有文件身份或内容摘要校验，所以不能通过换文件来延续同一个 checkpoint。`--resume-only` 只恢复待完成回答，不消费新输入。

每条追加记录是四个网络字节序 `uint32_t` 加 0–4 字节正文：

| 记录字段 | 长度 | 含义 |
| --- | --- | --- |
| `type` | 4 字节 | 使用 `T4_DELTA=6` 或 `T4_END=7` |
| `offset` | 4 字节 | 该条记录之前已保存的回答字节数 |
| `length` | 4 字节 | DELTA 为 1–4，END 为 0 |
| `checksum` | 4 字节 | 对前 12 字节报头及正文计算的 FNV-1a 32 位值 |
| `body` | `length` 字节 | 一个完整 UTF-8 码点；END 无正文 |

载入时从 offset=0 顺序验证每条记录，由 DELTA 累计恢复偏移，由 END 确认完成。FNV-1a 用于发现损坏记录，不用于认证。`scripts/checkpoint.py` 使用相同格式只读检查和导出当前轮回答；若发现残缺末尾只报告 `torn_bytes`，截断修复由 C 客户端持有锁时执行。

## 方案 4 的适配位置

公共接口定义于 `include/task4/backend.h`：

```c
struct t4_backend {
    const char *name;
    int (*prepare)(const char *corpus_dir, const char *turn_dir);
    int (*start)(const char *generator, const char *turn_dir,
                 const char *context_path, uint32_t offset, uint32_t rate,
                 struct t4_generation *generation);
};
```

`prepare` 在新轮次建立时固定这一轮的生成依据；`start` 接收轮次目录、会话上下文路径、已持久化字节偏移和演示速率，返回子进程 PID 与非阻塞 stdout 管道读端。工作进程通过统一接口读取数据并调用 `t4_generation_stop` 取消、回收生成器。

当前语料后端通过 `fork`、`dup2`、`exec` 启动 `text_generator`，传入 `source.txt`、`prompt.txt`、`context.txt` 路径。模型后端可放到 `src/backend/`，在 `prepare`/`start` 生命周期中读取该会话的提问和历史，启动本地推理，并保持 stdout 为 UTF-8 回答、stderr 为诊断。推理依赖、参数与安装说明集中放入 `deps/`。

语料恢复可以从固定文件准确跳到字节偏移；模型恢复需要新的生命周期实现，不能仅重新启动推理再跳过同样数量的字节。采样、推理缓存和运行环境可能使重生成内容变化。实际适配应保存足够的生成状态，或采用能保证已发前缀一致的恢复策略，并验证后续内容与已保存前缀衔接；网络层和存储层可继续复用已有 token、请求序号和字节游标。

真实 token/s 需要模型对应的 tokenizer、token 计数以及首 token 时间和后续吞吐计时。当前项目没有这些统计，不把码点速率标成模型速度。未来还需按模型资源能力安排并发、限定上下文长度，并将模型恢复策略纳入验证；当前源码提供接口与数据组织，不包含模型文件、推理部署或方案 4 的回答能力。
