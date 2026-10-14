# 本机 Qwen3.5-4B 部署与实测

方案 4 使用本机部署的 **Qwen3.5-4B Q5_K_M + llama.cpp CUDA**。模型由常驻的 `llama-server` 加载，C 服务端为各会话启动适配器，通过本机回环 HTTP 流获取思考与正式回答。默认从 `.runtime/model-service.json` 发现实际端口；8081 同时作为兼容实例探测地址。HTTP 兼容接口只用于本机进程通信，没有调用外部模型 API。

## 硬件与选型

2026 年 10 月 14 日检查到的硬件与运行结果：

| 项目 | 本机结果 |
| --- | --- |
| CPU | AMD Ryzen AI 7 H 350，8 核、16 线程 |
| 系统内存 | 约 16 GiB |
| 独立显卡 | NVIDIA GeForce RTX 5060 Laptop GPU |
| `nvidia-smi` 显存 | 8151 MiB，初次检查约 1552 MiB 已占用 |
| Windows NVIDIA 驱动 | 616.56，CUDA compute capability 12.0 |
| WSL GPU 设备 | `/dev/dxg` 可用 |
| llama.cpp 设备枚举 | CUDA0 为 RTX 5060 Laptop，报告 8123 MiB 总量、7033 MiB 空闲 |
| 每槽 16k、两个推理槽 | 加载后 5967 MiB，双槽长输入后 5921 MiB；总显存 8151 MiB |

显卡型号、显存及计算能力由 NVIDIA 工具核实。原始证据见 [model-hardware.txt](../logs/model-hardware.txt)。

现有 CUDA 驱动与运行库已经实测可用，因此优先使用独显，CPU 作为可运行的备用路径。NVIDIA 的 [CUDA on WSL 指南](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)说明，WSL 可以运行兼容的已编译 CUDA 程序，显卡驱动由 Windows 提供。本项目只解包本地运行库，不安装 Linux 显卡驱动、不修改系统目录、不重启 WSL。

Q4_K_M 与 Q5_K_M 都能在本机运行。Q5 文件比 Q4 多约 0.40 GB，16k 双槽实测约有 2.1 GiB 显存余量，因此采用 Q5。这个选择依据是精度、速度与显存的折中；本次没有进行回答质量基准，不宣称 Q5 在具体题目上必然优于 Q4。

## 固定来源、版本和校验

原模型为 [Qwen 官方 Qwen3.5-4B](https://huggingface.co/Qwen/Qwen3.5-4B)，量化文件采用 [Unsloth 原始 Qwen3.5-4B-GGUF](https://huggingface.co/unsloth/Qwen3.5-4B-GGUF)，没有使用修改回答行为的微调模型。原模型卡采用 Apache-2.0 许可证。本实验只处理文本，不下载视觉投影文件。

| 组件 | 固定版本 |
| --- | --- |
| 官方原模型参考 revision，未下载 BF16 | `851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a` |
| GGUF revision | `e87f176479d0855a907a41277aca2f8ee7a09523` |
| llama.cpp 发行关联 | 官方 `v0.6.0` 指向二进制版本 `b11429` |
| llama.cpp 源码 commit | `d81235049384534c167caea52b85a694f6103d14` |
| 实际版本输出 | `0.6.0-dev (build 11429, commit d81235049)` |
| CUDA 二进制与运行库 | 官方 Ubuntu x64 CUDA 13.4 发布包 |

二进制来源为 [llama.cpp b11429 官方发行版](https://github.com/ggml-org/llama.cpp/releases/tag/b11429)。安装脚本固定 URL 与 SHA256，模型校验值来自固定 revision 的 Hugging Face LFS 元数据，运行库校验值来自 GitHub 官方发行资产：

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `Qwen3.5-4B-Q4_K_M.gguf` | 2740937888 | `00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4` |
| `Qwen3.5-4B-Q5_K_M.gguf` | 3143656608 | `8814232b85594dcd46c50e5b8b29324a7efe9e746edbe8a3d1df3d3fce7aad39` |
| `llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz` | 152519318 | `8082b7eaa74a714c9fecca19128f751c8e32da763ee8096b8ad1e824da7621d3` |
| `cudart-llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz` | 440236630 | `93d18648d815b2bd624d83d82f653e1db97afb478f02064305fe3cf570040a6d` |

本机四个文件都通过了校验；安装记录保存在 `.runtime/metadata/installation.json`。`.runtime/` 不进入源代码提交，提交包通过脚本重新取得同一组文件。

## 安装与三个终端运行

在 `homework/task4` 目录执行，需要 Python 3.12、curl、GCC 和 GNU Make。首次下载默认约 3.74 GB，包括 Q5 与两个压缩运行包；解包后另占磁盘空间：

```sh
python3 scripts/model/setup.py
make
```

脚本只写 `.runtime/`。下载中断后重复命令按字节续传，完成后验证大小与 SHA256；重复安装采用暂存解包与逐文件原子替换，避免原地改写运行中的库文件。需要对照 Q4 时执行：

```sh
python3 scripts/model/setup.py --quant Q4_K_M --quant Q5_K_M
```

终端 A 启动持久模型服务：

```sh
bash scripts/model/serve.sh gpu
```

启动器会输出实际地址或已验证实例的复用地址，不需要固定 8081。默认 `endpoint: "auto"` 根据运行记录连接；端口 0 由 llama-server 真正绑定时分配，避免先探测端口再释放的竞态。启动参数由 [config/model.json](../config/model.json) 和脚本共同确定：默认 Q5，GPU 层数 99，8 个 CPU 线程，flash attention 开启，2 个推理槽，总上下文 32768 token，关闭统一 KV 分配。实测 `/props` 和 `/slots` 确认每槽 `n_ctx=16384`；32768 是两槽总配置，单会话容量为 16k。

容量验证让两槽同时处理各 5529-token 提示，分别在 3.768 / 4.040 秒以 EOS 完成，未截断上下文，显存为 5921 / 8151 MiB。该检查直接调用模型接口，验证长输入在当前配置下可用；C 协议仍限制单轮问题最多 4096 UTF-8 字节，多轮累计历史共享 16k 窗口。日志见 [model-context-16k.txt](../logs/model-context-16k.txt)。该项容量测试的输出预算为 2048 token；默认新轮预算为 4096 token，已有未完成轮沿用其冻结预算。上下文或输出预算增大均不能保证消除长思考循环。

没有可用 NVIDIA GPU 时，在终端 A 换为：

```sh
bash scripts/model/serve.sh cpu
```

CPU 模式在该进程内隐藏 CUDA 设备并设置 GPU 层数为 0，仍使用本地同一 GGUF。结束模型服务在其终端按 Ctrl+C；它独立于 C 服务端，需要单独启动与结束。

服务列表可同时保存 CPU 与 GPU 实例，退出一个实例后其余实例仍可被发现。推理端按列表顺序选择配置匹配且健康的服务，可用显式 URL 指定实例。CPU 模式只复用启动记录能确认的 CPU 实例，不会把已有 GPU 实例作为 CPU 测试结果。GPU 模式跳过已确认的 CPU 实例，但允许复用模式未知的兼容外部服务；这种复用不能作为 GPU 已启用的证据。`--port N` 可指定模型固定端口；若配置中已写明 URL，则两者端口必须一致。日常使用保留 `endpoint: "auto"` 即可。

若启动器提示复用了已有服务，本次命令没有创建新的模型进程，退出该命令也不会停止原实例。自动端口的连接与恢复见 [README 的服务发现说明](../README.md#自动端口与服务发现)。课程服务端向适配器提供当前连接配置，旧轮仍使用冻结的采样/预算和原始前缀，只有网络端点跟随运行记录变化。

终端 B 启动 C 服务端：

```sh
./bin/stream_server --backend llama --model-config config/model.json --state state/model-server
```

终端 C 启动 C 客户端：

```sh
./bin/stream_client
```

再开一个终端时仍用同一条命令，客户端自动分配独立 checkpoint 并显示路径。使用 `--list-sessions` 查找旧会话、`--resume FILE` 恢复。多轮问题、思考展示、停止、转向与恢复操作见 [README](../README.md)。

真实模型检查命令为：

```sh
python3 scripts/model/smoke.py
```

该脚本要求本地模型服务已健康，启动真实 C 服务端与两个真实 C 客户端，使用独立临时状态，各执行两轮“记住代号、回忆代号”，然后核对不同 token、当前轮 checkpoint、回答和上下文隔离。模型服务保持运行。原始结果写入 `logs/model-smoke.txt`。

本机这次检查在 15.530 秒内通过：两个客户端都完成两轮、返回 0，各自回忆出 `MAPLE` 与 `CEDAR`，不同 token 的会话上下文没有混入对方代号。正式回答包含模板分隔后的两个 LF，checkpoint 分别保存 `\n\nMAPLE` 与 `\n\nCEDAR`，都是 7 字节。完整记录见 [model-smoke.txt](../logs/model-smoke.txt)，白底实测窗口见 [多轮与隔离截图](../screenshots/09-model-smoke.png)。

## 实测速度与范围

原始数据由固定版本 `llama-bench` 生成，GPU 使用 8 线程、flash attention、128 个提示 token 与 64 个生成 token，各重复 3 次。CPU 隐藏 CUDA 设备，仅做 32 个提示 token 与 16 个生成 token 的一次短样本：

| 配置 | 提示处理 token/s | 生成 token/s |
| --- | ---: | ---: |
| RTX 5060 / Q4_K_M | 2069.65 ± 314.32 | 86.34 ± 1.57 |
| RTX 5060 / Q5_K_M | 2378.86 ± 421.52 | 80.52 ± 0.52 |
| CPU / Q5_K_M，短样本 | 131.71 | 14.78 |

Q4 的短生成吞吐约比 Q5 高 7.2%。提示处理样本的方差较大，不能根据这两行断言 Q5 提示处理更快；CPU 一次短样本也不能代表长上下文、持续负载或并发速度。两个量化均在加载的 Q5 服务空闲时测量，推理请求未同时争用 GPU。原始记录见 [Q4 GPU](../logs/model-q4-gpu-raw.txt)、[Q5 GPU](../logs/model-q5-gpu-raw.txt)、[CPU](../logs/model-q5-cpu-only-raw.txt)，摘要见 [model-performance.txt](../logs/model-performance.txt)。

可用相同参数重复短 GPU 基准。先保证模型服务没有正在处理的请求，再执行：

```sh
TASK4_BENCH_LIB="$PWD/.runtime/llama/llama-b11429:$PWD/.runtime/llama/cudart-llama-b11429-bin-ubuntu-cuda-13.4-x64:/usr/lib/wsl/lib"
LD_LIBRARY_PATH="$TASK4_BENCH_LIB" .runtime/llama/llama-b11429/llama-bench \
  -m .runtime/models/Qwen3.5-4B-Q5_K_M.gguf -t 8 -p 128 -n 64 -r 3 -ngl 99 -fa 1 -o json
```

将模型文件名换为 Q4 即可使用同一参数对照。CPU 短样本增加 `CUDA_VISIBLE_DEVICES=''`，并使用 `-p 32 -n 16 -r 1 -ngl 0`，避免把 GPU 计算混入 CPU 对照。

这些数值使用模型 tokenizer 的 token 计数，**不是中文字符/秒**。真实端到端速度还受问题长度、思考长度、并发、磁盘同步、C 协议逐码点转发与终端绘制影响。适配器分别记录首段输出时间、累计 token 和模型返回的生成计时，不用收到的 UTF-8 字节数冒充 token 数。

归档的白底 xterm 窗口展示本次实际日志：[硬件与运行库](../screenshots/07-model-hardware.png)、[量化及 CPU/GPU 性能](../screenshots/08-model-performance.png)。采集时原样显示归档日志，长行由终端自动换行；仅截取本次采集程序自行创建的窗口。

## 思考、完成与恢复语义

官方 [Qwen3.5-4B 模型卡](https://huggingface.co/Qwen/Qwen3.5-4B)说明默认采用思考模式。本项目默认 `thinking=true`、最大新生成 4096 token（思考与正文共用），采用 `temperature=1.0`、`top_p=0.95`、`top_k=20`、`min_p=0.0`、`presence_penalty=1.5`。通过本机 `/apply-template` 冻结本轮聊天模板，思考开启时模板尾部已经包含 `<think>`；原始生成流负责输出思考正文、`</think>` 和正式回答。C 服务端在新轮开始时重新读取配置，预算调整不需要重启模型或服务端；旧轮快照不回写。

实测原始 SSE 在 `special=false` 下保留 `</think>`，而不把会话控制 token 当作正文输出；最后一条包含 `stop=true`、`stop_type`、token 计数及 `timings`。闭合思考与正式回答样本见 [model-api-complete-probe.txt](../logs/model-api-complete-probe.txt)。一次 512 token 的短预算在思考阶段触发 `limit`，这种情况不能标记为成功完成，状态与原始生成前缀会保留，供继续或停止。

适配器将原始流先追加到 `raw-generation.jsonl` 并同步，再解析为独立的 THINK 与 DELTA。THINK 有自己的偏移、文件和展示区域，不推进正式回答的 checkpoint；只有正式回答写入 `response.txt` 并用于后续完成轮次的会话历史。不同客户端不共用聊天历史。

断线恢复先重放已经保存的正式回答后缀，再用**冻结的提示模板＋已经保存的原始生成文本**作为输入，从前缀之后续写。不会重新生成旧内容然后按字节跳过，也不会把已显示的前缀改成另一个采样结果。模型、构建、模板变化或运行时容量缩小时拒绝直接续写；仅容量增加可以继续使用旧冻结快照，旧轮预算仍保持原值。

这里的恢复保证已持久化文本前缀连续；没有保存推理进程的 KV cache 或采样 RNG 状态，后续新生成 token 不保证与一条从未中断的运行逐 token 相同。续写还要重新检查 tokenizer 长度与本轮冻结容量（默认新轮为 16384）；超出容量或缺少合法结束标记会保留断点并报告原因。详细帧及状态规则见 [协议说明](protocol.md)，后端职责见 [架构说明](architecture.md)。


## 上下文、指标与资源边界

新轮次的消息仅来自本会话之前已完成轮次的 `prompt.txt` 和 `response.txt`，按 user/assistant 成对排列，最后附加当前问题。取消轮次、思考、STATUS 和其他会话不加入这些消息。恢复未完成轮次时则使用冻结提示词与该轮原始生成前缀，前缀可能包含尚未闭合的思考；这是继续同一轮的输入，不是下一轮聊天历史。

| 限制 | 当前默认及含义 |
| --- | --- |
| `context_tokens` | 每槽 16384 token；模板、历史、当前问题和生成共同占用 |
| `max_tokens` | 每次尝试最多 4096 个新 token，思考和正式回答共用；实际预算还受剩余上下文限制 |
| 客户端 `--idle-ms` | 默认 10000 ms；网络等待期限，和模型 token 预算独立 |

适配器用本机 `/tokenize` 检查输入长度，显式启用 `add_special=true`、`parse_special=true`；实际生成预算取配置上限与剩余上下文减去 8-token 余量后的较小值。

`stop_type=limit` 表示本次新生成达到预算，不等于历史占满上下文。判断资源状态时结合 `prompt_tokens`、`context_tokens`、`attempt_max_tokens`、结束事件的 `truncated` 字段及错误信息中的 `context_truncated`。未闭合思考或没有合法完成事件的输出不写 `done`；已持久化前缀保留，可继续或取消。达到输出预算后重试仍使用该轮冻结设置，也会再次消耗剩余上下文。

`prompt_progress` 事件的占位 `tokens=[0]` 不计为生成 token，真实空文本生成 token 仍计数。有效整数 `tokens_predicted` 用于原生累计计数；终止事件具有有效整数 `timings.predicted_n` 时，以它校准正常结束和 `limit` 结束的统计。指标仅供客户端和看板显示，不反馈到模型提示词。相关检查见 [当前验证记录](../logs/verify-current.txt)。

当前没有独立思考预算、重复检测、自动转为直接回答、自动摘要或工具执行能力。较短问题仍可能产生长思考；增大输出预算不能保证消除重复。系统未给模型添加用于约束应用能力和元信息的系统提示，模型有关“见过哪些对话”“能否执行代码”或知识截止日期的自述不作为程序状态或事实依据。固定种子的局部模型测试只覆盖相应输入，不能证明所有问题都不会循环。

安装时的 SHA256 用于校验固定下载文件。续写时检查的是服务上报的模型路径、模板、构建、上下文容量，以及路径可访问时的文件大小和修改时间；不会每轮重新计算 GGUF 的完整 SHA256。这些检查用于配置一致性，不能视为对运行中模型内容的密码学认证。恢复、帧期限及终态详见 [架构](architecture.md) 和 [协议](protocol.md)。
