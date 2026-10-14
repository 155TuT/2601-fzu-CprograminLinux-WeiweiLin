# 依赖与许可证

| 用途 | 依赖 | 管理位置 |
| --- | --- | --- |
| C 主体与 TUI | Linux 5.3+（pidfd）、GCC、GNU Make、glibc/POSIX、支持 ANSI 的 UTF-8 终端 | 自制 termios/ANSI 界面，无额外链接库 |
| 客户端状态解析 | jsmn，MIT 许可证，commit `25647e692c7906b96ffd2b05ca54c097948e879c` | `deps/jsmn/`，只用于显示元数据 |
| 服务端看板 | Python 标准库 curses（Linux Python 的 ncurses 绑定） | `scripts/server/dashboard.py` |
| 本地模型适配、验证 | Python 3.12 标准库（含安装脚本） | `src/model/`、`tests/` |
| 推理引擎 | 固定 llama.cpp b11429，官方 Ubuntu CUDA 13.4 发布包 | `.runtime/llama/` |
| CUDA 用户态库 | 同版本官方 cudart 发布包 | `.runtime/llama/`，启动脚本设置库路径 |
| 模型 | 原始 Qwen3.5-4B 的 Q5_K_M GGUF（Unsloth 量化） | `.runtime/models/` |
| 文档证据 | xterm、Pillow/python-xlib、PyMuPDF 等临时工具 | `/tmp`，不影响运行或测试 |

安装与文件哈希固定在 `scripts/model/setup.py`，同时核对下载文件大小和 SHA256；支持已有下载前缀按字节续传。首次下载访问 GitHub/Hugging Face，正常推理只访问本机回环接口。不会安装系统 NVIDIA 驱动，也不向外部模型服务发送问题。

llama.cpp 使用 MIT 许可证；Qwen3.5-4B 模型卡声明 Apache-2.0。模型上游与量化仓库信息、固定 revision、来源链接见 [本地模型说明](../docs/local-model.md)。`.runtime/` 不纳入源码提交，重新安装按固定来源取得相同文件。分发模型或运行时应同时遵守对应上游许可证并保留其声明。

`include/task4/` 只包含本项目公共头文件。系统头文件由编译器读取，不复制系统库或推理引擎实现到课程源码。没有 torch、Transformers、CUDA 编译器或第三方 Python 包的运行依赖。默认 CPU 模式仍复用本次官方分发包，在未取得运行库时请先执行安装脚本。

jsmn 来自 [官方仓库](https://github.com/zserge/jsmn)，头文件、MIT 声明及固定来源/哈希分别保存为 `jsmn/jsmn.h`、`jsmn/LICENSE`、`jsmn/source.json`。TUI 没有引入 OpenCode 的运行环境；仅参考其思考可折叠、底栏和会话式交互。截图采集用的临时 X11 工具也不是用户运行客户端的依赖。
