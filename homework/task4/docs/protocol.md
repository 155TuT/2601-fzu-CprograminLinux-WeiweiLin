# TCP 流式文本协议

协议常量与帧结构声明于 `include/task4/protocol.h`，收发实现位于 `src/net/protocol.c`。所有套接口使用非阻塞模式、close-on-exec 和 `TCP_NODELAY`；收发通过 `poll` 等待就绪并循环完成部分字节。

## 帧格式

每帧由 20 字节固定报头和 `length` 字节正文组成。报头是五个 32 位无符号整数，均使用网络字节序，不直接发送 C 结构体：

| 字节偏移 | 字段 | 说明 |
| --- | --- | --- |
| 0 | `magic` | `0x54345331`，按字节表示 ASCII `T4S1` |
| 4 | `type` | 消息类型，见下表 |
| 8 | `request_id` | 当前会话轮次编号；握手和 ERROR 为 0 |
| 12 | `offset` | 随类型解释的字节偏移 |
| 16 | `length` | 本帧正文长度，最大 4096 |
| 20 | `data` | 原始正文；长度不包含报头或字符串结束符 |

帧层可以传输原始字节，上层按类型限制正文。例如提问禁止 NUL，回答必须是一个完整且合法的 UTF-8 码点。内部额外写入的 C 字符串结束符不在网络传输中。

`t4_recv` 返回 1 表示取得完整一帧，0 只表示帧边界上的正常 EOF，-1 表示错误。报头或正文中途 EOF 属于截断错误；magic 错误、正文超长或状态不符都会拒绝处理。TCP 的拆包与合包不会改变帧语义。

## 类型及字段约定

| 类型值 | 名称 | 方向 | request / offset / data |
| --- | --- | --- | --- |
| 1 | `HELLO` | 客户端 → 服务器 | request=0、offset=0；首次连接正文为空，恢复时正文为 32 字节会话 token |
| 2 | `WELCOME` | 服务器 → 客户端 | request=0、offset=0；正文为 32 字节小写十六进制 token |
| 3 | `BEGIN` | 客户端 → 服务器 | request 为本轮编号，offset 为已保存的回答字节数；正文为空 |
| 4 | `INPUT` | 客户端 → 服务器 | request 与 BEGIN 相同，offset 为本轮问题已传字节数；正文为非空输入片段 |
| 5 | `COMMIT` | 客户端 → 服务器 | request 与 BEGIN 相同，offset 为本轮问题总字节数；正文为空 |
| 6 | `DELTA` | 服务器 → 客户端 | request 为本轮编号，offset 为该码点之前的回答字节数；正文为一个 UTF-8 码点，1–4 字节 |
| 7 | `END` | 服务器 → 客户端 | request 为本轮编号，offset 为完整回答字节数；正文为空 |
| 8 | `ERROR` | 服务器 → 客户端 | request=0、offset=0；正文为错误说明，连接随后结束 |
| 9 | `HEARTBEAT` | 服务器 → 客户端 | request 为本轮编号，offset 为目前已发回答字节数；正文为空 |
| 10 | `THINK` | 服务器 → 客户端 | 本轮编号，offset 为独立思考字节游标，正文为完整 UTF-8 片段，最多 4096 字节 |
| 11 | `CANCEL` | 客户端 → 服务器 | 本轮编号，正文为空；offset 可携带客户端正文游标，服务端以自己的持久长度确认 |
| 12 | `CANCELLED` | 服务器 → 客户端 | 本轮编号，offset 为取消时的持久正文长度，正文为空；可继续下一轮 |
| 13 | `STATUS` | 服务器 → 客户端 | 本轮编号，offset 为当前正文长度，正文为状态或统计；不推进游标 |
| 14 | `PROBE` | 客户端 → 服务器 | 仅 HELLO 前可选一次；request=0、offset=0、正文为空 |
| 15 | `SERVICE` | 服务器 → 客户端 | 对 PROBE 的回答；request=0、offset=0、正文为本次主服务的 32 字节实例标识 |

`BEGIN.offset` 与 `INPUT.offset` 使用不同的坐标：前者属于回答恢复游标，后者属于提问拼接位置。`COMMIT.offset` 也属于提问位置。恢复时仍须从问题 offset=0 重新发送原问题，不能用回答游标替代问题偏移。

## 首次握手与连续提问

自动发现地址的客户端先在同一连接发送 `PROBE`，核对 `SERVICE` 与本地有效租约记录中的实例标识，再发送 HELLO。PROBE 不建立会话、不写历史，也不获取会话锁。若实例不一致，客户端结束连接，不发送 HELLO；端口变化后需重新发现。显式 `--host/--port` 及已有客户端仍可直接 HELLO，下面主流程保持兼容。

```text
客户端                                      服务端
HELLO(0, 0, empty)                         → 创建 token 与独立会话目录
                                          ← WELCOME(0, 0, token)
保存 token
BEGIN(1, 0, empty)                         →
INPUT(1, 0, first bytes)                   →
INPUT(1, n, remaining bytes)               → 拼接，检查连续偏移
COMMIT(1, prompt_length, empty)            → 保存问题，固定模型配置
                                          ← DELTA(1, 0, first character)
持久化、显示                              ← DELTA(1, width, next character)
…                                         ← HEARTBEAT(1, current_offset, empty)
…                                         ← DELTA(1, current_offset, character)
                                          ← END(1, answer_length, empty)
标记本轮完成
BEGIN(2, 0, empty)                         → 开始下一轮
```

问题总长度最多 4096 字节。当前 C 客户端每个 INPUT 最多 128 字节；服务器也接受其他大小的合法非空片段，要求总长度不超限、offset 严格连续、request_id 不变。空问题用 `BEGIN → COMMIT(offset=0)` 表示，不发送空 INPUT。

服务器只有收到合法 COMMIT 后才保存问题和开始生成。一次连接按轮次顺序处理，一轮 END 或 CANCELLED 后才能提交下一轮；不同连接可同时处于任意生成阶段。`/quit` 是客户端本地完整行命令，不作为提问发给服务器；生成期间先发送 CANCEL。客户端 EOF 结束连接并保留恢复资格。

服务器逐码点落盘并发送 DELTA。客户端要求 DELTA 的 request_id 等于当前轮次、offset 等于本地已经保存的回答字节数，验证 UTF-8 后追加保存，随后显示。收到合法 END 后保存完成标记。HEARTBEAT 使用同样的 request/offset 校验，但不增加游标，也不显示或写入回答。

## 恢复流程

```text
HELLO(0, 0, saved_token)
WELCOME(0, 0, same_token)
BEGIN(saved_request, durable_answer_offset, empty)
INPUT(saved_request, 0, original_prompt_part_1)
INPUT(saved_request, n, original_prompt_part_2)
COMMIT(saved_request, original_prompt_length, empty)
DELTA…   # 从客户端游标重放服务端已经保存的后缀
DELTA…   # 未完成时，从服务端持久长度继续生成
END(saved_request, complete_answer_length, empty)
```

会话 token 是恢复会话的持有者凭据，格式为 32 位小写十六进制。未知 token、正在被另一个连接持锁的 token 或存储错误会拒绝握手。客户端也锁定 checkpoint，避免两个进程同时改写同一恢复记录。该协议用于本机实验；没有 TLS 和独立用户认证，不宜直接用于公网服务。

服务端必须核对原问题与保存的 `prompt.txt` 完全一致，回答恢复偏移不得超过服务端已有 `response.txt` 长度，并须符合完整 UTF-8 码点边界。新轮次编号从 1 顺序递增，最多 10000；开始下一轮之前，其之前的轮次必须已完成或已取消。

客户端保存游标可能落后于服务器：服务端已写文件但 DELTA 未到客户端时，重连从客户端较小的游标重放即可。模型适配器核对服务端缓存与原始生成前缀，再从冻结提示词和已有原始输出续写。若此前生成已经完成，则只重放缓存并发送 END。

## 帧期限与生成心跳

一次 `t4_send` 或 `t4_recv` 用 `CLOCK_MONOTONIC` 建立一个期限，报头与正文共享它，部分传输不会重置计时。服务器帧期限默认 600000 ms，客户端默认 10000 ms；客户端参数下限 1500 ms，为约每秒一次的空闲心跳留出时间。

工作进程等待生成器时，同时 poll 生成连接和客户端。约 1 秒没有可读生成数据时，向该客户端发送 HEARTBEAT。客户端收到合法完整的 THINK、DELTA、STATUS 或 HEARTBEAT 后刷新网络等待期限，因此一段低速回答可以长于 `--idle-ms`。这里没有“整轮回答必须在固定秒数内完成”的限制。

客户端以单调时钟独立计算网络空闲期限，键盘输入和界面刷新不延长期限。到期时先做不阻塞的网络就绪检查，处理已经到达的帧；对端只发送部分报头或正文时仍受完整帧期限约束。`Connection timed out` 表示 I/O 等待到期，与模型的 `stop_type=limit` 或上下文容量不足分开处理。TUI 报错后保留当前会话并支持 `/retry`，批处理以失败退出码报告。

服务端等待下一轮 BEGIN 或正在收集 INPUT 时仍受帧期限约束；心跳仅发生在生成等待阶段。若生成器失败、给出非法 UTF-8、输出超过 16 MiB，或网络/存储失败，工作进程停止并回收生成器，保留已有恢复状态；可以在服务恢复可用后用原 checkpoint 重试。

模型 token 的计数及其生成状态属于后端职责，正式回答 offset 始终按已保存 UTF-8 字节计数，不能直接用 token 数替换。THINK 使用独立字节游标，客户端允许恢复后的第一个 THINK 从非零偏移开始，之后要求连续；它只供界面显示，逐行模式写 stderr，不写回答 checkpoint。STATUS 同样仅供显示。

## 取消和模型事件

```text
COMMIT(r) → THINK… / DELTA…
CANCEL(r) → 停止并回收生成进程 → fsync cancelled → CANCELLED(r, durable bytes)
BEGIN(r+1) → 新一轮；上下文排除 r 的部分回答和思考
```

取消可以发生在思考、回答、等待首 token 或排队阶段。服务端在生成状态只接受匹配本轮编号、无正文的 CANCEL，其他上行帧视为协议错误。客户端在等待确认时仍处理已在途的 DELTA，并以终态确认的偏移核对持久长度。

若 END 已发出而 CANCEL 才到达，READY 状态对最近终结请求返回原 END/CANCELLED。客户端忽略上一轮的重复终态，避免晚到确认破坏下一轮。取消确认丢失后，使用原请求恢复会返回持久 CANCELLED，不重新生成。

模型适配器向 C worker 的 socketpair 也发送此 20 字节帧，request_id 固定为 0，由 worker 验证并替换成实际轮次编号。只允许 DELTA、THINK、STATUS、END、ERROR。模型 END 还须结合子进程成功退出才成为对客户端的 END。适配器 EOF、token limit、截断不能冒充 END。

原始生成与正式回答之间的恢复顺序见 [架构说明](architecture.md)。模型恢复用原始前缀续写。协议并不声称恢复了 GPU KV cache 或随机采样状态。
