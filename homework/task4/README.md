# 实验四：并发流式文本交互系统

依据《Linux 操作系统设计实践》实验指导书的方案 2 完成。系统由服务端、客户端和独立文本生成程序组成：客户端输入问题后，服务器为这一轮随机选择本地语料，生成程序逐个输出 UTF-8 码点，服务器边读取边发送，客户端边接收边保存、显示，形成打字机效果。

同一服务器可以同时服务多个客户端；每个连接有自己的工作进程，每个会话有自己的日志、轮次目录和上下文文件。关闭一个客户端不会结束其他客户端的生成。随机选择分别执行，两个客户端仍可能抽中同一份语料。

当前执行本地语料方案，问题内容不决定语料内容。项目保留方案 4 所需的生成后端接口、上下文路径、分帧输入、低速生成等待和断点恢复结构；模型部署与推理适配见 [架构说明](docs/architecture.md)。

## 编译与依赖

在 `homework/task4` 目录执行 `make`

需要 Linux、GCC、GNU Make 和系统自带的 glibc/POSIX 接口。三个 C 程序不依赖第三方 C 库；验证额外需要 Python 3 标准库。详细依赖及将来的推理依赖组织约定见 [deps/README.md](deps/README.md)。

编译产物集中放在 `bin/` 和 `build/`。Makefile 为源文件生成 `.d` 头文件依赖，修改 `include/task4/` 中的接口会重新编译相关模块。

## 三个终端演示

三个终端均进入本目录。终端 A 启动服务器：

```sh
./bin/stream_server --rate 30
```

看到 `listening on 127.0.0.1:3340` 后，终端 B 启动客户端 A：

```sh
./bin/stream_client --checkpoint state/a.ckpt
```

终端 C 启动客户端 B：

```sh
./bin/stream_client --checkpoint state/b.ckpt
```

分别输入任意问题，例如 `请介绍 Linux 进程` 和 `流式输出如何恢复？`。可以观察文字持续出现，两端同时输出；在客户端 A 的终端按 Ctrl+C，客户端 B 应继续输出。回答结束后可以继续输入问题；输入完整的 `/quit` 或在等待输入时发送 EOF 结束客户端。服务器在其终端按 Ctrl+C 结束并回收工作进程和生成进程，保留会话状态。

客户端 A 被中断后，重复同一条客户端命令会恢复其未完成回答。此前已落盘的前缀保留在 checkpoint 中，终端显示尚未保存的后缀；这次恢复不会重新随机选择语料。每个客户端须使用独立 checkpoint。重复使用同一 checkpoint 表示继续原会话，演示新会话时改用另一个名称。

如果只需恢复当前回答并退出，使用：

```sh
./bin/stream_client --checkpoint state/a.ckpt --resume-only
```

`--resume-only` 不读取新的问题；若当前轮已完成，则握手后直接退出。

`--rate 30` 表示约每秒 30 个 Unicode 码点；`--rate 1` 便于观察慢速输出和恢复，`--rate 0` 立即输出，便于快速检查。磁盘同步、调度和网络开销会使实测速度低于设置值。码点速度与语言模型的 **token/s** 含义不同，当前没有 tokenizer 和模型吞吐统计。

## 文件输入与独立生成程序

可将多轮问题按行放入 UTF-8 文本文件；文件最后一行没有 LF 也会发送。例如：

```sh
printf 'first question\n第二个问题\n' > state/demo-prompts.txt
./bin/stream_client --input state/demo-prompts.txt --checkpoint state/file-demo.ckpt
```

恢复文件输入时，使用原来的 `--input` 和 `--checkpoint` 命令，输入文件内容应保持不变。客户端跳过此前已经消费的行，并先恢复未完成的一轮。不要把同一 checkpoint 在不同输入文件或交互输入之间混用。

checkpoint 是包含当前轮恢复元数据和回答追加记录的一个文件，不是纯文本回答。可使用只读辅助脚本查看进度或导出这一轮完整的持久回答前缀：

```sh
python3 scripts/checkpoint.py state/a.ckpt
python3 scripts/checkpoint.py state/a.ckpt --answer > state/a-answer.txt
```

完整多轮问题、回答及生成日志保存在服务端会话目录中，见 [状态文件组织](docs/architecture.md)。

生成程序也可以独立运行：

```sh
./bin/text_generator --source data/corpus/streaming.txt --rate 10
./bin/text_generator --source data/corpus/streaming.txt --rate 0 > state/generated.txt
```

生成程序的 stdout 只写原始 UTF-8 文本，stderr 输出来源、速率和错误信息。服务器通过管道读取 stdout，并将 stderr 保存到该轮的 `generator.log`，避免诊断文字混入回答。`--offset` 使用字节单位，必须落在 UTF-8 码点边界；`--prompt` 和 `--context` 当前只保留输入路径，语料生成器不读取其内容来生成回答。

## 参数与边界

```text
bin/stream_server [--bind IPv4] [--port 0..65535] [--state DIR]
  [--corpus DIR] [--generator PATH] [--rate 0..10000]
  [--idle-ms 100..3600000] [--max-clients 1..64]

bin/stream_client [--host HOST] [--port 1..65535] [--checkpoint FILE]
  [--input FILE] [--resume-only] [--idle-ms 1500..3600000]

bin/text_generator --source FILE [--prompt FILE] [--context FILE]
  [--offset BYTE_OFFSET] [--rate 0..10000]
```

| 参数或限制 | 默认值及含义 |
| --- | --- |
| 服务端地址、端口 | `127.0.0.1:3340`；`--port 0` 由系统选择空闲端口，客户端使用启动信息打印的实际端口 |
| 服务端状态目录 | `state/server`；父目录需存在，程序创建会话子目录 |
| 客户端 checkpoint | `state/client.ckpt`；父目录需存在，每个客户端分别指定 |
| 语料与生成程序 | `data/corpus` 中的普通 `.txt` 文件；`bin/text_generator` |
| 并发连接容量 | 默认 32，最大 64；达到容量后拒绝新增连接 |
| 服务端帧等待期限 | 默认 `600000` 毫秒 |
| 客户端帧等待期限 | 默认 `10000` 毫秒，最小 `1500` 毫秒 |
| 提问长度 | 每轮最多 4096 字节，不含 LF；允许空行，不允许 NUL |
| 回答长度 | 每轮最多 16 MiB；按完整 UTF-8 码点发送与保存 |
| 会话轮次 | 从 1 顺序递增，最多 10000 轮 |

客户端消费完整的超长或含 NUL 输入行，提示后跳过，下一行仍可处理。TCP 传输采用显式帧边界和请求序号，不依赖一次 `send` 对应一次 `recv`；完整字段与状态机见 [协议说明](docs/protocol.md)。

`--idle-ms` 是一次完整帧收发的期限，不是整段回答的总时限。生成器暂时没有输出时，服务器在约 1 秒空闲轮询后发送 `HEARTBEAT`；客户端收到它继续等待，游标不前移。这使低速生成和较长的首字等待可以继续使用同一连接。等待用户下一次提问仍受服务端帧期限约束。

服务器默认用于本机演示，握手 token 是恢复会话的持有者凭据，当前协议未提供 TLS 与用户认证；不宜直接作为公网服务使用。

## 目录与模块调用

```text
task4/
├── Makefile
├── README.md
├── report.md                    # 可编辑实验报告与源码附录
├── include/task4/               # 本项目公共接口
│   ├── protocol.h               # 帧、网络收发、地址与数字解析
│   ├── store.h                  # 服务端会话、轮次和持久化
│   ├── checkpoint.h             # 客户端恢复状态与文件锁
│   ├── files.h                  # 两侧共有的路径、完整写入和目录同步
│   ├── backend.h                # 生成后端生命周期
│   └── text.h                   # UTF-8 码点检查
├── src/
│   ├── server/main.c            # 接入、fork 工作进程、转发与心跳
│   ├── client/main.c            # 输入、checkpoint、接收与显示
│   ├── net/protocol.c           # TCP 帧收发和超时
│   ├── storage/session.c        # 独立会话目录、日志与上下文
│   ├── storage/checkpoint.c     # 客户端恢复记录与追加日志
│   ├── storage/files.c          # 公共文件工具，不依赖会话和后端
│   ├── backend/corpus.c         # 随机语料快照、pipe/fork/exec
│   └── generator/main.c         # 独立逐码点文本生成程序
├── deps/                        # 依赖说明，当前无第三方 C 库
├── data/
│   ├── corpus/                  # 默认必需语料数据
│   └── demo/corpus/             # 自动演示使用的短语料
├── tests/                       # Python 标准库集成验证
├── scripts/
│   ├── checkpoint.py            # 只读检查 checkpoint、导出回答
│   ├── demo.py                  # 重录并发、流式、恢复、隔离日志
│   └── render_report.py         # 保留报告正文，更新行数与源码附录
├── docs/                        # 架构和线协议详细说明
├── logs/                        # 归档的实测日志和验证结果
├── screenshots/                 # 实际 xterm 白底窗口截图
├── state/                       # 运行状态、断点和会话数据
├── bin/                         # make 生成的三个可执行程序
└── build/                       # make 生成的 .o 与 .d 文件
```

调用关系如下；箭头表示调用或数据传送，生成程序作为独立进程运行：

```text
client/main ── protocol ── TCP ── server/main（每连接工作进程）
    │                                  ├── storage/session（会话与轮次）
    └── storage/checkpoint             └── backend/corpus
            │                               │
            └── storage/files               └── pipe + fork + exec
                （session 也调用）               └── generator/main
```

`include/task4/` 只声明跨模块接口，源代码按职责分目录，系统头文件由编译器读取，不复制到项目。未来新增生成后端放入 `src/backend/`，相关外部依赖说明放入 `deps/`；客户端、帧收发及会话存储继续通过公共接口协作。

客户端只链接客户端入口、checkpoint、网络协议和公共文件工具；服务端会话管理与语料后端由服务器链接。`storage/files` 提供 `t4_path`、`t4_write_all` 与 `t4_directory_sync`，使两侧可以共享文件操作，而不互相依赖对方的会话或生成模块。

## 验证、状态和提交

```sh
make verify
make clean
```

`make verify` 启动真实的三个 C 程序，使用回环地址、临时端口和独立临时状态目录进行集成检查。检查覆盖并发隔离、逐字输出、UTF-8、低速等待、断点恢复和协议错误等行为，运行记录见 [logs/verification.txt](logs/verification.txt)。

维护源码或重新采集演示时，还可执行：

```sh
python3 scripts/demo.py
python3 scripts/render_report.py
```

`demo.py` 使用 `data/demo/corpus/` 和独立临时状态，重录 `logs/02-concurrent.txt` 至 `05-isolation.txt`；相应截图需随新日志重新采集。`render_report.py` 保留报告正文，更新源码行数统计及标记之后的源码附录，避免报告中的接口和代码与最终实现不同。

`make clean` 只删除 `build/` 和 `bin/`，保留语料、状态、报告、日志与截图。恢复会话需要同时保留服务端的 `state/server/` 和客户端 checkpoint；追加记录就在 checkpoint 文件内部，只保存一侧不足以恢复。文件锁在进程退出时由内核释放，锁文件本身可留存。

实验报告为 [report.md](report.md)，包含系统开发情况、运行截图、实验结果、编程工作总结和注释源码附录。提交前填写学号、姓名、专业与班级；转换 DOCX 时从本目录处理 Markdown，并确认 `screenshots/` 相对路径对应的图片已嵌入。运行时 PID、会话 token 和随机选择结果会变化。
