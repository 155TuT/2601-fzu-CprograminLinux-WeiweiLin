# 实验二：进程通信

依据实验指导书 PDF 第 5 页（印刷页码 3）完成，参考 `example/task2/2-3shm-*`、`2-4semshm-*`。采用 **System V 共享内存 + 信号量**，由两个独立启动的 C 程序实现客户端请求、服务器回复。

客户端输入文本，服务器将其中的 ASCII 小写字母转换为大写并回复。每次服务器运行接待一个客户端会话，一个会话支持多轮收发；输入完整的 `quit` 或到达 EOF 后，两端完成退出确认并结束。

## 编译与运行

需要 Linux、GCC 和 GNU Make。在 `homework/task2` 目录执行：

```sh
make
```

在终端 A 中启动服务器：

```sh
./ipc_server
```

看到 `ready; waiting` 后，在终端 B 中进入同一目录，启动客户端：

```sh
./ipc_client
```

依次输入，例如：

```text
hello linux
shared memory + semaphore
quit123
quit
```

前三次分别收到 `HELLO LINUX`、`SHARED MEMORY + SEMAPHORE`、`QUIT123`；最后收到 `bye`，客户端和服务器均返回 `0`。`quit123` 是普通消息，只有完全等于 `quit` 的输入才是退出命令。也可在终端 B 执行：

```sh
printf 'hello linux\nshared memory + semaphore\nquit123\nquit\n' | ./ipc_client
```

单条文本最多 **255 字节**，不包括换行和字符串结束符；空行可发送，UTF-8 中文按原字节保留。超过容量的整行会被丢弃并提示，后续行仍可正常收发。输入用于文本，不支持包含 NUL 字节的二进制数据。

程序默认用 `ftok(".", '2')` 生成键。也可让两个终端传入同一个**现有文件或目录**，便于从不同工作目录连接或隔离多组实验：

```sh
./ipc_server /path/to/existing/key-file
./ipc_client /path/to/existing/key-file
```

以上两条分别在不同终端执行。路径只是生成 IPC 键的依据，不写入通信内容；`ftok` 不能保证全局唯一，创建冲突时程序会报错。

## 验证与清理

```sh
make verify
make clean
```

`make verify` 额外需要 Python 3（仅标准库），执行 9 组集成检查：连续 103 次通信、输入边界、EOF、启动顺序、重复服务器、第二客户端、服务器信号终止、初始化失败后的资源归属等。测试使用独立的临时路径，并检查每组实验的 IPC 对象已删除。实际记录见 `logs/verification.txt`。

`make clean` 只删除可执行文件和目标文件，保留报告、截图与日志。若要切换编译选项，先执行 `make clean`，例如：

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O2 -g -Werror' verify
```

正常输入 `quit` / EOF 会自动回收 IPC 对象；在服务器终端按 Ctrl+C 也会触发清理。客户端异常退出后，本次会话不再接纳新客户端，请结束服务器并重新启动。服务器被 `SIGKILL` 强制终止时无法运行清理代码，需核对启动信息打印的 `shmid`、`semid` 后，用 `ipcs -m`、`ipcs -s` 查看，并只删除本次实验遗留的对象：

```sh
ipcrm -m <本次实验的shmid>
ipcrm -s <本次实验的semid>
```

## 文件说明

| 文件 | 用途 |
| --- | --- |
| `ipc.h` | 共享内存结构、信号量编号和公共声明 |
| `ipc.c` | 信号处理、键生成和 P/V 操作 |
| `server.c` | 创建 IPC 对象、处理请求、回复、退出握手和清理 |
| `client.c` | 读取输入、发送请求、等待回复和退出确认 |
| `Makefile` | 多文件编译、增量构建、验证与清理 |
| `verify.py` | 启动真实 C 程序的集成检查 |
| `report.md` | Markdown 实验报告，包含完整注释代码及结果分析 |
| `screenshots/` | 实际运行的 xterm 白底终端窗口截图 |
| `logs/` | 截图对应输出和验证记录 |

截图沿用实验一的白底黑字 xterm 方式，字体为 `DejaVu Sans Mono`。日志中的 `[exit status: ...]` 是采集程序读取的实际命令退出码。运行时 PID、IPC 标识符和键值可能变化。

提交前填写 `report.md` 顶部的学号、姓名、专业和班级。如转换为 DOCX，请从本目录处理报告并保留 `screenshots/` 相对路径，确认图片已嵌入。
