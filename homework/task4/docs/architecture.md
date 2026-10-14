# 方案 4 架构与持久化契约

## 进程和资源归属

```text
stream_server 主进程：listen / poll / accept / fork / waitpid
  ├─ worker A：TCP A、session A 的 flock、事件日志、当前轮 response.txt
  │    └─ fork/exec Python adapter A：原始生成 journal、HTTP 流 A
  └─ worker B：TCP B、session B 的 flock、事件日志、当前轮 response.txt
       └─ fork/exec Python adapter B：原始生成 journal、HTTP 流 B
                       │
                       └── 127.0.0.1 llama-server：共享只读权重、独立推理槽
                                      └── CUDA GPU（或 CPU）
```

课程服务端、客户端以及每轮独立生成进程仍分离。llama-server 是共享推理依赖，只加载一次模型；两个推理槽允许两次推理同时推进，更多请求排队。连接数与推理槽数分别限制，默认是 32 和 2。不能将多个工作进程分别加载整份模型，否则显存和冷启动成本会随连接数增长。

工作进程持有会话锁，处理一轮时 `poll` 同时等待生成输出、客户端控制帧和子进程 pidfd。生成器关闭 stdout 不等于进程已经退出；输出 EOF 后继续通过 pidfd 等待并响应取消、发送心跳，避免阻塞 `waitpid` 使控制失效。pidfd 就绪后使用 `waitpid(..., WNOHANG)` 回收。C 层启动生成器前令不相关文件描述符 close-on-exec；生成器 stdin 接 `/dev/null`，stderr 追加到本轮 `generator.log`。模型的带类型帧输出用 `socketpair`，从而复用完整帧接收函数。只有该工作进程能写该会话的正式回答与终态标记。

取消时，工作进程关闭生成连接、终止并 `waitpid` 回收生成子进程，然后保存 `cancelled` 并确认；独立适配器退出也关闭其 HTTP 流，llama-server 回收相应推理任务。其他工作进程、模型进程继续运行。服务端停止时逐一通知并回收 worker；Linux `PR_SET_PDEATHSIG` 同时覆盖父进程被 SIGKILL 的情况。模型服务独立启动、独立停止，不由某一个会话所有。

## 模块与依赖方向

| 模块 | 职责 |
| --- | --- |
| `client/main.c` | 问题分帧、socket/tty 并发等待、取消与纠偏、会话切换、错误恢复循环 |
| `client/terminal.c` | 非规范输入、UTF-8 编辑、键盘历史与鼠标滚动分流、粘贴、独立输入区 |
| `client/view.c` | 对话显示缓存、按宽度重排、主题、灰色思考/指标与滚动位置 |
| `client/screen.c` | 屏幕行差分、不可变待发送帧、有界非阻塞终端写出 |
| `client/status.c` | 显示指标解析、拥有独立存储的状态字段 |
| `client/state.c` | 独立新会话、稳定列表/解析、锁保护软删除、终态正文归档与回放 |
| `server/main.c` | 每连接状态机、生成事件验证/转发、心跳、取消确认 |
| `net/protocol.c` | 网络字节序、完整收发、帧总期限、监听与连接 |
| `net/discovery.c` | 主服务租约、自动端口发布、实例校验及恢复会话筛选 |
| `storage/session.c` | 会话锁、原子建立轮次、后端标识、历史和终态 |
| `storage/checkpoint.c` | 客户端回答 journal、校验/残尾修复、持久纠偏队列 |
| `storage/files.c` | 路径、完整文件写入、目录同步 |
| `backend/process.c` | 生成子进程与 FD 生命周期 |
| `backend/model.c` | 模型配置快照，启动独立 Python 适配器 |
| `model/adapter.py` | 完整历史、官方模板、本地生成、事件与统计 |
| `model/transport.py` | 限定回环地址、禁代理、HTTP/SSE 解析 |
| `model/discovery.py` | 验证健康状态/模型身份、解析受管回环端点 |
| `model/journal.py` | 原始生成日志、持久快照、残缺尾部修复 |
| `model/stream.py` | 跨 SSE 片段的思考/回答分流，T4 帧编码 |

公共接口统一位于 `include/task4/`。客户端只链接自身入口、terminal、view、screen、status、client state、checkpoint、files、protocol 和 discovery；不会引入会话或模型后端。服务端额外链接 session 和 backend。Makefile 的 `build/` 与源目录镜像，`.d` 文件记录头文件依赖。Python 只用标准库；C 客户端通过固定版本 jsmn 解析显示所需的 STATUS 字段，未知嵌套字段跳过，非法更新不会部分覆盖已显示指标。

## 自动端口与服务身份

课程服务端默认绑定端口 0，由内核在实际 `bind` 时分配端口，通过 `getsockname` 取得真实地址，不使用“探测一个空端口、关闭后再绑定”的竞态方式。显式非零 `--port` 保持固定端口语义。主进程持有 `<state>/server.lock` 独占锁，worker 在 fork 后关闭继承的主锁 FD；因此主进程被 SIGKILL 后不会由 worker 延长错误的服务存活记录。

每次启动生成新的 128 位实例标识，写入锁文件，再原子发布 `<state>/server.endpoint` 和 `state/services/<dev>-<inode>.endpoint`。读取者核对目录设备/inode、当前锁仍被持有、锁中的实例与两份记录一致；旧记录即使残留也不会成为活动候选。文件读取有长度上限，拒绝符号链接、FIFO 和其他非普通文件。正常退出仅删除属于本次实例的发布记录，保留会话与 checkpoint。

无参数客户端选择唯一活动服务；`--resume` 根据已有会话 token 筛选服务状态目录，因此地址变化不要求修改 checkpoint。多候选明确报歧义，可用 `--server-state DIR` 指定。连接后，在 HELLO 前进行只读 `PROBE → SERVICE(instance)`，再次核对同一连接上的实例，避免端口被复用时把新会话建在另一服务中。显式 `--host/--port` 不依赖本机注册文件，仍允许原来的直接 HELLO 握手。

本地模型使用独立的 `.runtime/model-service.json` 保存服务列表，兼容读取单实例记录。模型启动器验证已有实例的健康状态、模型文件/别名、槽数和容量，匹配时复用，否则使用 llama-server 的端口 0 启动并在健康后发布真实地址。发布、复用和退出清理共用启动锁；新启动或选中的服务排在列表首位，其他仍存活的实例保留。CPU 启动只复用已确认的 CPU 实例；GPU 启动跳过已确认的 CPU 实例，也允许复用模式未知的兼容外部服务，此时不能据复用结果确认 GPU 已启用。退出只移除对应实例，不删除其他服务的发现入口。模型发现与课程服务发现分开，二者都不把运行地址写进用户对话历史。

后端 `start` 接口接收当前 `runtime_config` 路径，以 `--runtime-config` 传给适配器。适配器仅取其中 endpoint 构造连接配置，推理参数仍来自本轮冻结的 `model-config.json`。因此已有快照能够跟随当前自动端口，恢复不改写快照中的推理设置，仍检查模型文件信息、构建、模板与容量。

## 交互终端与会话入口

默认启动使用 `mkdtemp` 创建独立会话目录，`--resume FILE` 显式恢复已有 checkpoint，`--checkpoint FILE` 保留创建或恢复的兼容行为。锁被占用时拒绝启动第二个写入者；暂停的进程仍持锁。正常退出自动释放锁，不删除锁文件。

stdin 与 stderr 均为 TTY 时启用 termios 编辑器：关闭 ICANON/ECHO、保留 ISIG，按字节接收并按完整 UTF-8 码点编辑。`terminal.c` 管理输入、终端生命周期和约 34 ms 的重绘节流；`view.c` 按窗口列数重排本地对话、生成固定底栏与灰色思考/指标的屏幕行；`screen.c` 比较上一帧，只发变化的行。正文采用浅色主题纯黑字，支持深色主题。STATUS 更新本轮缓存，HEARTBEAT 只维护网络期限，都不追加到对话正文。

绘制通过独立打开的 tty 文件描述符使用 `O_NONBLOCK`，不会更改父 shell 或 stdin/stdout 的文件状态标志。每次循环最多尝试 4 次写出；部分写入保留游标，`EAGAIN` 交回主循环。只有一个不可变待发送帧，帧未排空时后续变化继续合并到显示状态；排空后再生成最新行差分，避免无限堆积旧重绘帧。每个输出缓冲区有 8 MiB 上限，退出排空也有有限等待时间。界面拥塞不会用阻塞写占住网络事件循环，文本顺序和持久化顺序不变。

输出完成、状态更新、窗口缩放都不清除尚未提交的草稿。支持 bracketed paste，粘贴换行归一为空格，必须显式 Enter 才提交。启用 SGR 鼠标报告并解析传统 X10 鼠标帧，滚轮仅滚动对话区（每次 3 行），PgUp/PgDn 翻页，键盘 ↑/↓ 独立浏览输入历史；禁用备用屏幕把滚轮转换为方向键的模式。向上查看时保持滚动位置，回到底部后继续跟随新输出。退出或暂停时恢复鼠标模式。`/thinking` 和 `/theme` 不生成模型请求。stdout 重定向仍只写正式回答；`--plain`、非 TTY 和 `--input` 使用逐行显示，默认不打印每次 STATUS，`--verbose-status` 可显式开启。

界面缓存限制为约 1 MiB 文本、128 个条目，满时先淘汰旧条目，再裁掉单个超长当前轮的前部并作标记；磁盘记录和模型历史不受影响。重开或切换时，从本地终态归档恢复已保存的多轮正文，并合并当前 checkpoint，按 request 去重。未存本地归档的会话只有现存 checkpoint 能恢复，不从服务端下载完整聊天。网络空闲期限用单调时钟独立计算，约 34 ms 的 UI 唤醒不刷新该期限。界面缓存与下一轮模型提示构造没有调用通路。

TUI 具有持久的会话循环：`/sessions` 通过回调读取按 ID 排序的本地元数据；`/new` 建立独立 checkpoint；`/switch ID|FILE` 先解析、持锁并加载目标，再释放旧 checkpoint。只有成功切换到另一会话才关闭当前传输，保留 pending 轮次；切回后 `/retry` 从已保存前缀继续。无效、损坏、占用或当前会话目标不打断原生成，删除其他闲置会话也继续接收当前输出。网络或生成错误关闭失败的连接，在对话与状态属性中显示原因，仍接受 `/retry`、`/stop`、新建或切换。切换已提交后若新服务握手失败，停留在目标会话并提供恢复操作。批处理及 `--resume-only` 等调用保留失败退出码。

`/delete [ID]` 只移动受管 `session-XXXXXX`，当前会话先切换到新会话。存储模块先取得该 checkpoint 的原 `flock`，验证目录、锁和 checkpoint 身份，再用 Linux `renameat2(RENAME_NOREPLACE)` 移入 `session_dir/.trash/` 并同步两个父目录；不会 unlink 锁，也不会覆盖已有回收目录。会话被其他进程占用、路径是外部文件或符号链接时拒绝操作。checkpoint、正文归档与锁 inode 一起保存，可移回原目录或显式恢复回收目录中的 checkpoint；服务端会话副本不变。删除当前会话时若软删除失败，已完成的新会话切换不会自动撤销。

SIGINT 在生成时发送 CANCEL、空闲时清草稿；SIGWINCH 重绘；SIGTSTP 先恢复 termios 与滚动区再停止，继续运行后恢复编辑器。正常退出、SIGTERM/SIGHUP 和失败路径均恢复终端。每个信号处理函数只设置标志，在主循环处理；帧传输期间暂缓交互信号，防止半帧丢失。

服务端看板 `scripts/server/dashboard.py` 是独立只读 curses 进程，每秒扫描状态目录和 `/proc/locks`，按会话锁 inode 判断连接/worker。它不获取业务锁、不写会话文件，不参与生成生命周期。文件读取限制长度并拒绝符号链接、FIFO 等特殊文件；指标是带时效的快照，`ctx~` 为当前尝试输入与已生成 token 的估计，不能替代 tokenizer 的准入校验。默认隐藏问题和思考正文。

## 状态文件

```text
state/
├── clients/
│   ├── session-XXXXXX/         # 默认启动或 /new 创建的独立目录
│   │   ├── checkpoint.ckpt    # T4CHECK2：当前轮问题、回答追加记录和终态
│   │   ├── checkpoint.ckpt.lock # 客户端独占 flock；获得锁后记录 PID
│   │   ├── checkpoint.ckpt.steer # 可选：尚未提交的纠偏，绑定 session/old request
│   │   └── checkpoint.ckpt.history/
│   │       └── turn-00000001.ckpt # T4HIST1：仅供显示的终态正文快照
│   └── .trash/session-XXXXXX-<随机后缀>/ # 本地软删除，保留完整目录和原锁
└── model-server/<token>/
    ├── session.lock           # 同一会话仅一个连接
    ├── events.log             # connected/generate/replay/completed/cancelled/disconnected
    ├── context.txt            # C 重建的可读历史，用于核查；不是模型提示词解析源
    └── turn-00000001/
        ├── prompt.txt         # 原始问题，不可改变后重试同一请求编号
        ├── backend.txt        # llama，核对轮次后端标识
        ├── response.txt       # C 层持久保存并发送的正式回答
        ├── generator.log      # 每轮独立 stderr
        ├── model-config.json  # 建立轮次时复制的配置
        ├── model-snapshot.json# 结构化消息、配置、模型/模板标识等快照
        ├── rendered-prompt.txt# 官方 chat template 渲染结果
        ├── raw-generation.jsonl # 先落盘的原始生成片段及完成记录
        ├── thinking.txt       # 从原始流提取的思考文本
        ├── metrics.json       # token 数、首 token 等待、推理吞吐等
        └── done 或 cancelled  # 二者为不同终态；中断时两者都没有
```

新轮次在临时目录写入问题、后端标识、模型配置和空回答，`fsync` 后 `rename` 发布，再同步父目录。会话 token 是 16 个随机字节的小写十六进制表示，目录互不共用。

模型适配器按请求编号读取当前会话之前的 `done` 轮次，将 `prompt.txt`、`response.txt` 组成结构化 user/assistant 消息。忽略取消轮次；C 会话层拒绝越过未终结轮次。`context.txt` 只是可读证据，不用字符串中的 `user:`/`assistant:` 重新推断角色，避免用户文本混淆历史边界。历史中没有思考、状态文本和半段回答。

## 输出与恢复的提交顺序

1. llama-server 输出 SSE 原始文本片段。适配器先把原始片段追加到 `raw-generation.jsonl` 并同步，再解析为 THINK/DELTA。
2. 思考标签可以跨任意 SSE 片段；增量解析器保留未确认的标签前缀，避免将 `</thi` 当正文。初始状态来自实际渲染模板中是否已打开思考段。
3. C 工作进程收到正式回答码点后，先追加 `response.txt` 并 `fsync`，再发送 DELTA。
4. 客户端校验 request、字节偏移和完整 UTF-8，先追加 checkpoint 并同步，再显示。THINK/STATUS/HEARTBEAT 不进入回答 checkpoint。
5. 模型正常 EOS/stop 结束且生成子进程成功退出后，C 层才保存 `done` 并发 END。`limit`、截断或异常 EOF 不算完成。
6. 完整交互 TUI 收到 END/CANCELLED 并保存终态后，将该轮正式问题、回答和终态原子归档到 `<checkpoint>.history/`，供切换和重开界面显示；开始下一轮前也检查旧轮归档。归档不修改服务端模型历史。

因此原始模型 journal 可以领先 C 的回答缓存，C 缓存可以领先客户端，但反向领先属于损坏，应拒绝恢复。重连仍上传同一问题与客户端正文偏移：C 先重放缓存缺口；适配器再从 journal 推导完整回答，核对 C 缓存是其前缀，重放适配器已经落盘但尚未交付的后缀，随后把冻结模板与完整原始生成前缀交给 `/completion` 续写。

若 journal 已包含完成记录，适配器只重放并结束；模型服务不必重新推理。完成记录保存生成量、耗时、首段输出延迟及速度，重放沿用这些统计，不把磁盘回放耗时当作推理时间。统计属于最后一次实际推理尝试，不跨多次 retry 累计。缺少统计的较早记录只使用能恢复的指标，不补造缺失数值。未完成续写需验证模型服务标识与原快照匹配。该恢复保证已交付文本前缀一致，不保存 GPU KV cache、采样器状态或未落盘 token；恢复后的新后缀允许与不中断的运行不同。

原始 journal 的残缺末行可以截去；完整但非法记录不能跳过。客户端 journal 同样只修复不完整尾部，完整记录校验失败则报错。正式回答以 UTF-8 字节计游标，token 数只用于性能和资源指标。终端显示与文件同步无法成为一个原子事务；完整正文以持久文件为准。

## 取消、纠偏和崩溃

`CANCEL` 是显式终态操作，`cancelled` 轮次不再推理。网络断连、SIGTERM、工作进程/模型崩溃没有这个标记，保留恢复资格。取消确认丢失时，重连重放 C 缓存后再次返回 CANCELLED。下一请求编号照常加一，C 和 Python 两层历史构造都会跳过该取消轮。

`/steer TEXT` 先检查“原问题 + User correction + TEXT”没有超过字节上限，写 `<checkpoint>.steer` 并同步，再发送 CANCEL。客户端只在收到旧轮终态后创建新请求 checkpoint；新 checkpoint 落盘后才移除纠偏队列。这样崩溃不会让纠偏静默丢失或重复执行；恢复看到旧轮尚未终结时先取消，看到新轮已经提交时只清除过时队列。

END 与 CANCEL 可能在相反方向同时传输。若 END 先完成，服务端对刚完成轮次的迟到 CANCEL 返回已有 END；客户端识别并忽略上一轮的重复终态。若 CANCEL 先处理，服务端停止子进程并返回 CANCELLED。两种结果都保持同一连接可继续。

## checkpoint 与限制

`T4CHECK2` 头依次保存 magic 行、token 行、`request prompt_length file_lines` 行及原问题字节。追加记录由四个网络序 uint32 组成：type、offset、length、FNV-1a 校验，随后跟正文。DELTA=6 含一个码点；END=7 和 CANCELLED=12 均无正文，对应 `done=1` 和 `done=2`；`done=0` 表示可恢复的未完成轮。

保存返回错误时，磁盘上可能已经有完整或部分新记录，因此使内存中的 journal 追加位置失效。下一次同进程重试从完整内存前缀原子重建 checkpoint，不重复追加不确定的记录。纠偏队列清理失败也走这一恢复路径，避免相同 offset/done 的快速返回跳过清理。故障注入测试覆盖 DELTA、END、CANCELLED 同步失败、部分写、close 失败、替换后目录同步失败及纠偏清理失败。

终态显示归档使用独立 `T4HIST1` 格式，不可作为活动 checkpoint 传入 `--resume`。固定 64 字节头保存 magic、会话 token、request、问题/回答长度、终态、文件行游标与 FNV-1a 校验，随后是完整问题和正式回答。正文最多 16 MiB、问题最多 4096 字节，每会话最多 10000 轮；加载前校验文件大小、属主、单硬链接、普通文件类型和 UTF-8。临时文件同步后以不覆盖的原子重命名发布，同内容重复归档幂等，不同内容或损坏记录明确报错；未发布的临时文件不参与回放。归档没有 THINK、STATUS 或纠偏队列，不被模型适配器读取。

帧总期限用单调时钟计算，部分收发不会刷新同一帧期限。生成等待期间约每秒心跳，排队/慢首 token 不导致默认 10 秒客户端期限误判。客户端交互取消通过 `poll` 唤醒；为防 Ctrl+C 丢弃半帧，SIGINT 在单帧传输期间暂缓，最长受帧期限限制。SIGTERM 仍用于退出并保留断点。

新轮次每次尝试默认输出最多 4096 token（含思考和正文），上下文每槽 16384 token，共 2 槽。C 后端建立新轮时读取当前配置，修改预算无需重启现有模型或课程服务。适配器使用本地 tokenizer 检查资源，不静默截去旧消息。运行时身份只允许容量增加，其余模型、构建和模板字段仍严格匹配；已有未完成轮次仍使用冻结配置，修改默认值不改变其预算。超限会明确报错，TUI 保留该轮与续传入口；本系统暂不实现自动摘要、滑动历史或在现有推理内部直接修改用户消息。两并发槽不承诺每个用户维持单独运行时的 token/s。
