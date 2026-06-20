# simplefs — 一个简易的 Linux 文件系统

文件系统 "simplefs" 是理解 Linux 虚拟文件系统（VFS）和文件系统基础知识的实用工具。Linux VFS 支持多种文件系统，内核负责处理大部分通用操作，同时通过处理程序（handler）将文件系统特定的任务委托给各个文件系统。内核并不直接调用函数，而是使用各种操作表。这些操作表是每个操作的处理器集合，本质上是由函数指针组成的结构体，用于回调处理。

超级块操作在挂载时建立。inode 和文件的操作表在访问 inode 时设置。访问 inode 之前的第一步是查找过程。文件的 inode 通过调用父 inode 的 lookup 处理程序来找到。

## 功能特性

* 目录：创建、删除、列表、重命名；
* 普通文件：创建、删除、读写（通过页缓存）、重命名；
* 硬链接/符号链接（软链接）：创建、删除、重命名；
* 不支持扩展属性。

## 环境准备

请提前安装 Linux 内核头文件。
```shell
$ sudo apt install linux-headers-$(uname -r)
```

## 构建与运行

可以通过 `make` 构建内核模块和工具。
通过 `make test.img` 生成测试镜像，该命令会创建一个 50 MiB 的零填充文件。

然后可以在安装了 simplefs 内核模块的系统上挂载此镜像。
来测试内核模块：
```shell
$ sudo insmod simplefs.ko
```

对应的内核日志：
```
simplefs: module loaded
```

通过创建一个 50 MiB 的零填充文件来生成测试镜像。然后我们可以在安装了 simplefs 内核模块的系统上挂载此镜像。
```shell
$ mkdir -p test
$ dd if=/dev/zero of=test.img bs=1M count=50
$ ./mkfs.simplefs test.img
$ sudo mount -o loop -t simplefs test.img test
```

你应该会看到以下内核日志：
```
simplefs: '/dev/loop?' mount success
```
这里的 `/dev/loop?` 可能是 `loop1`、`loop2`、`loop3` 等。

执行常规的文件系统操作：（以 root 身份）
```shell
$ echo "Hello World" > test/hello
$ cat test/hello
$ ls -lR
```

移除内核挂载点和模块：
```shell
$ sudo umount test
$ sudo rmmod simplefs
```

## 设计

目前，simplefs 只提供基础功能。

### 分区布局
```
+------------+-------------+-------------------+-------------------+-------------+
| superblock | inode store | inode free bitmap | block free bitmap | data blocks |
+------------+-------------+-------------------+-------------------+-------------+
```
每个块的大小为 4 KiB。

### 超级块
超级块位于分区的第一个块（block 0），存储分区的元数据。包括总块数、总 inode 数，以及空闲 inode 和空闲块的计数。

### Inode 存储区
此部分包含分区中所有的 inode，最大 inode 数量等于分区中的块数量。每个 inode 占用 72 字节的数据，包含标准信息如文件大小和已使用的块数，以及一个 simplefs 特有的字段 `ei_block`。此字段 `ei_block` 根据文件类型有不同的用途：
  - 对于目录，它包含该目录中文件的列表。一个目录最多可容纳 40,920 个文件，文件名最长为 255 个字符，以确保能放入单个块中。
  ```
  inode
  +-----------------------+
  | i_mode = IFDIR | 0755 |      block 123 (simplefs_file_ei_block)
  | ei_block = 123    ----|--->  +----------------+
  | i_size = 4 KiB        |      | nr_files  = 7  |
  | i_blocks = 1          |      |----------------|
  +-----------------------+    0 | ee_block  = 0  |
                                  | ee_len    = 8  |      block 84(simplefs_dir_block)
                                  | ee_start  = 84 |--->  +-------------+
                                  | nr_file   = 2  |      |nr_files = 2 |
                                  |----------------|      |-------------|
                                1 | ee_block  = 8  |    0 | inode  = 24 |
                                  | ee_len    = 8  |      | nr_blk = 1  |
                                  | ee_start  = 16 |      | (foo)       |
                                  | nr_file   = 5  |      |-------------|
                                  |----------------|    1 | inode  = 45 |
                                  | ...            |      | nr_blk = 14 |
                                  |----------------|      | (bar)       |
                              341 | ee_block  = 0  |      |-------------|
                                  | ee_len    = 0  |      | ...         |
                                  | ee_start  = 0  |      |-------------|
                                  | nr_file   = 12 |   14 | 0           |
                                  +----------------+      +-------------+

  ```
  - 对于普通文件，它列出存储文件实际数据的 extent 列表。由于块 ID 以 `sizeof(struct simplefs_extent)` 字节的值存储，单个块最多可容纳 341 个链接。此限制将文件的最大大小限制为约 10.65 MiB（10,912 KiB）。
  ```
  inode
  +-----------------------+
  | i_mode = IFDIR | 0644 |          block 93
  | ei_block = 93     ----|------>  +----------------+
  | i_size = 10 KiB       |       0 | ee_block  = 0  |
  | i_blocks = 25         |         | ee_len    = 8  |      extent 94
  +-----------------------+         | ee_start  = 94 |---> +--------+
                                    |----------------|     |        |
                                  1 | ee_block  = 8  |     +--------+
                                    | ee_len    = 8  |      extent 99
                                    | ee_start  = 99 |---> +--------+
                                    |----------------|     |        |
                                  2 | ee_block  = 16 |     +--------+
                                    | ee_len    = 8  |      extent 66
                                    | ee_start  = 66 |---> +--------+
                                    |----------------|     |        |
                                    | ...            |     +--------+
                                    |----------------|
                                341 | ee_block  = 0  |
                                    | ee_len    = 0  |
                                    | ee_start  = 0  |
                                    +----------------+
  ```

### Extent 支持
一个 extent 跨越连续的块；因此，我们在一次操作中为其分配连续的磁盘块。它由 `struct simplefs_extent` 定义，包含三个成员：
- `ee_block`：extent 覆盖的第一个逻辑块。
- `ee_len`：extent 覆盖的块数量。
- `ee_start`：extent 覆盖的第一个物理块。

```
struct simplefs_extent
  +----------------+
  | ee_block =  0  |
  | ee_len   =  200|              extent
  | ee_start =  12 |-----------> +---------+
  +----------------+    block 12 |         |
                                 +---------+
                              13 |         |
                                 +---------+
                                 | ...     |
                                 +---------+
                             211 |         |
                                 +---------+
```

### 日志支持

Simplefs 现在支持外部日志设备，利用 Linux 内核中的日志块设备（jbd2）子系统。此增强功能通过维护更改日志来提高文件系统的韧性，有助于防止损坏并在崩溃或断电时方便恢复。

simplefs 中的日志支持使用 jbd2 子系统实现，该子系统是 Linux 中广泛使用的日志层。目前，simplefs 主要将日志相关信息存储在外部日志设备中。

有关日志的详细介绍，请参考以下两个网站：
[Journal(jbd2) 文档](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html)
[Journal(jbd2) API](https://docs.kernel.org/filesystems/journalling.html)

外部日志设备磁盘布局：

```
+--------------------+------------------+---------------------------+--------------+
| Journal Superblock | Descriptor Block | Metadata/Data ( modified ) | Commit Block |
+--------------------+------------------+---------------------------+--------------+
```

提示：
每个事务以一个描述符块开始，后跟若干元数据块或数据块，并以一个提交块结束。每个被修改的元数据（如 inode、bitmap 等）占用各自的块。目前，simplefs 主要记录 "extent" 元数据。


如何启用 simplefs 中的日志：

第 1 步：创建日志磁盘镜像
要创建一个 8MB 的日志磁盘镜像，使用以下 make 命令：

注意：
假设外部日志设备大小为 8 MB（这是一个任意的选择），日志块长度固定设置为 2048，由设备大小除以块大小（4096 字节）计算得出。

```shell
$ make journal
```

第 2 步：确保已加载 SimpleFS 内核模块

```shell
$ insmod simplefs/simplefs.ko
```

第 3 步：为日志设置 Loop 设备
找到一个可用的 loop 设备并将其关联到日志镜像：

```shell
$ loop_device=$(losetup -f)
$ losetup $loop_device /simplefs/journal.img
```

你应该会看到以下内核日志：
```
loop0: detected capacity change from 0 to 16384
```

第 4 步：使用外部日志挂载 SimpleFS 文件系统
使用以下命令挂载带有外部日志设备的 SimpleFS 文件系统：

```shell
mount -o loop,rw,owner,group,users,journal_path="$loop_device" -t simplefs /simplefs/test.img /test
```

对应的内核日志：
```
loop1: detected capacity change from 0 to 409600
simplefs: simplefs_parse_options: parsing options 'owner,group,journal_path=/dev/loop0'
simplefs: '/dev/loop1' mount success
```

当前限制与已知问题

1. 外部日志设备大小：

- 无法确定外部日志设备的确切大小。作为临时解决方案，大小通过设备大小除以块大小来设置，外部日志设备大小固定为 8 MB。

2. 元数据记录：

- 目前，仅记录 "extent" 元数据。未来可以包含其他元数据，如 "super block" 和 inode 元数据。

3. 外部日志设备的实现：

- 仅实现了外部日志设备。未来的改进可以包括使用内部日志（inode journal）。但这将需要在 mkfs 期间添加 bmap 函数并对磁盘分区进行适当调整。

## 许可证

`simplefs` 基于 BSD 2-clause 许可证发布。此源代码的使用受 LICENSE 文件中的 BSD 风格许可证约束。
