# 实验一：进程管理

依据实验指导书 PDF 第 3、4 页（印刷页码 1、2）完成。一个多文件 C 程序同时使用 Makefile、`fork`、`execvp`、`sigaction`，并通过 `waitpid` 回收子进程。

## 编译与运行

在本目录执行，需要 Linux、GCC 和 GNU Make：

```sh
make
./process_demo
```

程序约 3 秒后自动结束。父进程执行 2 步任务，子进程执行 3 步任务，然后发送 `SIGUSR1` 并执行默认命令 `ls -l`。父进程接收通知、等待并打印子进程的终止状态。`make run` 也可运行默认演示。

可以指定子进程要执行的命令及参数：

```sh
./process_demo /bin/sh -c 'printf "exec: pid=%s, ppid=%s\n" "$$" "$PPID"'
./process_demo /definitely-missing-task1-command
echo $?
```

第一个命令用于核对 `exec` 前后 PID 不变；第二个命令用于观察 `execvp` 失败，退出码应为 `127`。除显式传入 `/bin/sh -c` 外，程序直接执行命令，不解释管道或重定向等 Shell 语法。程序自身的退出码会反映子进程的退出状态；若子进程被信号终止，则返回 `128 + 信号编号`。

```sh
make clean
```

以上命令只删除可执行文件和目标文件，保留代码、报告、图片和日志。

## 文件说明

| 文件 | 用途 |
| --- | --- |
| `main.c` | 注册信号处理、创建子进程、等待通知、回收子进程 |
| `process.c` | 并发任务演示、信号发送、执行目标命令 |
| `process.h` | 两个源文件共享的函数声明 |
| `Makefile` | 分别编译、链接、增量构建、运行与清理 |
| `report.md` | Markdown 实验报告，包含完整注释代码、结果分析和截图 |
| `screenshots/` | 实际 xterm 白底终端截图 |
| `logs/` | 截图对应的实际输出和其他验证记录 |

提交前填写 `report.md` 顶部的学号、姓名、专业和班级。转换为 DOCX 时，从本目录处理报告并保留 `screenshots/` 的相对路径，确认图片已嵌入文档。运行时 PID、输出先后顺序和目录列表可能与报告截图不同，这是正常现象。
