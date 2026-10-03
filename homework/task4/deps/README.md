# 依赖

运行与编译只使用 Linux、GCC、GNU Make 和 glibc/POSIX 系统接口，不需要下载 C 第三方库，不连接任何模型 API。验证脚本额外使用 Python 3 标准库。

`include/task4/` 是本项目自己的公共头文件；系统头文件由编译器从系统目录读取，不把系统库复制到仓库。Makefile 用 `-MMD -MP` 生成 `build/` 下的头文件依赖关系。

报告截图使用临时安装于 `/tmp` 的 xterm、Pillow 和 python-xlib；读取指导书使用临时 PyMuPDF。这些仅用于制作交付证据，不参与三个 C 程序的编译、运行或 `make verify`。

未来方案 4 的推理依赖应集中记录在本目录，并通过 `t4_backend` 适配，保持网络协议和会话存储接口稳定。当前不附带模型、推理服务或其安装脚本。
