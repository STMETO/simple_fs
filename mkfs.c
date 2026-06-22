#if !defined(__linux__) && !defined(__APPLE__) // 仅支持 Linux 和 macOS，其他平台无法编译此文件。
#error \
    "Do not manage to build this file unless your platform is Linux or macOS."
#endif

// 文件打开、文件控制相关系统调用宏与函数：open()、O_RDWR/O_CREAT 等
#include <fcntl.h>

#if defined(__linux__)
// Linux 内核文件系统相关头文件，提供块设备 ioctl 命令 BLKGETSIZE64（获取块设备总字节大小）
#include <linux/fs.h> /* BLKGETSIZE64 */

#elif defined(__APPLE__)
// macOS 内核字节序转换工具头文件，提供系统大小端交换原生接口 OSSwapXXX
#include <libkern/OSByteOrder.h>
// macOS 磁盘设备 ioctl 头文件，提供 DKIOCGETBLOCKCOUNT(总扇区数)、DKIOCGETBLOCKSIZE(单扇区字节)
#include <sys/disk.h> /* DKIOCGETBLOCKCOUNT and DKIOCGETBLOCKSIZE */
// 主机序转小端32位整数，对齐Linux标准接口 htole32
#define htole32(x) OSSwapHostToLittleInt32(x)
// 小端32位转主机序，对齐Linux标准接口 le32toh
#define le32toh(x) OSSwapLittleToHostInt32(x)
// 主机序转小端64位整数，对齐Linux标准接口 htole64
#define htole64(x) OSSwapHostToLittleInt64(x)
// 小端64位转主机序，对齐Linux标准接口 le64toh
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif

// 固定宽度标准整数类型：uint8_t / uint32_t / uint64_t 等文件系统数据类型
#include <stdint.h>
// 标准输入输出：printf、fprintf、perror 打印日志与错误信息
#include <stdio.h>
// 内存分配释放、程序退出：malloc/free/calloc、EXIT_FAILURE/EXIT_SUCCESS
#include <stdlib.h>
// 内存操作工具函数：memset、memcpy 用于清空超级块、位图、inode缓存块
#include <string.h>
// ioctl 系统调用头文件，用于块设备/磁盘设备控制指令下发
#include <sys/ioctl.h>
// 文件/文件系统状态信息：struct stat、fstat()，获取镜像文件或块设备大小、文件类型
#include <sys/stat.h>
// POSIX 标准系统调用：write()、close() 读写磁盘镜像、关闭文件描述符
#include <unistd.h>

#include "simplefs.h"

// struct superblock 是 SimpleFS 文件系统磁盘超级块在内存中的镜像结构
struct superblock {
    union {
        struct simplefs_sb_info info;   // 真正存储文件系统元数据
        char padding[SIMPLEFS_BLOCK_SIZE]; // 填充字节,整体大小强制等于 SIMPLEFS_BLOCK_SIZE，保证写入磁盘时刚好占满一整个文件系统块。
    };
};

// 配合下面这句静态断言做尺寸校验
// 编译期直接检查，如果联合体总大小不等于块大小直接编译报错，防止布局出错。
_Static_assert(sizeof(struct superblock) == SIMPLEFS_BLOCK_SIZE);

/**
 * DIV_ROUND_UP - 整数除法向上取整宏
 * @n: 被除数（总量）
 * @d: 除数（每块可容纳的单元数）
 *
 * 作用：计算 n / d，若存在余数则结果进1，实现向上取整
 * 数学公式：⌈n/d⌉ = (n + d - 1) / d
 * 适用场景：计算存储所需块数、位图块数量等，不足一块也要分配一整块
 */
// 文件系统里大量场景需要计算占用多少个完整块，只要有一点数据就要新开一整块
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))


/**
 * write_superblock - 计算并写入SimpleFS超级块到磁盘块0
 * @fd: 磁盘镜像/块设备的可读写文件描述符
 * @fstats: 磁盘文件stat信息，用于获取总字节大小
 * Return: 成功返回堆上分配好的超级块指针，失败返回NULL
 *
 * 逻辑流程：
 * 1. 分配超级块内存
 * 2. 根据磁盘总容量计算各类元数据块、数据块数量
 * 3. 填充超级块元信息（魔数、各类块计数、空闲计数），统一转小端序存储
 * 4. 将完整超级块（一整个FS块大小）写入磁盘第0块
 * 5. 打印超级块信息并返回超级块内存指针
 */
static struct superblock *write_superblock(int fd, struct stat *fstats)
{
    // 分配一块内存，大小等于一个完整FS块（联合体superblock）
    struct superblock *sb = malloc(sizeof(struct superblock));
    if (!sb)
        return NULL;
    
    // 1. 计算磁盘总共有多少个SimpleFS块：总字节 / 单块大小 
    uint32_t nr_blocks = fstats->st_size / SIMPLEFS_BLOCK_SIZE;
    // 约定inode总数等于总块数，保证inode资源充足
    uint32_t nr_inodes = nr_blocks;

    // 对齐inode总数：让inode总数为「单个4KB inode存储块里，最多能存放多少个磁盘inode结构体」的整数倍
    uint32_t mod = nr_inodes % SIMPLEFS_INODES_PER_BLOCK;
    if (mod)    // 有余数则补足到下一个整数倍
        nr_inodes += SIMPLEFS_INODES_PER_BLOCK - mod;

    // 2. 计算[inode存储区]占用多少块（向上取整）
    uint32_t nr_istore_blocks = DIV_ROUND_UP(nr_inodes, SIMPLEFS_INODES_PER_BLOCK);
    // 3. inode空闲位图占用块数：每块字节*8=每块可存bit数，向上取整
    uint32_t nr_ifree_blocks = DIV_ROUND_UP(nr_inodes, SIMPLEFS_BLOCK_SIZE * 8);
    // 4. 数据块空闲位图占用块数
    uint32_t nr_bfree_blocks = DIV_ROUND_UP(nr_blocks, SIMPLEFS_BLOCK_SIZE * 8);
    // 5. 纯数据块数量 = 总块 - inode存储块 - inode位图块 - 数据块位图块
    uint32_t nr_data_blocks =
        nr_blocks - nr_istore_blocks - nr_ifree_blocks - nr_bfree_blocks;

    // 清空整个超级块内存（包含padding填充区）
    memset(sb, 0, sizeof(struct superblock));
    // 填充超级块核心信息，所有数值转小端序存入磁盘（文件系统统一小端存储）
    sb->info = (struct simplefs_sb_info){
        .magic = htole32(SIMPLEFS_MAGIC),         // 文件系统魔数，标识SimpleFS
        .nr_blocks = htole32(nr_blocks),          // 磁盘总块数
        .nr_inodes = htole32(nr_inodes),          // 总inode数量
        .nr_istore_blocks = htole32(nr_istore_blocks), // [inode存储区块]数
        .nr_ifree_blocks = htole32(nr_ifree_blocks),   // [inode空闲位图块]数
        .nr_bfree_blocks = htole32(nr_bfree_blocks),   // [数据块空闲位图块]数
        .nr_free_inodes = htole32(nr_inodes - 1),      // 空闲inode：预留inode1作为根，少1个空闲
        .nr_free_blocks = htole32(nr_data_blocks - 1),  // 空闲数据块：预留1块给根目录，少1块空闲
    };

    // 将整个超级块（完整1个FS块）写入fd当前偏移，也就是磁盘block0
    int ret = write(fd, sb, sizeof(struct superblock));
    if (ret != sizeof(struct superblock)) {
        free(sb);
        return NULL;
    }

    // 控制台打印超级块关键参数，方便调试查看格式化结果
    printf(
        "Superblock: (%ld)\n"
        "\tmagic=%#x\n"
        "\tnr_blocks=%u\n"
        "\tnr_inodes=%u (istore=%u blocks)\n"
        "\tnr_ifree_blocks=%u\n"
        "\tnr_bfree_blocks=%u\n"
        "\tnr_free_inodes=%u\n"
        "\tnr_free_blocks=%u\n",
        sizeof(struct superblock), sb->info.magic, sb->info.nr_blocks,
        sb->info.nr_inodes, sb->info.nr_istore_blocks, sb->info.nr_ifree_blocks,
        sb->info.nr_bfree_blocks, sb->info.nr_free_inodes,
        sb->info.nr_free_blocks);

    // 返回填充完成、已写入磁盘的超级块指针，供后续函数读取块计数
    return sb;
}

/**
 * write_inode_store - 写入全部inode存储区块（istore）
 * @fd: 磁盘镜像/块设备文件描述符，写入偏移紧跟超级块之后
 * @sb: 已填充完成的超级块指针，从中读取istore总块数、数据起始块号
 * Return: 成功返回0；内存分配/写入失败返回-1
 *
 * 功能说明：
 * 1. 分配一块FS大小缓冲区，用于存放单块inode数组
 * 2. 计算文件系统第一个可用数据块编号
 * 3. 在第一块istore中初始化1号根目录inode（inode 0保留不使用）
 * 4. 写入第一块istore到磁盘
 * 5. 剩余所有istore块全部清零写入（空白inode）
 * 6. 统一释放缓冲区并返回执行状态
 */
static int write_inode_store(int fd, struct superblock *sb)
{
    // 分配一个完整FS块大小的缓冲区，用于临时存放单块inode数组
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    // 整块内存清零，所有inode初始为空
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);

    /* Root inode (inode 1) */
    // 将缓冲区强制转为inode数组指针，一块内连续存放多个simplefs_inode
    struct simplefs_inode *inode = (struct simplefs_inode *) block;

    // 计算第一个可用数据块的块号，推导磁盘布局顺序：
    // block0：超级块
    // 后续依次：bfree空闲块位图块 → ifree空闲inode位图块 → istore inode存储块
    // 数据块 = 1(超级块) + bfree块数 + ifree块数 + istore块数
    uint32_t first_data_block = 1 + le32toh(sb->info.nr_bfree_blocks) +
                                le32toh(sb->info.nr_ifree_blocks) +
                                le32toh(sb->info.nr_istore_blocks);

    /*
     * VFS与glibc规范：inode 0废弃不用，根目录固定使用inode=1
     * readdir会跳过inode0，内核VFS也不会分配0号inode避免冲突
     * 指针向后偏移1，指向数组第2个元素（下标1，对应inode号1）
     */
    inode += 1;
    // 设置inode权限：目录 + 所有者读写执行、组/其他读执行
    inode->i_mode = htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
                            S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
    inode->i_uid = 0;   // 属主root、属组root
    inode->i_gid = 0;

    inode->i_size = htole32(SIMPLEFS_BLOCK_SIZE);   // 目录大小占用1个FS块
    inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0); // 访问/修改/创建时间初始化为0
    inode->i_blocks = htole32(1);   // 该inode占用1个数据块
    inode->i_nlink = htole32(2);    // 硬链接数为2：自身+目录内. 项
    inode->ei_block = htole32(first_data_block);     // 指向根目录存放内容的第一个数据块编号，转为小端存储

    // 将填充好根inode的第一块istore写入磁盘
    int ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    // 清空缓冲区，准备写入剩下空白istore块（全部未分配inode）
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);
    uint32_t i;
    // 循环写入剩余所有istore块（i从1开始，跳过已写的第0块istore）
    for (i = 1; i < sb->info.nr_istore_blocks; i++) {
        ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf(
        "Inode store: wrote %d blocks\n"
        "\tinode size = %ld B\n",
        i, sizeof(struct simplefs_inode));

end:
    free(block);
    return ret;
}


/**
 * write_ifree_blocks - 写入inode空闲位图（ifree）
 * @fd: 磁盘镜像/块设备文件描述符，顺序向后写入，无lseek跳转
 * @sb: 超级块指针，读取nr_ifree_blocks（inode位图总块数）
 * Return: 成功返回0，内存分配/IO写入失败返回-1
 *
 * 位图规则：每1 bit代表1个inode，bit=1表示inode空闲，bit=0表示inode已占用
 * inode0 保留不用，inode1 是根目录（已占用），因此位图最低2位清0，其余置1
 */
static int write_ifree_blocks(int fd, struct superblock *sb)
{
    // 分配一个FS块大小缓冲区，用来存放单块inode空闲位图
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;

    // 将缓冲区强制转为uint64_t数组，方便按8字节操作位图bit
    uint64_t *ifree = (uint64_t *) block;

    // 整块填充0xff：所有bit初始全部置1，代表所有inode默认空闲
    memset(ifree, 0xff, SIMPLEFS_BLOCK_SIZE);

    // 处理第一块ifree位图：清除最低2个bit，标记inode0、inode1已占用
    // 0xfffffffffffffffc 二进制末尾是 1100，bit0、bit1=0，其余bit=1
    // 所有磁盘存储数值统一转为小端序
    ifree[0] = htole64(0xfffffffffffffffc); // 只是改了其中一部分
    // 写入第一块inode空闲位图
    int ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE); // 一次性写了4096*8个bit
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    // 重置缓冲区全部bit为1，剩余所有ifree位图块全部inode空闲
    ifree[0] = 0xffffffffffffffff;
    uint32_t i;
    // 循环写入剩下所有ifree位图块（跳过已写的第0块）
    for (i = 1; i < le32toh(sb->info.nr_ifree_blocks); i++) {
        ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);    // 一次性写了4096*8个bit
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf("Ifree blocks: wrote %d blocks\n", i);

end:
    free(block);

    return ret;
}

/**
 * write_bfree_blocks - 写入数据块空闲位图（bfree）
 * @fd: 磁盘镜像文件描述符，顺序向后写入无lseek偏移跳转
 * @sb: 超级块指针，读取各类元数据块数量、bfree总块数
 * Return: 成功返回0，内存分配/IO失败返回-1
 *
 * 位图规则：1bit对应1个磁盘块；bit=1代表块空闲，bit=0代表块已占用
 * 系统初始化时，所有元数据块 + 1个根目录数据块 标记为已占用，其余全部空闲
 */
static int write_bfree_blocks(int fd, struct superblock *sb)
{
    // 计算总共有多少个磁盘块初始化就要标记为【已占用】
    // +2 含义：block0超级块 + 预留1块给根目录使用的数据块
    uint32_t nr_used = le32toh(sb->info.nr_istore_blocks) +
                       le32toh(sb->info.nr_ifree_blocks) +
                       le32toh(sb->info.nr_bfree_blocks) + 2;

    // 分配1个FS块缓冲区，存放单张数据块空闲位图
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    // 转uint64_t数组，按64bit批量操作位图比特位
    uint64_t *bfree = (uint64_t *) block;

    // 先将整块位图全部置0xff，所有bit=1（默认全部磁盘块空闲）
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    uint32_t i = 0;

    // 循环从最低位开始，依次清零nr_used个bit，标记对应块为已占用
    while (nr_used) {
        // 初始64位全1，代表本组64块全部空闲
        uint64_t line = 0xffffffffffffffff;
        // mask从最低bit(bit0)开始左移，逐个清除bit
        for (uint64_t mask = 0x1; mask; mask <<= 1) {
            line &= ~mask;   // 当前bit置0，对应磁盘块标记占用
            nr_used--;       // 待占用计数减一
            if (!nr_used)    // 所有需要占用的块标记完成，跳出内层循环
                break;
        }
        // 把修改后的64位掩码转为小端序存入位图数组
        bfree[i] = htole64(line);
        i++;
    }
    // 写入第一张bfree位图（包含已占用块标记）
    int ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* 处理剩余全部bfree位图块：整张所有bit=1，代表对应区间磁盘块全部空闲 */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    for (i = 1; i < le32toh(sb->info.nr_bfree_blocks); i++) {
        ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    // 打印一共写入多少张bfree位图块
    printf("Bfree blocks: wrote %d blocks\n", i);
end:
    // 统一释放缓冲区，避免内存泄漏
    free(block);
    return ret;
}


/**
 * write_data_blocks - 写入根目录占用的第一个数据块
 * @fd: 磁盘镜像/块设备文件描述符，按当前偏移顺序写入
 * @sb: 超级块结构体指针（本函数未实际使用，仅统一传参）
 * Return: 成功返回0，内存分配失败或写入IO失败返回-1
 *
 * 作用说明：
 * 1. 根inode的ei_block指向第一个数据块，格式化时需要预先占用该块存放根目录项；
 * 2. calloc分配全零缓冲区，写入一块空白数据块，作为根目录初始存储区；
 * 3. 仅写入1个数据块，其余数据块格式化阶段不做填充，留空即可。
 */
static int write_data_blocks(int fd, struct superblock *sb)
{
    // calloc分配内存并自动初始化为0，大小为一个文件系统块
    char *buffer = calloc(1, SIMPLEFS_BLOCK_SIZE);
    if (!buffer) {
        perror("Failed to allocate memory");
        return -1;
    }

    // 向当前文件偏移写入一整块全零数据，这是根目录专属数据块
    ssize_t ret = write(fd, buffer, SIMPLEFS_BLOCK_SIZE);
    // 写入字节数不等于完整块代表IO异常，释放内存后返回错误
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        perror("Failed to write data block");
        free(buffer);
        return -1;
    }

    // 释放缓冲区，无错误返回0
    free(buffer);
    return 0;
}

/**
 * main - SimpleFS 文件系统格式化工具入口函数（mkfs-simplefs）
 * @argc: 命令行参数个数
 * @argv: 命令行参数字符串数组
 * Return: EXIT_SUCCESS(0) 格式化成功；EXIT_FAILURE(非0) 出错退出
 *
 * 功能：接收磁盘镜像/块设备路径，校验容量，按顺序写入SimpleFS全套元数据，完成格式化
 * 平台：仅支持 Linux / macOS
 * 整体执行流程：
 * 1. 参数校验
 * 2. 读写打开磁盘文件/块设备
 * 3. 获取磁盘总字节大小（区分普通文件与块设备ioctl）
 * 4. 校验磁盘最小容量（至少100个FS块）
 * 5. 依次写入：超级块 → inode存储区 → inode空闲位图 → 数据块空闲位图 → 根目录数据块
 * 6. 统一释放堆内存、关闭文件描述符后退出
 */
int main(int argc, char **argv)
{
    // 校验命令行参数，必须传入且仅传入1个磁盘路径参数
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Open disk image：以读写模式打开目标磁盘镜像或裸块设备 */
    int fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("open():");
        return EXIT_FAILURE;
    }

    /* Get image size：获取文件基础stat信息 */
    struct stat stat_buf;
    int ret = fstat(fd, &stat_buf);
    if (ret) {
        perror("fstat():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Get block device size：
     * 如果打开的是块设备，不能直接用stat.st_size，需要调用平台专属ioctl获取真实磁盘总字节数
     * Linux / macOS 两套不同ioctl接口分开实现
     */
    if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
        long int blk_size = 0;
#if defined(__linux__)
        // Linux ioctl：BLKGETSIZE64 直接获取块设备总字节大小
        ret = ioctl(fd, BLKGETSIZE64, &blk_size);
        if (ret != 0) {
            perror("BLKGETSIZE64:");
            ret = EXIT_FAILURE;
            goto fclose;
        }
#elif defined(__APPLE__)
        uint64_t block_count = 0;   // 总扇区数
        uint32_t sector_size = 0;   // 单扇区字节大小

        // 获取磁盘总扇区数量
        ret = ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count);
        if (ret) {
            perror("DKIOCGETBLOCKCOUNT");
            ret = EXIT_FAILURE;
            goto fclose;
        }
        // 获取单个扇区字节尺寸
        ret = ioctl(fd, DKIOCGETBLOCKSIZE, &sector_size);
        if (ret) {
            perror("DKIOCGETBLOCKSIZE");
            ret = EXIT_FAILURE;
            goto fclose;
        }
        // 总字节 = 扇区总数 × 单扇区大小
        blk_size = block_count * sector_size;
#endif
        // 覆盖stat_buf中的size，统一使用真实磁盘总容量
        stat_buf.st_size = blk_size;
    }

    /* Verify if the file system image has sufficient size.
     * 校验磁盘最小容量，至少需要100个文件系统块，过小无法存放各类元数据
     */
    long int min_size = 100 * SIMPLEFS_BLOCK_SIZE;
    if (stat_buf.st_size < min_size) {
        fprintf(stderr, "File is not large enough (size=%ld, min size=%ld)\n",
                stat_buf.st_size, min_size);
        ret = EXIT_FAILURE;
        goto fclose;
    }

     /* Write superblock (block 0)
     * 第一步：写入0号块超级块，计算并存储全局FS参数
     * 返回堆上分配的超级块结构体指针，供后续函数读取块计数
     */
    struct superblock *sb = write_superblock(fd, &stat_buf);
    if (!sb) {
        perror("write_superblock():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write inode store blocks (from block 1)
     * 第二步：写入全部inode存储区块，初始化inode1为根目录
     * 文件偏移持续向后累加，无lseek回跳
     */
    ret = write_inode_store(fd, sb);
    if (ret) {
        perror("write_inode_store():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write inode free bitmap blocks
     * 第三步：写入inode空闲位图ifree
     */
    ret = write_ifree_blocks(fd, sb);
    if (ret) {
        perror("write_ifree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write block free bitmap blocks
     * 第四步：写入数据块空闲位图bfree
     */
    ret = write_bfree_blocks(fd, sb);
    if (ret) {
        perror("write_bfree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* clear a root index block
     * 第五步：写入根目录专用空白数据块，根inode的ei_block指向此处
     */
    ret = write_data_blocks(fd, sb);
    if (ret) {
        perror("write_data_blocks():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

free_sb:
    free(sb);
fclose:
    close(fd);

    return ret;
}
