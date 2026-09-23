# 实验三：网络编程及文件操作

依据实验指导书 **PDF 第 6 页（印刷页码 4）** 完成，参考 `example/task3/3-1tcp-*`、`3-2tcp-*` 和 `3-3fd.c`、`3-4stream.c` 等文件操作示例。使用 IPv4 TCP 套接口实现双向通信，使用 `FILE *` 数据流读取待发送文件、保存回复和服务器收发记录。

客户端逐行读取文件，服务器将 ASCII 小写字母转换为大写后回复。一次服务器运行接待一个客户端连接，一个连接可处理多行文本；输入文件读完后，两端自动结束。`quit` 也是普通文本，结束条件是文件 EOF。

## 编译与运行

需要 Linux、GCC 和 GNU Make。在 `homework/task3` 目录执行：

```sh
make
```

终端 A 启动服务器：

```sh
./tcp_server
```

看到 `listening on 127.0.0.1:3339` 后，在同一目录的终端 B 执行：

```sh
./tcp_client
```

客户端读取自带的 `data/messages.txt`，生成 `output/replies.txt`；服务器追加写入 `output/server-records.txt`。预期回复为：

```text
HELLO LINUX
TCP SOCKET + FILE IO
GOODBYE, TASK3!
```

两端正常返回 `0`。查看保存的文件并比较内容：

```sh
cat output/replies.txt
cat output/server-records.txt
tr 'a-z' 'A-Z' < data/messages.txt | diff -u - output/replies.txt
```

`diff` 没有输出且退出码为 `0`，表示保存的回复与预期一致。本次实测文件的归档副本为 `logs/sample-replies.txt` 和 `logs/sample-server-records.txt`。

**结果文件必须尚不存在。** 客户端使用 `fopen(..., "wx")`，防止覆盖旧结果或误把输入文件截断。再次实验时重新启动服务器，并换一个结果文件名，例如：

```sh
./tcp_client 127.0.0.1 3339 data/messages.txt output/replies-2.txt
```

服务器日志采用追加模式，会保留以前的会话。程序不自动创建文件的父目录；自带的 `output/` 目录可直接使用。

## 参数、消息与文件格式

```text
./tcp_server [port [log-file [bind-ipv4]]]
./tcp_client [host [port [input-file [new-output-file]]]]
```

服务器端口允许 `0`，此时由系统分配空闲端口，实际端口打印在启动信息中；客户端必须使用实际的 `1..65535` 端口。默认监听 `127.0.0.1`。客户端同时支持 IPv4 地址和能解析为 IPv4 的主机名，例如 `localhost`。

如需在两台计算机上实验，服务器可执行 `./tcp_server 3339 output/server-records.txt 0.0.0.0`，客户端把地址换成服务器实际的局域网 IPv4 地址，并确保该端口可达。本次已验证本机 `127.0.0.1` 和 `localhost`，没有进行跨计算机实测。

文本以 LF 换行分隔，一行最多 **1024 字节**，不含换行和字符串结束符。空行可以发送；UTF-8 中文等非 ASCII 小写字节保持原样；文件最后一行没有换行也会发送。结果文件为每条有效回复补一个 LF。超过长度或包含 NUL 的整行会被跳过，客户端输出警告并统计跳过行数，后续有效行继续处理。

TCP 协议使用“4 字节网络字节序长度＋正文”，长度不包含报头和字符串结束符。长度 `0` 表示空文本，EOF 表示连接发送方向关闭，两者不同。收发函数循环处理部分收发和 `EINTR`，并拒绝超长帧、含 NUL 的帧和中途截断的帧。

程序返回 `0` 表示有效行的收发与保存完成（可能有已提示的跳过行）；文件、连接或协议出错返回 `1`，参数错误返回 `2`。出错时结果文件可能为空或只包含已经收到的完整回复，保留该文件便于检查；再次运行应换用新文件名。

## 验证与清理

```sh
make verify
make clean
```

`make verify` 额外需要 Python 3（仅标准库），使用临时文件和回环地址上的临时端口执行 **12 组集成检查**，包括连续 103 次收发、文件内容比对、拆分/合并帧、输入边界、EOF、异常回复、端口冲突、文件保护及写入失败。运行记录见 `logs/verification.txt`。

切换编译选项前先清理，例如：

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O2 -g -Werror' verify
```

`make clean` 只删除可执行文件和目标文件，保留输入、输出、报告、图片与日志。服务器等待连接或等待未完成的消息时会阻塞，可在其终端按 Ctrl+C 结束；本实验未实现并发服务和应用层超时。已保存的记录逐条 `fflush`，进程退出时内核回收套接口描述符。

## 文件说明

| 文件 | 用途 |
| --- | --- |
| `net.h`、`net.c` | 长度协议、完整收发、文件行读取、地址解析和端口检查 |
| `server.c` | 监听、接收文本、转换回复、追加保存收发记录 |
| `client.c` | 读取输入文件、发送请求、接收并保存回复 |
| `data/messages.txt` | 默认实验输入 |
| `output/` | 运行时产生的回复和服务器记录，不纳入版本控制 |
| `Makefile` | 多文件编译、增量构建、验证与清理 |
| `verify.py` | 启动真实 C 程序的集成检查，不参与 C 程序运行 |
| `report.md` | 与前两个 task 同格式的实验报告，含完整注释源码与分析 |
| `screenshots/` | 6 张实际 xterm 白底黑字窗口截图 |
| `logs/` | 截图对应输出、文件结果归档及验证记录 |

截图字体沿用 `DejaVu Sans Mono`，`[exit status: ...]` 由采集程序读取真实命令退出码后打印。PID 和客户端临时端口随运行变化。

提交前填写 `report.md` 顶部的学号、姓名、专业和班级。转换为 DOCX 时，从本目录处理报告并保留 `screenshots/` 的相对路径，确认图片已嵌入。
