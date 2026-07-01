#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include <linux/blkdev.h>
#include <linux/jbd2.h>
#include <linux/namei.h>
#include <linux/parser.h>

#include "simplefs.h"

// 函数前置声明
struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data);
void simplefs_kill_sb(struct super_block *sb);

// inode私有信息内存缓存全局句柄
static struct kmem_cache *simplefs_inode_cache;

/**
 * simplefs_init_inode_cache - 文件系统模块初始化阶段创建inode专用slab缓存
 * 调用时机：simplefs文件系统驱动module_init入口执行
 * 作用：为自定义inode私有结构体 simplefs_inode_info 预分配内核slab内存池
 * 结构体关系：struct simplefs_inode_info 内嵌标准VFS struct inode，同时携带simplefs私有磁盘元数据
 * kmem_cache_create_usercopy：带用户拷贝边界校验的slab创建接口，防止越界拷贝泄露内核敏感数据
 * 参数说明：
 *  1. "simplefs_cache"：slab缓存名称，/proc/slabinfo 可查看
 *  2. sizeof(struct simplefs_inode_info)：单个缓存对象大小
 *  3. 0：对象对齐字节，0使用默认对齐
 *  4. 0：slab创建标志位，无特殊标记
 *  5. 0：slab构造函数，无自定义初始化回调
 *  6. sizeof(struct simplefs_inode_info)：用户可拷贝区域长度，做安全校验
 *  7. NULL：slab析构函数，无自定义销毁逻辑
 * 返回值：0 创建缓存成功；-ENOMEM 内核内存不足，创建失败
 */
int simplefs_init_inode_cache(void)
{
    // 创建slab缓存，句柄存入全局静态变量 simplefs_inode_cache
    simplefs_inode_cache = kmem_cache_create_usercopy(
        "simplefs_cache",
        sizeof(struct simplefs_inode_info),0,0,0,
        sizeof(struct simplefs_inode_info),
        NULL);

    // 创建失败，缓存句柄为NULL，返回内存不足错误
    if (!simplefs_inode_cache)
        return -ENOMEM;

    return 0;
}

/**
 * simplefs_destroy_inode_cache - 卸载驱动时，销毁之前创建的inode内存缓存池
 * 执行顺序：
 * 1. rcu_barrier() 等待所有延迟释放inode的操作全部完成
 * 2. kmem_cache_destroy 彻底销毁整个slab缓存，把内存还给内核
 * 配套函数：simplefs_init_inode_cache（加载驱动建缓存，本函数卸载销毁缓存，成对使用）
 */
void simplefs_destroy_inode_cache(void)
{
    /*
     rcu_barrier()：等待所有RCU延迟释放操作全部结束
     内核销毁inode不会立刻释放内存，会延后一小段时间（RCU机制）
     如果不等就直接销毁缓存池，会出现：还有inode内存没归还，但池子没了，导致崩溃
     这条代码就是堵在这里，等所有延迟释放的inode全部归还缓存再往下走
    */
    rcu_barrier();

    // 销毁slab缓存池，这块内存彻底还给操作系统
    kmem_cache_destroy(simplefs_inode_cache);
}


/**
 * simplefs_alloc_inode - super_block的回调函数，VFS需要新建inode时自动调用
 * 功能：从之前创建好的inode slab缓存池取出一块内存，初始化内嵌的标准VFS inode并返回
 * @sb：当前文件系统的超级块
 * 返回：成功返回VFS标准inode指针；内存分配失败返回NULL
 * 配套销毁函数：simplefs_destroy_inode，用完会把内存归还缓存池
 */
static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
    // 1. 从全局slab缓存池拿一块预先分配好的simplefs_inode_info内存
    // GFP_KERNEL：常规内核内存分配标识，允许睡眠等待空闲内存
    struct simplefs_inode_info *ci =
        kmem_cache_alloc(simplefs_inode_cache, GFP_KERNEL);
    // 内存耗尽、分配失败，直接返回NULL告知VFS
    if (!ci)
        return NULL;

    // 2. 初始化结构体里内嵌的标准VFS inode，清零基础inode字段
    inode_init_once(&ci->vfs_inode);

    // 3. 返回内嵌的标准inode指针，VFS层只认识struct inode
    // 文件系统后续通过SIMPLEFS_INODE宏反向拿到ci私有信息
    return &ci->vfs_inode;
}


/**
 * simplefs_destroy_inode - super_ops回调，inode彻底销毁时VFS自动调用
 * 功能：把inode对应的自定义结构体内存归还到slab缓存池，不直接还给内核
 * @inode：待销毁的VFS标准inode指针
 * 配套成对函数：simplefs_alloc_inode（拿内存），本函数（归还内存）
 */
static void simplefs_destroy_inode(struct inode *inode)
{
    // 通过宏SIMPLEFS_INODE，从标准VFS inode反向拿到外层simplefs私有结构体
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);

    // 将这块内存放回之前创建的inode缓存池，供下次新建文件复用
    kmem_cache_free(simplefs_inode_cache, ci);
}


/**
 * simplefs_write_inode - super_operations.write_inode 回调函数
 * 触发时机：内核回写脏页时，需要把内存中修改过的inode持久化写到磁盘
 * 核心作用：把内存里VFS inode + simplefs私有信息，同步拷贝到磁盘分区上的inode存储区
 * @inode：内存中待落盘的VFS标准inode
 * @wbc：回写控制结构体，标记本次是同步/异步回写（本代码未使用）
 * 返回值：0 写入成功；-EIO 读取磁盘inode块失败
 */
static int simplefs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    // disk_inode：磁盘上存储inode的结构体（磁盘布局）
    struct simplefs_inode *disk_inode;
    // ci：拿到我们封装的、带simplefs私有数据的inode信息
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    // sb：当前文件系统超级块
    struct super_block *sb = inode->i_sb;
    // sbi：超级块的simplefs私有全局信息（总inode数量、块布局等）
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    // bh：磁盘块缓冲区，用来读写磁盘inode块
    struct buffer_head *bh;

    uint32_t ino = inode->i_ino; // 当前文件的inode编号
    // 计算这个inode存放在磁盘第几块：每块SIMPLEFS_INODES_PER_BLOCK个inode，超级块占块0，inode区从块1开始
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
    // 计算该inode在磁盘块内的偏移下标（第几个inode）
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;

    // 如果inode编号超过文件系统最大inode总数，无需写盘，直接返回成功
    if (ino >= sbi->nr_inodes)
        return 0;

    // 读取存放该inode的磁盘块到内存缓冲区bh
    bh = sb_bread(sb, inode_block);
    if (!bh) // 读磁盘失败，返回IO错误
        return -EIO;

    // 把缓冲区数据转成磁盘inode结构体指针
    disk_inode = (struct simplefs_inode *) bh->b_data;
    // 指针偏移，定位到当前ino对应的磁盘inode位置
    disk_inode += inode_shift;

    // ========== 把内存inode所有字段同步拷贝到磁盘inode ==========
    // 文件类型+权限 rwx
    disk_inode->i_mode = inode->i_mode;
    // 文件属主uid
    disk_inode->i_uid = i_uid_read(inode);
    // 文件属组gid
    disk_inode->i_gid = i_gid_read(inode);
    // 文件大小字节数
    disk_inode->i_size = inode->i_size;

    // 兼容不同内核版本：修改时间ctime（创建/属性变更时间）
    #if SIMPLEFS_AT_LEAST(6, 6, 0)
        struct timespec64 ctime = inode_get_ctime(inode);
        disk_inode->i_ctime = ctime.tv_sec;
    #else
        disk_inode->i_ctime = inode->i_ctime.tv_sec;
    #endif

    // 访问时间atime、修改内容时间mtime
    #if SIMPLEFS_AT_LEAST(6, 7, 0)
        disk_inode->i_atime = inode_get_atime_sec(inode);
        disk_inode->i_mtime = inode_get_mtime_sec(inode);
    #else
        disk_inode->i_atime = inode->i_atime.tv_sec;
        disk_inode->i_mtime = inode->i_mtime.tv_sec;
    #endif

    // 文件占用磁盘块数量
    disk_inode->i_blocks = inode->i_blocks;
    // 硬链接计数
    disk_inode->i_nlink = inode->i_nlink;

    // simplefs私有字段：文件extent索引块号（普通文件/目录的数据索引块）
    disk_inode->ei_block = ci->ei_block;
    // 软链接专用：拷贝软链接目标路径字符串
    strncpy(disk_inode->i_data, ci->i_data, sizeof(ci->i_data));

    // 标记这块缓冲区为脏：内存修改过，需要刷到磁盘
    mark_buffer_dirty(bh);
    // 强制同步，等待数据真正写入磁盘，不留在缓存
    sync_dirty_buffer(bh);
    // 释放缓冲区
    brelse(bh);

    return 0;
}

/**
 * simplefs_put_super - super_operations.put_super 回调
 * 触发时机：文件系统完全卸载(umount)，没有任何进程持有该文件系统时执行
 * 核心作用：文件系统卸载收尾清理，释放所有挂载时申请的内核资源，避免内存泄漏
 * 清理内容：
 * 1. 销毁JBD2外部日志句柄，释放日志相关资源
 * 2. 同步、作废主分区设备的所有块缓存
 * 3. 多内核兼容：同步并释放独立的外部日志块设备缓存/句柄
 * 4. 释放两张全局空闲位图内存、释放simplefs私有超级块sbi
 * @sb：待卸载文件系统的超级块
 */
static void simplefs_put_super(struct super_block *sb)
{
    // sbi：simplefs挂载时分配的私有超级块信息
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    int aborted = 0;
    int err;

    // 一、销毁外部JBD2日志
    if (sbi->journal) {
        // 判断日志是否已经异常中止
        aborted = is_journal_aborted(sbi->journal);
        // 销毁日志，关闭所有日志事务
        err = jbd2_journal_destroy(sbi->journal);
        sbi->journal = NULL;
        // 日志未崩溃、销毁又报错，打印错误日志
        if ((err < 0) && !aborted) {
            pr_err("Couldn't clean up the journal, error %d\n", -err);
        }
    }

    // 二、处理主文件系统分区块设备
    sync_blockdev(sb->s_bdev);   // 同步设备所有脏缓存，数据刷入磁盘
    invalidate_bdev(sb->s_bdev); // 作废该设备所有内存块缓存，释放缓存内存

    // 三、多内核版本兼容：处理独立外部日志块设备（journal_dev/journal_path挂载项）
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    if (sbi->s_journal_bdev_file) {
        sync_blockdev(file_bdev(sbi->s_journal_bdev_file));
        invalidate_bdev(file_bdev(sbi->s_journal_bdev_file));
    }
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    if (sbi->s_journal_bdev_handle) {
        sync_blockdev(sbi->s_journal_bdev_handle->bdev);
        invalidate_bdev(sbi->s_journal_bdev_handle->bdev);
    }
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    if (sbi->s_journal_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
    }
#elif SIMPLEFS_AT_LEAST(6, 5, 0)
    if (sbi->s_journal_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
        blkdev_put(sbi->s_journal_bdev, sb); // 释放块设备持有引用
        sbi->s_journal_bdev = NULL;
    }
#elif SIMPLEFS_AT_LEAST(5, 10, 0)
    // 日志设备不能和主分区是同一个设备
    if (sbi->s_journal_bdev && sbi->s_journal_bdev != sb->s_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
        blkdev_put(sbi->s_journal_bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
        sbi->s_journal_bdev = NULL;
    }
#endif

    // 四、释放挂载时分配的所有内存资源
    if (sbi) {
        kfree(sbi->ifree_bitmap);  // 释放inode空闲位图内存
        kfree(sbi->bfree_bitmap);  // 释放数据块空闲位图内存
        kfree(sbi);                // 释放simplefs私有超级块结构体
    }
}


/**
 * simplefs_sync_fs - super_operations.sync_fs 回调函数
 * 触发场景：执行 sync / syncfs 系统调用、卸载文件系统前、定时同步脏元数据
 * 核心功能：把内存中三份核心全局元数据全部同步写入磁盘，防止断电丢失分区全局统计/空闲位图
 * 同步三部分内容：
 *  1. 磁盘0号块超级块（分区总块、总inode、空闲统计数字）
 *  2. inode空闲位图 ifree_bitmap（标记哪些inode号可用）
 *  3. 磁盘块空闲位图 bfree_bitmap（标记哪些数据块可用）
 * @sb：当前文件系统超级块
 * @wait：同步等待标志
 *    wait=1：阻塞，等待数据真正写完磁盘再返回；
 *    wait=0：仅标记缓冲区为脏，后台异步刷盘，函数立刻返回
 * 返回值：0 同步成功；-EIO 读取磁盘块失败
 */
static int simplefs_sync_fs(struct super_block *sb, int wait)
{
    // sbi：内存里SimpleFS私有超级块信息，保存最新统计与两张空闲位图
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    // disk_sb：磁盘块0上的超级块结构体
    struct simplefs_sb_info *disk_sb;
    int i;

    // ====================== 第一步：同步超级块（磁盘块0） ======================
    // 读取磁盘0号超级块缓冲区
    struct buffer_head *bh = sb_bread(sb, 0);
    if (!bh)
        return -EIO;

    // 缓冲区数据转成磁盘超级块结构体指针
    disk_sb = (struct simplefs_sb_info *) bh->b_data;

    // 将内存里最新的分区全局统计，覆盖到磁盘超级块
    disk_sb->nr_blocks = sbi->nr_blocks;          // 分区总磁盘块
    disk_sb->nr_inodes = sbi->nr_inodes;          // 分区总inode数量
    disk_sb->nr_istore_blocks = sbi->nr_istore_blocks; // inode存储区占用块数
    disk_sb->nr_ifree_blocks = sbi->nr_ifree_blocks;   // inode位图占用块数
    disk_sb->nr_bfree_blocks = sbi->nr_bfree_blocks;   // 块位图占用块数
    disk_sb->nr_free_inodes = sbi->nr_free_inodes;     // 当前空闲inode总数
    disk_sb->nr_free_blocks = sbi->nr_free_blocks;     // 当前空闲数据块总数

    mark_buffer_dirty(bh); // 标记缓冲区为脏，需要落盘
    if (wait)
        sync_dirty_buffer(bh); // 同步模式：阻塞等待写入磁盘完成
    brelse(bh); // 释放缓冲区

    // ====================== 第二步：同步inode空闲位图 ifree_bitmap ======================
    // 遍历所有inode位图块，逐块拷贝内存位图到磁盘
    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        // 计算当前inode位图在磁盘的块号：inode存储区全部块之后
        int idx = sbi->nr_istore_blocks + i + 1;
        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        // 把内存ifree_bitmap对应分段拷贝到磁盘块缓冲区
        memcpy(bh->b_data, (void *) sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    // ====================== 第三步：同步数据块空闲位图 bfree_bitmap ======================
    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        // 块位图存储位置：inode存储区 + inode位图区 之后
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;
        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    return 0;
}


/**
 * simplefs_statfs - super_operations.statfs 回调函数
 * 作用：给用户态工具（df、statfs命令）返回文件系统整体磁盘/inode使用统计信息
 * @dentry：任意该文件系统下的目录/文件dentry，用来获取超级块sb
 * @stat：内核输出结构体kstatfs，填充各类磁盘统计数据给上层VFS
 * 返回值：0 填充信息成功，无错误返回
 */
static int simplefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    // 通过dentry拿到当前文件系统的超级块
    struct super_block *sb = dentry->d_sb;
    // 拿到simplefs私有超级块信息，里面存总块、空闲块、总inode、空闲inode等统计值
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    stat->f_type = SIMPLEFS_MAGIC;        // 文件系统类型魔数，标识是simplefs
    stat->f_bsize = SIMPLEFS_BLOCK_SIZE;  // 文件系统单个磁盘块大小
    stat->f_blocks = sbi->nr_blocks;      // 分区总磁盘块数量
    stat->f_bfree = sbi->nr_free_blocks;  // 空闲磁盘块总数
    stat->f_bavail = sbi->nr_free_blocks; // 普通用户可用空闲块（simplefs无配额，和bfree相等）
    stat->f_files = sbi->nr_inodes;       // 文件系统最大inode总数（总文件上限）
    stat->f_ffree = sbi->nr_free_inodes;  // 当前空闲inode数量（还能新建多少文件）
    stat->f_namelen = SIMPLEFS_FILENAME_LEN; // 支持的最大文件名字符长度

    return 0;
}


/* Code related to the external journal device settings */
/**
 * simplefs_get_dev_journal - 打开独立外部日志块设备，初始化JBD2 journal_t日志对象
 * 多内核版本兼容：不同内核块设备打开/持有API差异巨大，用条件编译区分
 * 整体流程：
 *  1. 根据dev_t设备号打开日志块设备，拿到bdev/bdev_handle/bdev_file句柄
 *  2. 校验文件系统块大小与日志设备逻辑块大小匹配性
 *  3. 读取日志设备起始超级块位置，固定日志长度2048块
 *  4. 调用jbd2_journal_init_dev创建JBD2日志实例
 *  5. 保存日志设备持有句柄到sbi，绑定journal私有数据为super_block
 *  6. 出错统一走out_bdev分支，关闭释放块设备句柄，防止句柄泄漏
 * @sb：主文件系统超级块
 * @journal_dev：日志块设备dev_t（主+次设备号）
 * 返回：成功返回journal_t*；失败返回错误指针ERR_PTR，out_bdev释放设备句柄返回NULL
 */
static journal_t *simplefs_get_dev_journal(struct super_block *sb,
                                            dev_t journal_dev)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;          // 日志设备超级块缓冲区
    struct block_device *bdev;       // 块设备核心结构体
    int hblock, blocksize;           // hblock：硬件逻辑块大小；blocksize：simplefs文件系统块大小
    unsigned long long sb_block, start, len;
    unsigned long offset;
    journal_t *journal;              // JBD2日志核心句柄
    int errno = 0;

    // ===================== 分支1：不同内核打开块设备，获取设备持有句柄 =====================
    #if SIMPLEFS_AT_LEAST(6, 9, 0)
        // 新版内核：通过bdev_file打开块设备文件句柄
        struct file *bdev_file;
        bdev_file = bdev_file_open_by_dev(
                journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_RESTRICT_WRITES,
                sb, &fs_holder_ops);
    #elif SIMPLEFS_AT_LEAST(6, 8, 0)
        // 6.8内核：bdev_handle包装块设备
        struct bdev_handle *bdev_handle;
        bdev_handle = bdev_open_by_dev(
                journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_RESTRICT_WRITES,
                sb, &fs_holder_ops);
    #elif SIMPLEFS_AT_LEAST(6, 7, 0)
        // 6.7：操作sb读写锁后打开bdev_handle
        struct bdev_handle *bdev_handle;
        up_write(&sb->s_umount);
        bdev_handle = bdev_open_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE,
                sb, &fs_holder_ops);
        down_write(&sb->s_umount);
    #elif SIMPLEFS_AT_LEAST(6, 6, 0)
        // 6.6：解锁super再调用blkdev_get_by_dev打开原始bdev
        up_write(&sb->s_umount);
        bdev = blkdev_get_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE, sb,
                &fs_holder_ops);
        down_write(&sb->s_umount);
    #elif SIMPLEFS_AT_LEAST(6, 5, 0)
        // 6.5：简化版blkdev_get_by_dev，无ops
        bdev = blkdev_get_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE, sb, NULL);
    #elif SIMPLEFS_AT_LEAST(5, 10, 0)
        // 5.10老内核：FMODE标志打开独占块设备
        bdev = blkdev_get_by_dev(journal_dev, FMODE_READ | FMODE_WRITE | FMODE_EXCL,sb);
    #endif

    // ===================== 分支2：判断打开设备是否失败，提取标准bdev指针 =====================
    #if SIMPLEFS_AT_LEAST(6, 9, 0)
        if (IS_ERR(bdev_file)) {
            printk(KERN_ERR
            "failed to open journal device unknown-block(%u,%u) %ld\n",
            MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev_file));
            return ERR_CAST(bdev_file);
        }
        bdev = file_bdev(bdev_file); // 从file句柄拿到底层block_device
    #elif SIMPLEFS_AT_LEAST(6, 7, 0)
        if (IS_ERR(bdev_handle)) {
            printk(KERN_ERR
            "failed to open journal device unknown-block(%u,%u) %ld\n",
            MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev_handle));
            return ERR_CAST(bdev_handle);
        }
        bdev = bdev_handle->bdev; // 从handle取出原始bdev
    #elif SIMPLEFS_AT_LEAST(5, 10, 0)
        if (IS_ERR(bdev)) {
            printk(KERN_ERR "failed to open block device (%u:%u), error: %ld\n",
            MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev));
            return ERR_CAST(bdev);
        }
    #endif

    // ===================== 校验块大小合法性 =====================
    blocksize = sb->s_blocksize;
    hblock = bdev_logical_block_size(bdev); // 获取磁盘硬件最小读写块

    // 文件系统块不能小于硬件逻辑块，否则读写对齐出错
    if (blocksize < hblock) {
        pr_err("blocksize too small for journal device\n");
        errno = -EINVAL;
        goto out_bdev; // 设备打开失败，释放句柄退出
    }

    // 计算日志设备内超级块偏移位置
    sb_block = SIMPLEFS_BLOCK_SIZE / blocksize;
    offset = SIMPLEFS_BLOCK_SIZE % blocksize;

    // 设置日志设备块大小与主文件系统一致
    #if SIMPLEFS_AT_LEAST(6, 9, 0)
        set_blocksize(bdev_file, blocksize);
    #elif SIMPLEFS_AT_LEAST(6, 7, 0)
        set_blocksize(bdev, blocksize);
    #endif
        // 读取日志设备起始超级块（仅校验设备可正常读写，无解析逻辑）
        bh = __bread(bdev, sb_block, blocksize);

    if (!bh) {
        pr_err("couldn't read superblock of external journal\n");
        errno = -EINVAL;
        goto out_bdev;
    }
    brelse(bh); // 读完缓冲区直接释放

    /*
    * FIXME: 临时硬编码日志总长度2048个文件系统块
    * 限制外部日志分区固定8MB大小，后续优化改为读取设备真实容量自动计算len
    */
    len = 2048;
    start = sb_block; // 日志事务存储起始块号

    // ===================== 初始化JBD2日志实例 =====================
    #if SIMPLEFS_AT_LEAST(6, 9, 0)
        journal = jbd2_journal_init_dev(file_bdev(bdev_file), sb->s_bdev, start, len, sb->s_blocksize);
    #elif SIMPLEFS_AT_LEAST(6, 7, 0)
        journal = jbd2_journal_init_dev(bdev_handle->bdev, sb->s_bdev, start, len, sb->s_blocksize);
    #elif SIMPLEFS_AT_LEAST(5, 15, 0)
        journal = jbd2_journal_init_dev(bdev, sb->s_bdev, start, len, blocksize);
    #endif

    // 日志初始化失败，释放设备句柄退出
    if (IS_ERR(journal)) {
        pr_err(
        "simplefs_get_dev_journal: failed to initialize journal, error "
        "%ld\n",
        PTR_ERR(journal));
        errno = PTR_ERR(journal);
        goto out_bdev;
    }

    // 将日志设备持有句柄存入sbi，卸载时同步释放
    #if SIMPLEFS_AT_LEAST(6, 9, 0)
        sbi->s_journal_bdev_file = bdev_file;
    #elif SIMPLEFS_AT_LEAST(6, 7, 0)
        sbi->s_journal_bdev_handle = bdev_handle;
    #elif SIMPLEFS_AT_LEAST(5, 15, 0)
        sbi->s_journal_bdev = bdev;
    #endif

    // 日志私有数据绑定主超级块，JBD2回调可拿到simplefs信息
    journal->j_private = sb;
    return journal;

    // ===================== 错误统一回收分支：关闭、释放日志块设备句柄 =====================
    out_bdev:
        #if SIMPLEFS_AT_LEAST(6, 9, 0)
            fput(bdev_file);
        #elif SIMPLEFS_AT_LEAST(6, 7, 0)
            bdev_release(bdev_handle);
        #elif SIMPLEFS_AT_LEAST(6, 5, 0)
            blkdev_put(bdev, sb);
        #elif SIMPLEFS_AT_LEAST(5, 10, 0)
            blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
        #endif
    return NULL;
}


/**
 * simplefs_load_journal - 根据编码后的设备号，初始化并加载外部JBD2日志
 * 调用来源：simplefs_parse_options，解析journal_dev / journal_path参数后调用
 * 功能流程：
 *  1. 解码设备号得到dev_t块设备标识
 *  2. 打开日志块设备、创建journal_t日志句柄
 *  3. 校验主分区/日志设备读写权限冲突
 *  4. 擦除过期未完成事务、加载日志事务上下文
 *  5. 将日志句柄存入sbi，挂载完成；出错则销毁日志释放资源
 * @sb：当前文件系统超级块
 * @journal_devnum：上层传入、经过new_encode_dev编码的设备数字
 * 返回：0 日志加载成功；负数错误码 加载失败
 */
static int simplefs_load_journal(struct super_block *sb,
                                unsigned long journal_devnum)
{
    journal_t *journal;                     // JBD2日志核心句柄
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb); // simplefs私有超级块
    dev_t journal_dev;                      // 标准内核块设备类型(主+次设备号)
    int err = 0;
    int really_read_only;                   // 整体是否只读：主分区只读 或 日志设备只读
    int journal_dev_ro;                     // 判断日志块设备本身是否只读

    // 把上层传入的编码数字，解码成内核标准dev_t（存放主、次设备号）
    journal_dev = new_decode_dev(journal_devnum);
    // 打开日志块设备，初始化journal_t对象
    journal = simplefs_get_dev_journal(sb, journal_dev);
    // 打开设备失败，直接返回错误
    if (IS_ERR(journal)) {
        pr_err("Failed to get journal from device, error %ld\n",
        PTR_ERR(journal));
        return PTR_ERR(journal);
    }

    // 判断日志块设备是否只读
    journal_dev_ro = bdev_read_only(journal->j_dev);
    // 整体只读标记：主分区只读 ｜ 日志设备只读，任意一个满足整体只读
    really_read_only = bdev_read_only(sb->s_bdev) | journal_dev_ro;

    // 冲突校验：日志设备只读，但文件系统以可读写模式挂载，不允许
    if (journal_dev_ro && !sb_rdonly(sb)) {
        pr_err("journal device read-only, try mounting with '-o ro'\n");
        err = -EROFS;
        goto err_out;
    }

    // 擦除日志：第二个参数=!really_read_only，只读挂载不擦除旧事务
    err = jbd2_journal_wipe(journal, !really_read_only);

    // 擦除无错误，加载磁盘上残留的日志事务
    if (!err) {
        err = jbd2_journal_load(journal);
        if (err) {
            pr_err("error loading journal, error %d\n", err);
            goto err_out;
        }
    }

    // 日志加载全部成功，挂载日志句柄到私有超级块，后续读写使用日志
    sbi->journal = journal;

    return 0;

    // 出错兜底分支：销毁已创建的日志句柄，释放所有日志资源再返回错误
    err_out:
        jbd2_journal_destroy(journal);
    return err;
}


/* we use SIMPLEFS_OPT_JOURNAL_PATH case to load external journal device now */
#define SIMPLEFS_OPT_JOURNAL_DEV 1
#define SIMPLEFS_OPT_JOURNAL_PATH 2
static const match_table_t tokens = {
    {SIMPLEFS_OPT_JOURNAL_DEV, "journal_dev=%u"},
    {SIMPLEFS_OPT_JOURNAL_PATH, "journal_path=%s"},
};


/**
 * simplefs_parse_options - 解析mount挂载时传入的-o选项参数
 * 支持两种外部日志指定方式：
 *  1. journal_dev=主设备号:次设备号  直接传设备数字
 *  2. journal_path=/dev/xxx         传块设备文件路径
 * 解析完成后调用simplefs_load_journal加载外部JBD2日志设备
 * @sb：当前文件系统超级块
 * @options：mount传入的选项字符串，逗号分隔多个参数
 * 返回值：0 解析成功；负数/正数错误码 解析失败
 */
static int simplefs_parse_options(struct super_block *sb, char *options)
{
    substring_t args[MAX_OPT_ARGS]; // 存放解析出来的参数值
    int token, ret = 0, arg;
    char *p;
    char *journal_path;     // 存放journal_path=后面的设备路径字符串
    struct inode *journal_inode;
    struct path path;       // kern_path解析文件路径输出

    pr_info("simplefs_parse_options: parsing options '%s'\n", options);

    // 按逗号分割所有挂载选项，循环处理每一项
    while ((p = strsep(&options, ","))) {
        if (!*p) // 空字符串跳过（连续逗号）
            continue;

        args[0].to = args[0].from = NULL;
        // 匹配当前参数属于哪一种（journal_dev / journal_path）
        token = match_token(p, tokens, args);

        switch (token) {
        // 分支1：journal_dev=数字 格式
        case SIMPLEFS_OPT_JOURNAL_DEV:
            // 把参数字符串转成设备号整数arg
            if (args->from && match_int(args, &arg)) {
                pr_err("simplefs_parse_options: match_int failed\n");
                return 1;
            }
            // 根据设备号加载外部日志
            if ((ret = simplefs_load_journal(sb, arg))) {
                pr_err(
                    "simplefs_parse_options: simplefs_load_journal failed with "
                    "%d\n",
                    ret);
                return ret;
            }
            break;

        // 分支2：journal_path=/dev/xxx 路径格式
        case SIMPLEFS_OPT_JOURNAL_PATH: {
            // 复制路径字符串到堆内存
            journal_path = match_strdup(&args[0]);
            if (!journal_path) {
                pr_err("simplefs_parse_options: match_strdup failed\n");
                return -ENOMEM;
            }
            // 根据文件路径查找对应的path结构体，LOOKUP_FOLLOW跟随软链接
            ret = kern_path(journal_path, LOOKUP_FOLLOW, &path);
            if (ret) {
                pr_err(
                    "simplefs_parse_options: kern_path failed with error %d\n",
                    ret);
                kfree(journal_path);
                return ret;
            }

            // 获取该路径对应的inode
            journal_inode = path.dentry->d_inode;

            // 释放路径查询占用资源
            path_put(&path);
            kfree(journal_path);

            // 判断该文件是块设备，才可以作为日志设备
            if (S_ISBLK(journal_inode->i_mode)) {
                // 把inode里的rdev设备号编码为数字
                unsigned long journal_devnum =
                    new_encode_dev(journal_inode->i_rdev);
                // 用设备号加载日志，和journal_dev分支共用加载逻辑
                if ((ret = simplefs_load_journal(sb, journal_devnum))) {
                    pr_err(
                        "simplefs_parse_options: simplefs_load_journal failed "
                        "with %d\n",
                        ret);
                    return ret;
                }
            }
            break;
        }
        }
    }
    return 0;
}


static struct super_operations simplefs_super_ops = {
    .put_super = simplefs_put_super,
    .alloc_inode = simplefs_alloc_inode,
    .destroy_inode = simplefs_destroy_inode,
    .write_inode = simplefs_write_inode,
    .sync_fs = simplefs_sync_fs,
    .statfs = simplefs_statfs,
};

/**
 * simplefs_fill_super - 文件系统挂载核心函数，mount 时 VFS 自动调用
 * 整体功能：从磁盘分区读取SimpleFS元数据，初始化超级块、加载两张空闲位图、创建根目录inode、解析挂载参数
 * 挂载失败会通过goto逐层释放已分配内存，杜绝内存泄漏
 * @sb：VFS层超级块对象，本次挂载对应的文件系统超级块
 * @data：mount -o 传入的挂载选项字符串（journal_dev=xxx这类参数）
 * @silent：静默挂载标记，为1时不打印错误日志
 * 返回值：0 挂载初始化全部成功；负数错误码(-EIO/-ENOMEM/-EINVAL)挂载失败
 */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh = NULL;          // 磁盘块缓冲区，读取超级块、位图时使用
    struct simplefs_sb_info *csb = NULL;     // 磁盘上的超级块结构体指针
    struct simplefs_sb_info *sbi = NULL;     // 内核内存中SimpleFS私有超级块信息
    struct inode *root_inode = NULL;         // 根目录inode，ino固定为1
    int ret = 0, i;

    // ========== 1、初始化VFS标准super_block基础字段 ==========
    sb->s_magic = SIMPLEFS_MAGIC;            // 设置文件系统魔数，标识SimpleFS
    sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE); // 设置文件系统块大小
    sb->s_maxbytes = SIMPLEFS_MAX_FILESIZE;  // 单个文件支持最大字节
    sb->s_op = &simplefs_super_ops;          // 绑定超级块回调函数集(alloc_inode/write_inode/sync_fs/put_super等)

    // ========== 2、读取磁盘分区内逻辑0号块（超级块） ==========
    bh = sb_bread(sb, SIMPLEFS_SB_BLOCK_NR);
    if (!bh)
        return -EIO; // 读磁盘失败，直接返回IO错误

    csb = (struct simplefs_sb_info *) bh->b_data;

    // 校验魔数，判断该分区是否为合法SimpleFS
    if (csb->magic != sb->s_magic) {
        pr_err("Wrong magic number\n");
        ret = -EINVAL;
        goto release; // 魔数不匹配，跳转释放缓冲区返回错误
    }

    // ========== 3、分配内存私有超级块sbi，拷贝磁盘超级块统计信息 ==========
    sbi = kzalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
    if (!sbi) {
        ret = -ENOMEM;
        goto release;
    }

    // 将磁盘上的分区全局统计数据拷贝到内存sbi
    sbi->nr_blocks = csb->nr_blocks;               // 分区总逻辑块
    sbi->nr_inodes = csb->nr_inodes;               // 分区最大inode总数
    sbi->nr_istore_blocks = csb->nr_istore_blocks; // inode存储区占用块数量
    sbi->nr_ifree_blocks = csb->nr_ifree_blocks;    // inode空闲位图占用块数量
    sbi->nr_bfree_blocks = csb->nr_bfree_blocks;    // 数据块空闲位图占用块数量
    sbi->nr_free_inodes = csb->nr_free_inodes;      // 当前空闲inode数量
    sbi->nr_free_blocks = csb->nr_free_blocks;      // 当前空闲数据块数量
    sb->s_fs_info = sbi; // 将私有sbi挂载到VFS超级块，后续SIMPLEFS_SB宏可取到

    brelse(bh); // 超级块读取完成，释放磁盘缓冲区
    bh = NULL;

    // ========== 4、分配并加载 inode空闲位图 ifree_bitmap ==========
    // 分配一整块内存存放全部inode位图
    sbi->ifree_bitmap = kzalloc(sbi->nr_ifree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->ifree_bitmap) {
        ret = -ENOMEM;
        goto free_sbi; // 内存分配失败，先释放sbi
    }

    // 循环读取磁盘上每一块inode位图，拷贝到内存ifree_bitmap
    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        // 位图磁盘位置：inode存储区全部块之后
        int idx = sbi->nr_istore_blocks + i + 1;
        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_ifree; // 读盘失败，释放已分配的ifree_bitmap
        }
        memcpy((void *)sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data, SIMPLEFS_BLOCK_SIZE);
        brelse(bh);
    }
    bh = NULL;

    // ========== 5、分配并加载 数据块空闲位图 bfree_bitmap ==========
    sbi->bfree_bitmap = kzalloc(sbi->nr_bfree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->bfree_bitmap) {
        ret = -ENOMEM;
        goto free_ifree;
    }

    // 循环读取磁盘每一块数据块位图，拷贝到内存bfree_bitmap
    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        // 块位图磁盘位置：inode存储区 + inode位图区 之后
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;
        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_bfree;
        }
        memcpy((void *)sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data, SIMPLEFS_BLOCK_SIZE);
        brelse(bh);
    }
    bh = NULL;

    // ========== 6、加载根目录inode（固定ino=1） ==========
    root_inode = simplefs_iget(sb, 1);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto free_bfree;
    }

    // 多内核兼容：初始化根inode的属主、用户命名空间
#if SIMPLEFS_AT_LEAST(6, 3, 0)
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
    inode_init_owner(&init_user_ns, root_inode, NULL, root_inode->i_mode);
#else
    inode_init_owner(root_inode, NULL, root_inode->i_mode);
#endif

    // 根据根inode创建根目录dentry，赋值给sb->s_root，代表文件系统挂载树根
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto iput;
    }

    // ========== 7、解析mount挂载参数（journal_dev/journal_path外部日志） ==========
    ret = simplefs_parse_options(sb, data);
    if (ret) {
        pr_err("simplefs_fill_super: Failed to parse options, error code: %d\n", ret);
        return ret;
    }

    // 全部初始化流程无错误，挂载成功返回0
    return 0;

    // -------------------- 多层goto资源释放回滚分支（失败兜底） --------------------
iput:
    // 根dentry创建失败，释放根inode
    iput(root_inode);
free_bfree:
    // 释放数据块空闲位图
    kfree(sbi->bfree_bitmap);
free_ifree:
    // 释放inode空闲位图
    kfree(sbi->ifree_bitmap);
free_sbi:
    // 释放私有超级块sbi
    kfree(sbi);
release:
    // 释放磁盘缓冲区bh
    brelse(bh);

    // 返回挂载错误码
    return ret;
}
