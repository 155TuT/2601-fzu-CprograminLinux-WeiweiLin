# 实验四：本地 Qwen3.5-4B 并发流式对话

依据实验指导书 PDF 第 7–9 页的**方案 4**实现：C 服务端、C 客户端、独立生成进程；多个客户端同时输出；同一连接连续提问并携带本会话历史；模型在本机运行。

本机已确认 WSL2 可调用 NVIDIA GeForce RTX 5060 Laptop GPU（8 GB）。默认使用 **Qwen3.5-4B Q5_K_M + llama.cpp CUDA**，模型常驻内存，两个推理槽共享权重，**每槽 16384 token（16k）上下文**。选择依据、固定版本、校验和及实测速度见 [本地模型说明](docs/local-model.md)。不调用外部推理 API，首次安装需要联网下载运行库和模型。

生成时区分思考和正式回答；可以随时取消本轮、修改要求后重问。网络断开保留恢复资格，主动取消则结束该轮。仅完成的正式回答进入后续历史，思考及取消轮次留作本地运行记录。

## 快速运行方案 4

下面命令均在 `homework/task4` 执行。需要 Linux 5.3+（使用 pidfd 等待子进程）、GCC、Make、Python 3.12；Python 模块仅使用标准库。

```sh
make
python3 scripts/model/setup.py
```

安装脚本把固定版本的运行库、GGUF 模型放到忽略提交的 `.runtime/`，校验 SHA256，支持下载中断后续传；不安装系统显卡驱动。已有完整文件仍会重新校验并跳过下载，运行库压缩包会重新解包安装。

终端 A，启动常驻模型服务：

```sh
bash scripts/model/serve.sh gpu
```

看到模型加载完成后，终端 B 启动课程服务端：

```sh
./bin/stream_server --backend llama --state state/model-server
```

终端 C 启动客户端。以后每开一个新终端，运行同一条命令即可建立独立会话：

```sh
./bin/stream_client
```

客户端自动创建 `state/clients/session-XXXXXX/checkpoint.ckpt`，启动时显示具体路径；并行启动不会争用同一个 checkpoint。客户端没有固定的 Alice/Bob 身份，连接数量由服务端配置控制。需要接着以前的会话聊天时，可在界面输入 `/sessions`、`/switch ID`，也可启动时使用下文的 `--resume FILE`。

可以直接输入问题，按 Enter 发送；例如先问 `请记住本次项目代号是松树，只回复收到。`，再问 `本次项目代号是什么？`，检验多轮上下文。首次启动和排队时可能暂时没有 token；课程服务端约每秒发送心跳，客户端继续等待，心跳不显示为对话内容。

没有可用 CUDA 显卡时，终端 A 可改用 `bash scripts/model/serve.sh cpu`。本机实测 GPU 明显更快，默认采用 GPU。请保持推理服务与课程服务端运行。两层服务都支持自动端口：模型启动器复用已验证匹配的健康实例，否则由系统分配新端口；课程服务端默认使用 `--port 0`，由内核在绑定时分配可用端口。

### 自动端口与服务发现

日常仍使用上面的三条命令，不需要把服务端打印的端口复制给客户端，也不需要为了端口冲突结束其他程序。

- 模型服务的实际地址列表发布到 `.runtime/model-service.json`，默认模型配置 `endpoint: "auto"` 从该记录解析。8081 上的模型若健康且配置匹配，会直接复用，避免额外加载一份模型占用显存。
- 课程服务端把地址发布到 `state/services/`；无参数客户端发现唯一运行中的服务。服务记录与主进程持有的状态锁、每次启动的新实例标识绑定，已退出服务的残留记录不会被选中。
- 同一个 `--state` 只允许一个主服务。重复启动会提示已有地址；新命令的配置不会覆盖正在运行的实例。不同状态目录可以同时起服务，端口互不争用。
- 恢复时按 checkpoint 的会话 token 匹配原服务状态目录，服务端重启后可以换端口。多个新会话候选或复制了相同会话的多个服务会明确报歧义，不任意选一个。

需要选择特定服务或仍使用固定端口时：

```sh
./bin/stream_client --server-state state/model-server
./bin/stream_client --resume 路径/checkpoint.ckpt --server-state state/model-server

# 固定端口为显式选择，被占用时明确报错；省略 --port 或设置为 0 即自动分配
./bin/stream_server --backend llama --state state/model-fixed-server --port 3340
./bin/stream_client --host 127.0.0.1 --port 3340
```

`--discovery-dir DIR` 可更改服务注册目录，客户端与服务端应传相同目录；`--server-state DIR` 直接读取指定状态目录，不依赖注册目录扫描。显式 `--host` 需要同时指定 `--port`。自动发现连接会在创建会话前核对同一 TCP 连接上的实例标识，避免旧端口被复用后误建会话。固定地址连接保留原 HELLO 协议。

## 终端界面与生成期间打断

默认开启自制 C TUI：浅色背景、纯黑正文、灰色思考和指标，顶部显示会话，底部保留状态栏和输入区。每轮下方显示阶段、输入/上下文容量、生成 token、速率及耗时；STATUS 帧只更新这些属性，不追加对话消息。思考不进入下一轮模型历史。该界面借鉴 [OpenCode TUI](https://opencode.ai/docs/tui/) 的交互方式，绘制与编辑由本项目实现。

```sh
./bin/stream_client                 # 浅色 TUI，适合白底终端
./bin/stream_client --theme dark    # 深色 TUI
./bin/stream_client --plain         # 传统逐行输出
```

鼠标滚轮每次滚动对话区 3 行，PgUp/PgDn 在对话区翻页；键盘 ↑/↓ 单独用于输入历史，滚轮不会替换输入草稿。向上查看时暂停自动跟随新输出，向下回到底部后恢复跟随。Ctrl+L 重绘；`/thinking` 切换思考的显示，`/theme light` 和 `/theme dark` 切换主题。这些是本地显示命令，不发给模型，也不改变模型的思考开关。窗口变化时正文重新换行，底部草稿保留；小于 12 列或 4 行时显示放大提示，放大后恢复完整布局。界面、尺寸截图及验证方法见 [TUI 说明](docs/tui.md)。

真实终端使用独立的底部输入区，上方显示思考、回答和状态。方向键左右移动，Home/End 或 Ctrl+A/E 到行首/行尾，Backspace/Delete 删除完整 UTF-8 码点，Ctrl+U/K 删除光标前/后文字，Ctrl+W 删除前一个词，↑/↓ 浏览当前会话的输入历史（含切换时恢复的问题）。长问题水平滚动显示。中文组合输入由终端和输入法完成；编辑单位为码点，组合字符与 emoji 字素簇不是整体编辑单位。

支持终端的 bracketed paste：粘贴中的换行合并为空格，粘贴本身不提交，检查后按 Enter。缩放窗口后输入区重绘。生成中可以编辑草稿，新的输出、状态变化和回答结束都不会清掉草稿。

输入区提示 `correct /stop>` 时，提交普通文字会作为当前轮的纠偏要求，取消后重新提问；若等回答结束、提示变为 `question>` 后才按 Enter，则草稿成为下一轮问题。命令同样输入后按 Enter：

| 操作 | 行为 |
| --- | --- |
| `/stop` 或生成期间 Ctrl+C | 取消当前思考/回答，等待服务端确认后继续接受新问题 |
| `/steer 请用中文，限制在三句话内` | 先检查长度并持久化纠偏要求，取消旧轮；确认后把原问题与纠偏要求合并为下一轮问题 |
| `/quit` | 生成期间先取消再退出；等待问题时直接退出 |
| `/help` | 显示交互帮助 |
| `/sessions` | 列出本地受管会话 ID、保存/占用状态及切换命令 |
| `/new` | 在当前客户端内建立并切换到独立新会话 |
| `/switch session-XXXXXX`、`/switch 路径/checkpoint.ckpt` | 切换到已有会话；被另一客户端占用或损坏的 checkpoint 会明确报错 |
| `/delete`、`/delete session-XXXXXX` | 将当前或指定受管会话移入本地 `.trash`；删除当前会话前先切换到新会话，服务端副本保留 |
| `/retry` | 重新连接并续传当前未完成轮次；无未完成轮次时只重新连接 |
| `/thinking` | 显示/隐藏思考，不取消生成、不修改上下文 |
| `/theme light`、`/theme dark` | 即时切换界面配色 |

生成过程中使用 `/new`、`/switch` 或 `/delete`，会先断开当前传输并保留未完成轮次；切回后用 `/retry` 接着生成。切换不等于 `/stop`，不会给旧轮写取消标记。网络超时、模型报错等会显示在当前对话和状态属性中，TUI 继续运行；可 `/retry`、`/stop`，或切换会话。首次连接发生在 TUI 初始化之前，连接失败会直接退出；批处理、`--resume-only` 等非交互调用通过退出码报告错误。

`/delete` 只处理 `--session-dir` 下的 `session-XXXXXX` 受管目录，拒绝任意外部路径、符号链接和仍被其他客户端持锁的会话。成功后会显示 `.trash` 中的实际目录；其中 checkpoint、历史与原锁文件一起保留。需要恢复时，在相关客户端退出后把该目录移回原会话 ID，或用 `/switch` 指向回收目录内的 checkpoint。该操作不删除服务端推理记录。

界面使用终端备用屏幕，退出后回到原命令行，并打印 checkpoint 路径。stdout 重定向时仍单独保存正式回答。非交互运行中，思考和一次最终统计写 stderr，正式回答写 stdout；需要调试原始 STATUS 时增加 `--verbose-status`。模型文本中的终端控制序列不会作为编辑命令执行。新文本先进入显示缓存，界面约每 34 ms 合并一帧，只发送发生变化的屏幕行。独立非阻塞终端输出保存尚未写完的帧；终端暂时接收不及，后续文本继续在有界缓存中合并，不阻塞网络读取，也不积压无数完整重绘帧。

纠偏采用“取消 → 确认 → 新请求”，不会修改已开始的模型输入；保留原问题使“改用另一种写法”有明确对象。旧轮的部分回答保留在磁盘，**不会作为完整 assistant 消息加入下一轮历史**。纠偏队列用 `<checkpoint>.steer` 持久保存，客户端意外退出后可继续处理。

交互控制针对真实终端。`--input FILE` 和管道输入保持逐行批处理，不把后续问题当作正在生成时的命令。超出 4096 字节的合并问题会在取消前被拒绝。

## 断线、恢复与文件输入

无参数命令总是建立新会话；`--resume FILE` 要求 checkpoint 已存在，并恢复旧会话。先用以下命令找到自动创建的会话路径，再把 `路径/checkpoint.ckpt` 替换为列表中的实际路径：

```sh
./bin/stream_client --list-sessions
./bin/stream_client --resume 路径/checkpoint.ckpt
```

`--session-dir DIR` 可更改自动会话目录，也可与 `--list-sessions` 一起使用。兼容参数 `--checkpoint FILE` 在文件不存在时建立指定会话、存在时恢复；日常使用无需手工为每个终端命名。

主动 `/stop` 是取消，**不是断点暂停**。要验证意外断线恢复，可从另一终端向客户端发送 SIGTERM，或关闭其终端；随后显式指定原 checkpoint 重连。恢复需使用原服务端状态目录，端口可以改变，不能删除服务端状态。显式连接远端时自行提供新的 `--host/--port`。

```sh
./bin/stream_client --resume 路径/checkpoint.ckpt --resume-only
./bin/stream_client --resume 路径/checkpoint.ckpt --cancel-pending
python3 scripts/checkpoint.py 路径/checkpoint.ckpt
python3 scripts/checkpoint.py 路径/checkpoint.ckpt --answer > state/answer.txt
```

`--resume-only` 恢复当前轮后退出，不读取新问题。TUI 会先恢复本地历史归档，再显示 checkpoint 中本轮已有正文；逐行模式和重定向 stdout 只输出尚未保存的后缀，完整内容可从 checkpoint 导出。活动 checkpoint 仍只保存当前轮；完整交互 TUI 在轮次完成或取消后，将正式问题、回答和终态归档到 `<checkpoint>.history/turn-00000001.ckpt`，切换或重开界面时按轮次恢复。归档不包含思考或 STATUS，也不参与模型提示构造。

没有本地归档的会话只能显示其现存 checkpoint 中的最近一轮；服务端仍保留模型需要的已完成历史。恢复应保留整个客户端会话目录，包括 checkpoint、可选 `.steer` 和 `.history/`，同时保留原服务端会话目录。界面有显示缓存上限，超长历史可能只显示最近部分，磁盘内容不因此删除。

`--cancel-pending` 在重连后立即取消旧未完成轮次并退出，适合模型离线、上下文用尽等无法继续生成的情况。已完成/取消的轮次不会重启。若已有持久纠偏队列，两种恢复命令都会继续执行该队列，避免丢掉用户已提交的修改要求；该参数只取消旧轮，不取消新纠偏轮。

模型恢复先重放服务端已保存的正式回答，再将**冻结的原始提示模板 + 已落盘的原始生成前缀**交给 llama.cpp 续写。模型不重新生成旧答案再跳过同样字节数。已保存前缀不会丢失或重复；进程重启后的 KV cache、采样随机状态不被保存，后续采样不保证与从未断线的运行逐 token 一致。思考流有独立游标，恢复时可重放思考，正式回答游标不受影响。

批量问题按行写入 UTF-8 文件：

```sh
printf '请用一句话介绍 fork\n它和 exec 有什么关系？\n' > state/questions.txt
./bin/stream_client --input state/questions.txt --checkpoint state/file.ckpt
```

重跑时保持同一输入文件、同一 checkpoint；客户端先恢复未完成轮次，再跳过已消费行。不要混用不同输入文件或交互模式。

### checkpoint 被占用与暂停进程

同一 checkpoint 同时只能由一个客户端持有。出现 `checkpoint is in use` 时，先检查原终端是否仍在运行客户端。Ctrl+Z 会暂停进程，**暂停后锁仍然有效**；在原终端运行 `jobs -l`、`fg`，回到客户端后用 `/quit` 正常退出，才能在另一终端恢复它。客户端暂停前恢复终端设置，继续前台运行后恢复编辑界面。另开独立会话可直接用无参数命令。

不要删除 `.lock` 文件“解锁”，也不要删除 checkpoint：删除锁文件可能使两个进程绕过互斥并同时写入。锁随进程退出自动释放，文件本身可以保留。`request=0` 的短 checkpoint 是已握手、尚未提交问题的合法空会话。重新编译后需重新启动相应程序，已运行或暂停的进程不会自动更新。

## 配置与边界

模型设置集中在 [config/model.json](config/model.json)：

| 配置 | 含义 |
| --- | --- |
| `endpoint` | 默认 `auto` 解析本地模型服务记录，也可显式指定回环 HTTP 地址；不使用代理或远程推理 |
| `model_file` / `model` | 固定 GGUF 路径与运行别名 |
| `thinking` | 由官方 chat template 开关思考；修改后对新轮次生效 |
| `max_tokens` | 每次推理尝试最多生成的 token 数，包含思考和正文，新轮次默认 4096 |
| `context_tokens` / `slots` | 默认每槽 16384 token、2 个推理槽；模型启动总容量为 32768 |
| 采样参数 | `temperature/top_p/top_k/min_p/presence_penalty`；默认采用模型卡的通用思考参数 |

服务端为每一轮保存配置快照；模型适配器还冻结模板、结构化历史及模型服务标识。修改运行配置不会悄悄改变正在恢复的轮次。切换 GGUF 或运行版本后，旧未完成轮次可能拒绝续写；可恢复原模型配置，或使用新 checkpoint 创建会话。

运行地址单独处理：C 服务端将当前 `--model-config` 路径作为 `--runtime-config` 传给模型适配器，只从中取连接端点。即使旧轮的快照保存了 8081，也能通过当前的 `auto` 配置找到模型的新端口；原快照、采样参数和输出预算不改写，实际模型/构建/模板身份仍必须匹配。

16k 是单会话模型输入与输出的共同窗口，包含模板、已完成历史和当前生成；新轮次每次推理尝试的输出上限为 4096 token。已有未完成轮次保留创建时的冻结配置。只增加运行时容量允许恢复，缩小容量或更换模型/模板仍会拒绝。每次建立新轮次会读取当前配置；修改生成预算无需重启模型或 C 服务端。本机已实测双槽同时接收 5529-token 输入，详见 [16k 记录](logs/model-context-16k.txt)，该实测使用记录中列明的配置。

达到 token 上限、上下文不足、HTTP 错误、模型进程退出都会明确报错，并保留已落盘前缀；这些情况不写 `done`。达到本次输出上限时可重连续写；旧轮已耗尽冻结上下文时，可用 `--cancel-pending` 结束该轮再提交新问题，或创建新会话。当前没有自动摘要、静默丢弃旧消息或工具执行功能。

`Connection timed out` 表示 I/O 等待超时，与 token 上限不同。客户端默认 `--idle-ms 10000`，收到合法的思考、正文、状态或保活帧会刷新等待期限；界面刷新和键盘事件不刷新网络期限。期限到达时先检查网络中已到达的数据；只有半帧的数据仍受完整帧期限约束。TUI 使用非阻塞绘制，发生超时后保留界面，可用 `/retry` 恢复当前轮；非交互调用以退出码报告失败。详见 [协议的期限说明](docs/protocol.md#帧期限与生成心跳)。

`stop_type=limit` 表示本次生成量达到预算，思考也计入其中；不等于历史占满上下文。判断时同时查看该轮 `metrics.json` 的 `prompt_tokens`、`context_tokens` 和 `context_truncated`（错误信息中），最终生成量以 `timings.predicted_n` 为准。状态只发往客户端显示，不作为后续模型消息；历史只包含已完成轮次的问题和正式回答。模型关于“以前回答过”“知识截止日期”的自述需要核验，不能作为会话是否串线的依据。上下文来源、指标计数和生成边界见 [本地模型说明](docs/local-model.md#上下文指标与资源边界)。

```text
bin/stream_server [--bind IPv4] [--port 0..65535] [--state DIR]
  [--discovery-dir DIR]
  [--backend llama] [--model-config config/model.json] [--adapter PATH]
  [--idle-ms 100..3600000] [--max-clients 1..64]

bin/stream_client [--host HOST --port 1..65535 | --server-state DIR]
  [--discovery-dir DIR]
  [--session-dir DIR | --resume FILE | --checkpoint FILE]
  [--input FILE] [--resume-only] [--cancel-pending] [--idle-ms 1500..3600000]
  [--plain] [--theme light|dark] [--verbose-status]
bin/stream_client --list-sessions [--session-dir DIR]
```

服务端使用 `llama` 后端，默认适配器为 `src/model/adapter.py`。默认接纳 32 个连接，最多 64 个；这是课程服务端的连接容量，与模型的两个并发推理槽不同，超过推理槽数量的请求需排队。每轮问题最多 4096 字节，正式回答最多 16 MiB，每会话最多 10000 轮。`INPUT` 支持分段上传，收到 `COMMIT` 才启动推理；没有逐键猜测尚未提交的问题。

默认仅用于本机实验。会话 token 用于恢复，协议尚未提供 TLS 或账户认证。帧等待期限不是整轮生成的总时限。

## 服务端后台看板

在另一个终端运行，只读观察各会话，不影响正在运行的 C 服务端：

```sh
python3 scripts/server/dashboard.py --state state/model-server
python3 scripts/server/dashboard.py --state state/model-server --once
python3 scripts/server/dashboard.py --state state/model-server --json
```

看板每秒刷新，↑/↓ 或 j/k 选择会话，q 退出。显示连接、worker PID、轮次、阶段、token、速度、上下文估计、思考/正文落盘字节数；默认不展示问题和思考正文，需要问题预览时加 `--preview`。`paused` 指持锁的服务端 worker 被暂停，`offline` 不等于回答完成；旧指标会标注快照时效。运行界面依赖 Python 标准库 curses；没有终端时使用 `--once`/`--json`。

## 目录与调用关系

```text
task4/
├── Makefile / README.md / report.md
├── include/task4/          # C 公共接口：协议、后端、持久化、UTF-8
├── src/
│   ├── server/main.c      # TCP 接入、每连接 fork、流式转发与取消
│   ├── client/
│   │   ├── main.c        # 会话切换、错误保活、poll、恢复和纠偏
│   │   ├── terminal.c    # termios、UTF-8 编辑、键鼠分流、粘贴及重绘节流
│   │   ├── view.c        # 可重排对话区、主题、灰色思考/指标、翻页
│   │   ├── screen.c      # 屏幕行差分、合帧缓存、有界非阻塞写出
│   │   ├── status.c      # 只解析显示需要的 STATUS 字段
│   │   └── state.c       # 会话列表/解析、软删除、终态正文归档与回放
│   ├── net/protocol.c     # 分帧、完整收发、超时
│   ├── net/discovery.c    # 服务租约、原子端口注册、恢复会话匹配
│   ├── storage/           # 会话、客户端 journal、文件工具
│   ├── backend/
│   │   ├── process.c     # fork/exec、FD、父进程死亡与回收
│   │   └── model.c       # 配置快照、启动独立 Python 适配器
│   └── model/            # 独立生成进程的 Python 模块
│       ├── adapter.py    # 上下文、模型调用、事件与统计
│       ├── transport.py  # 本地 HTTP/SSE
│       ├── discovery.py  # 健康模型实例识别、动态回环端点解析
│       ├── journal.py    # 原始生成日志、原子文件与恢复
│       └── stream.py     # 思考/回答分流和帧编码
├── config/model.json      # 本地推理设置
├── scripts/model/         # 固定版本安装、GPU/CPU 启动与实测
├── scripts/server/        # 独立只读 curses 后台看板
├── scripts/               # checkpoint 查看、演示、报告附录生成
├── tests/                 # 通信、交互控制、模拟本地模型检查
├── deps/jsmn/             # 固定版本 MIT JSON tokenizer 及许可证
├── deps/README.md         # 系统、推理和界面依赖说明
├── docs/                  # 架构、协议、界面和模型使用说明
├── logs/ / screenshots/   # 实测证据、真实白底终端截图
├── state/                 # 会话与 checkpoint，忽略提交
├── .runtime/              # 下载的模型/运行库，忽略提交
└── bin/ / build/           # C 可执行文件及 .o/.d，忽略提交
```

```text
C client ── TCP ── C worker（每连接独立）
  │                  ├── session/context/turn files
  └── checkpoint     └── backend + process（fork/exec）
                          └── socketpair → Python model adapter
                                                   └── loopback HTTP/SSE
                                                        └── llama-server → CUDA GPU
```

头文件仅声明跨模块接口；客户端不链接服务端存储或生成后端。Python 负责推理 JSON、SSE、模板和推理日志，C 负责 Linux 进程、TCP、并发、取消、持久转发和 TUI；客户端只用 jsmn 读取显示指标。推理引擎是现成依赖，其内部 C++ 源码不混入课程源文件。

## 验证与报告

```sh
make verify
python3 scripts/model/smoke.py
python3 scripts/model/tui_smoke.py
python3 scripts/model/controls.py
python3 scripts/render_report.py
make -q
```

`make verify` 不需要显卡、模型或联网，使用真实 C 程序和可控的本机 HTTP 测试服务。`smoke.py` 需要先启动真实 llama-server，运行独立状态中的 Qwen 多轮测试；`tui_smoke.py` 进一步使用全 stdio PTY 验证 4096 预算、历史归档和会话切换，实测见 [真实模型 TUI 记录](logs/model-tui-4096.txt)。测试范围与实测结果见 [report.md](report.md)，完整模块契约见 [架构](docs/architecture.md) 和 [协议](docs/protocol.md)。

`make clean` 只删除 C 构建产物，不删除模型、会话、报告与证据。提交源码时不用提交 `.runtime/`、`state/`。报告保留可编辑 Markdown，提交前填写个人信息；转 DOCX 时从本目录处理，并确认相对路径图片已嵌入。
