#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "bitmap.h"
#include "simplefs.h"

static const struct inode_operations simplefs_inode_ops;
static const struct inode_operations symlink_inode_ops;

/**
 * simplefs_iget - 根据inode编号ino加载磁盘inode到VFS内存inode
 * @sb: 当前文件系统超级块
 * @ino: 磁盘上已分配的inode唯一编号
 * 返回：成功返回填充完成的struct inode指针；失败返回ERR_PTR负错误码
 *
 * 核心作用：
 * 1. 先调用内核iget_locked检查inode缓存，缓存存在直接返回，避免重复读磁盘
 * 2. 缓存不存在（新I_NEW标记）：从磁盘inode块读取原生simplefs_inode磁盘结构
 * 3. 把磁盘小端字节序字段转CPU主机序，填充VFS标准inode通用字段
 * 4. 根据文件类型(目录/普通文件/软链接)挂载对应操作集：i_op / i_fop / a_ops
 * 5. 初始化SimpleFS私有inode信息ci->ei_block、ci->i_data
 *
 * 重要区分：
 * 仅用于**磁盘上已经分配存在**的inode；新建文件不能调用，新建用simplefs_new_inode
 */
struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode = NULL;                     // VFS通用inode对象
    struct simplefs_inode *cinode = NULL;           // 磁盘上原生simplefs inode结构体指针（存在inode块内）
    struct simplefs_inode_info *ci = NULL;          // SimpleFS私有inode信息，挂载在VFS inode尾部
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb); // 文件系统全局私有超级块信息（存总inode数量、位图等）
    struct buffer_head *bh = NULL;                  // 存放磁盘inode块的缓冲区
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;   // 该ino所属的磁盘inode块号
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;         // 该ino在inode块内的偏移下标
    int ret;

    // 校验：传入inode编号超过文件系统最大inode总数，非法
    if (ino >= sbi->nr_inodes)
        return ERR_PTR(-EINVAL);

    // 内核标准接口：从inode缓存获取锁定的inode，不存在则新建空inode并标记I_NEW
    inode = iget_locked(sb, ino);
    if (!inode)       // 内存分配失败，返回内存不足
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))  // 缓存中已有该inode（无I_NEW标记），无需读磁盘，直接返回复用
        return inode;

    // 通过容器宏获取SimpleFS私有inode扩展信息
    ci = SIMPLEFS_INODE(inode);
    /* 从磁盘读取存放所有inode的inode块 */
    bh = sb_bread(sb, inode_block);
    if (!bh) {           // 读inode磁盘块IO失败，跳转到失败回收分支
        ret = -EIO;
        goto failed;
    }

    // bh->b_data是整块inode块原始二进制，强转为simplefs磁盘inode数组
    cinode = (struct simplefs_inode *) bh->b_data;
    // 偏移到当前ino对应的磁盘inode条目
    cinode += inode_shift;

    // 填充VFS基础inode编号、所属超级块、默认inode操作集
    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &simplefs_inode_ops;

    // 磁盘存储为小端序，转CPU字节序，填充文件权限模式
    inode->i_mode = le32_to_cpu(cinode->i_mode);
    // 用户ID、组ID写入inode
    i_uid_write(inode, le32_to_cpu(cinode->i_uid));
    i_gid_write(inode, le32_to_cpu(cinode->i_gid));
    // 文件字节大小
    inode->i_size = le32_to_cpu(cinode->i_size);

    // 兼容多内核版本：设置inode变更时间ctime
#if SIMPLEFS_AT_LEAST(6, 6, 0)
    inode_set_ctime(inode, (time64_t) le32_to_cpu(cinode->i_ctime), 0);
#else
    inode->i_ctime.tv_sec = (time64_t) le32_to_cpu(cinode->i_ctime);
    inode->i_ctime.tv_nsec = 0;
#endif

    // 设置访问时间atime、修改时间mtime
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    inode_set_atime(inode, (time64_t) le32_to_cpu(cinode->i_atime), 0);
    inode_set_mtime(inode, (time64_t) le32_to_cpu(cinode->i_mtime), 0);
#else
    inode->i_atime.tv_sec = (time64_t) le32_to_cpu(cinode->i_atime);
    inode->i_atime.tv_nsec = 0;
    inode->i_mtime.tv_sec = (time64_t) le32_to_cpu(cinode->i_mtime);
    inode->i_mtime.tv_nsec = 0;
#endif

    // 文件占用磁盘块数量
    inode->i_blocks = le32_to_cpu(cinode->i_blocks);
    // 文件硬链接计数
    set_nlink(inode, le32_to_cpu(cinode->i_nlink));

    // 根据文件类型分别初始化私有字段与文件操作集
    if (S_ISDIR(inode->i_mode)) {
        // 目录：读取目录专属extent索引块号
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        // 挂载目录文件操作（open/readdir等）
        inode->i_fop = &simplefs_dir_ops;
    } else if (S_ISREG(inode->i_mode)) {
        // 普通文件：读取数据extent索引块号
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        // 普通文件读写文件操作集
        inode->i_fop = &simplefs_file_ops;
        // 绑定页缓存aops读写回调（readahead/writepage/write_begin等）
        inode->i_mapping->a_ops = &simplefs_aops;
    } else if (S_ISLNK(inode->i_mode)) {
        // 软链接：把磁盘存储的链接路径拷贝到私有缓冲区
        strncpy(ci->i_data, cinode->i_data, sizeof(ci->i_data));
        // VFS软链接路径指针指向私有缓冲区
        inode->i_link = ci->i_data;
        // 挂载软链接专属inode操作（get_link）
        inode->i_op = &symlink_inode_ops;
    }

    // 释放inode磁盘块缓冲区
    brelse(bh);

    // 清除I_NEW标记，解锁inode，内核可正常使用该inode
    unlock_new_inode(inode);

    return inode;

failed:
    // 读磁盘失败分支：释放缓冲区、标记inode创建失败
    brelse(bh);
    iget_failed(inode);
    return ERR_PTR(ret);
}


/**
 * simplefs_lookup - VFS目录查找回调函数（目录inode_ops.lookup）
 * @dir: 父目录inode，要在该目录下查找文件
 * @dentry: 待查找的目录项，包含要匹配的文件名，输出匹配到的子inode
 * @flags: 查找标志，本实现未使用
 * 返回：成功找到/不存在都返回NULL；出错返回ERR_PTR(负错误码)
 *
 * 功能说明：
 * 1. 校验文件名长度，超长返回-ENAMETOOLONG
 * 2. 读取父目录专属extent索引块ei_block
 * 3. 三层循环遍历：extent数组 → 每个extent内目录磁盘块 → 块内文件条目
 * 4. 逐条对比文件名，匹配成功则调用simplefs_iget加载对应子inode
 * 5. 更新父目录atime访问时间，标记inode脏
 * 6. d_add挂载dentry与inode：找到则绑定inode；没找到inode=NULL代表文件不存在
 *
 * 触发场景：open、stat、ls、cd、access等所有需要根据文件名找文件的系统调用
 */
static struct dentry *simplefs_lookup(struct inode *dir,
                                    struct dentry *dentry,
                                    unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    // 父目录inode私有扩展信息，存放ei_block索引块号
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    // 匹配成功后，子文件对应的VFS inode，找不到则为NULL
    struct inode *inode = NULL;
    // bh：父目录extent索引块缓冲区；bh2：单个目录数据块缓冲区
    struct buffer_head *bh = NULL, *bh2 = NULL;
    // 父目录extent索引块内存结构体
    struct simplefs_file_ei_block *eblock = NULL;
    // 单块目录数据块，存放多条文件条目
    struct simplefs_dir_block *dblock = NULL;
    // 单条文件条目：文件名 + inode号 + nr_blk占用长度
    struct simplefs_file *f = NULL;
    // 循环下标：ei=extent下标 bi=目录块下标 fi=条目下标
    int ei, bi, fi;

    // 校验文件名长度超过系统上限，返回文件名过长错误
    if (dentry->d_name.len > SIMPLEFS_FILENAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    // 读取父目录的extent索引块ei_block
    bh = sb_bread(sb, ci_dir->ei_block);
    // 读取索引块IO失败，返回IO错误
    if (!bh)
        return ERR_PTR(-EIO);
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    /* 三层循环遍历整个目录所有文件条目 */
    // 第一层：遍历目录全部extent数组
    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        // ee_start=0代表该extent从未分配磁盘，后面全部为空，直接跳出循环
        if (!eblock->extents[ei].ee_start)
            break;

        // 第二层：遍历当前extent里每一块连续的目录数据块
        for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
            // 读取当前目录磁盘块
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2) {
                brelse(bh); // 出错释放顶层索引块，防止缓存泄漏
                return ERR_PTR(-EIO);
            }

            dblock = (struct simplefs_dir_block *) bh2->b_data;
            // 当前目录块剩余有效文件总数
            int nr_files = dblock->nr_files;
            // 第三层：遍历单块内所有文件条目，按nr_blk跳跃遍历
            for (fi = 0; nr_files && fi < SIMPLEFS_FILES_PER_BLOCK;) {
                f = &dblock->files[fi];

                // 当前条目是有效文件（inode非0，不是空闲槽）
                if (f->inode) {
                    nr_files--; // 剩余待匹配文件数-1
                    // 对比文件名，完全匹配则找到目标文件
                    if (!strncmp(f->filename, dentry->d_name.name,SIMPLEFS_FILENAME_LEN)) {
                        // 根据条目内存储的ino，加载子文件inode到内存
                        inode = simplefs_iget(sb, f->inode);
                        brelse(bh2); // 释放当前目录块缓存
                        goto search_end; // 跳出全部循环
                    }
                }
                // 按当前条目占用槽位长度跳跃，不fi++（碎片合并管理）
                fi += f->nr_blk;
            }
            brelse(bh2); // 当前目录块遍历完毕，释放缓冲区
            bh2 = NULL;
        }
    }
    search_end:
    // 释放父目录顶层extent索引块缓冲区
    brelse(bh);
    bh = NULL;

    /* 更新父目录访问时间atime，读目录会更新访问时间 */
    #if SIMPLEFS_AT_LEAST(6, 7, 0)
        inode_set_atime_to_ts(dir, current_time(dir));
    #else
        dir->i_atime = current_time(dir);
    #endif
    // 父inode元数据修改，标记脏页等待内核刷盘
    mark_inode_dirty(dir);

    // 将dentry与找到的inode绑定：inode=NULL代表文件不存在
    d_add(dentry, inode);

    // 查找逻辑执行完毕，无系统调用级错误返回NULL
    return NULL;
}


/**
 * simplefs_new_inode - 分配并初始化一个全新磁盘inode（新建文件/目录/软链接底层工具）
 * @dir: 父目录inode，用于继承属主、时间等属性
 * @mode: 文件类型+权限（S_IFREG/S_IFDIR/S_IFLNK + rwx权限）
 * 返回：成功返回填充完毕的VFS inode；失败返回ERR_PTR负错误码
 *
 * 整体流程：
 * 1. 校验文件类型，仅支持普通文件、目录、软链接
 * 2. 校验全局空闲inode/数据块是否充足，磁盘满直接返回-ENOSPC
 * 3. 从inode位图分配全新ino（标记磁盘inode为已占用）
 * 4. 调用simplefs_iget创建内存VFS inode、加载空白磁盘inode结构
 * 5. 区分软链接/普通文件/目录分别初始化：
 *    - 软链接：无需分配ei_block索引块，仅初始化链接专属操作集
 *    - 文件/目录：单独分配1块ei_block（extent索引块），初始化私有ci->ei_block、文件大小、链接数、fop/aops
 * 6. 统一初始化属主、atime/mtime/ctime时间戳
 * 7. 分配失败完整回滚：释放已申请的块、归还ino到位图，防止磁盘空间泄漏
 *
 * 关键区分 vs simplefs_iget：
 * simplefs_iget：加载**磁盘已存在**的inode，只读盘、不分配任何磁盘资源；
 * simplefs_new_inode：创建**全新**inode，修改inode位图、分配ei_block索引块，真正占用磁盘空间。
 */
static struct inode *simplefs_new_inode(struct inode *dir, mode_t mode)
{
    struct inode *inode;                   // 新建文件对应的VFS标准inode
    struct simplefs_inode_info *ci;        // inode尾部私有内存扩展，存ei_block
    struct super_block *sb;                // 当前挂载分区超级块
    struct simplefs_sb_info *sbi;          // 文件系统全局私有超级块信息（位图、空闲计数）
    uint32_t ino, bno;                     // ino：新分配inode编号；bno：ei_block索引块磁盘号
    int ret;                               // 错误码临时变量

#if SIMPLEFS_AT_LEAST(6, 6, 0) && SIMPLEFS_LESS_EQUAL(6, 7, 0)
    struct timespec64 cur_time;
#endif

    // 1. 校验文件类型，只允许普通文件、目录、软链接，其他设备/管道不支持
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err(
            "File type not supported (only directory, regular file and symlink "
            "supported)\n");
        return ERR_PTR(-EINVAL);
    }

    // 2. 获取超级块与全局文件系统信息
    sb = dir->i_sb;
    sbi = SIMPLEFS_SB(sb);
    // 校验：无空闲inode 或 无空闲数据块，磁盘空间耗尽
    if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    // 3. 从inode位图分配一个全新未使用的ino，位图标记该inode为已占用
    ino = get_free_inode(sbi);
    if (!ino)
        return ERR_PTR(-ENOSPC);

    // 4. 调用iget创建内存inode、读取对应空白磁盘inode块
    // 此时磁盘inode已经分配，但内部字段全为0，需要手动初始化
    inode = simplefs_iget(sb, ino);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto put_ino; // iget失败，归还刚申请的ino，资源回滚
    }

    // ========== 分支1：软链接特殊处理 ==========
    // 软链接不需要数据extent，不分配ei_block索引块，仅存路径字符串在ci->i_data
    if (S_ISLNK(mode)) {
        // 初始化inode属主、用户命名空间（多内核版本兼容）
#if SIMPLEFS_AT_LEAST(6, 3, 0)
        inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
        inode_init_owner(&init_user_ns, inode, dir, mode);
#else
        inode_init_owner(inode, dir, mode);
#endif
        set_nlink(inode, 1); // 软链接硬链接数固定为1

        // 初始化三个时间戳
#if SIMPLEFS_AT_LEAST(6, 7, 0)
        simple_inode_init_ts(inode);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
        cur_time = current_time(inode);
        inode->i_atime = inode->i_mtime = cur_time;
        inode_set_ctime_to_ts(inode, cur_time);
#else
        inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
#endif
        // 挂载软链接专属inode操作集（仅get_link读取链接路径）
        inode->i_op = &symlink_inode_ops;
        return inode; // 软链接直接返回，无需分配数据索引块
    }

    // ========== 分支2：普通文件 / 目录 通用初始化 ==========
    // 获取inode私有扩展结构体指针，后续填充ei_block
    ci = SIMPLEFS_INODE(inode);

    // 单独分配1个磁盘块，作为该文件/目录的extent索引块ei_block
    bno = get_free_blocks(sb, 1);
    if (!bno) {
        ret = -ENOSPC;
        goto put_inode; // 索引块分配失败，释放inode资源回滚
    }

    // 初始化属主、权限，继承父目录uid/gid
#if SIMPLEFS_AT_LEAST(6, 3, 0)
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
    inode_init_owner(&init_user_ns, inode, dir, mode);
#else
    inode_init_owner(inode, dir, mode);
#endif
    // i_blocks = 1：仅占用1块ei_block索引块，数据块写入后再增加
    inode->i_blocks = 1;

    if (S_ISDIR(mode)) {
        // 目录：将分配到的索引块号存入私有ci
        ci->ei_block = bno;
        inode->i_size = SIMPLEFS_BLOCK_SIZE; // 目录默认占一块索引空间
        inode->i_fop = &simplefs_dir_ops;   // 目录专用文件操作（readdir）
        set_nlink(inode, 2); // 目录自带 . 和 .. 两个硬链接，链接数初始为2
    } else if (S_ISREG(mode)) {
        // 普通文件：存入extent索引块号
        ci->ei_block = bno;
        inode->i_size = 0; // 新建空文件，字节大小0
        inode->i_fop = &simplefs_file_ops;  // 普通文件裸读写操作
        inode->i_mapping->a_ops = &simplefs_aops; // 绑定页缓存整套读写回调
        set_nlink(inode, 1); // 普通文件初始硬链接数1
    }

    // 统一更新文件三个时间戳（atime/mtime/ctime）
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    simple_inode_init_ts(inode);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(inode);
    inode->i_atime = inode->i_mtime = cur_time;
    inode_set_ctime_to_ts(inode, cur_time);
#else
    inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
#endif

    return inode;

// 分配索引块失败回滚：释放inode内存缓存，再归还ino位图
put_inode:
    iput(inode);
// iget失败回滚：仅归还ino到位图
put_ino:
    put_inode(sbi, ino);

    return ERR_PTR(ret);
}


/**
 * simplefs_get_available_ext_idx - 在目录的extent数组里寻找第一个可写入新文件的extent下标
 * @dir_nr_files：入参传出，传入目录总文件数，循环中不断减去已占用extent内的文件计数
 * @eblock：目录自身的extent索引块结构体，存放该目录所有extent数组
 * 返回值：uint32_t 可用extent下标；返回-1代表目录完全没有空位，无法新增文件
 *
 * 目录存储结构说明：
 * 一个目录对应一块ei_block(eblock)，内部有SIMPLEFS_MAX_EXTENTS个extent条目
 * 每个extent：
 *  1. ee_start=0：该extent未分配任何磁盘块，是空空闲extent，可以分配一整块目录块存放子文件
 *  2. ee_start!=0：已分配磁盘块，extent内nr_files是当前存放的子文件数量
 *     SIMPLEFS_FILES_PER_EXT：单个extent最大能容纳的子文件总数，满了则不能再插文件
 *
 * 查找优先级规则（先复用已有未满extent，不够再新开空白extent）：
 * 1. 优先找【已经分配磁盘块、但还没存满文件】的extent，直接往里面追加子文件（不用分配新磁盘块，性能好）
 * 2. 如果所有已分配extent全部存满，则取第一个【未分配磁盘块(ee_start=0)】的extent下标，后续会分配一整块磁盘给这个extent
 * 3. 极端兜底逻辑：前面所有extent全部填满、无空白extent，返回ei+1，上层判断超出最大extent则返回目录已满
 */
static uint32_t simplefs_get_available_ext_idx(
    int *dir_nr_files,
    struct simplefs_file_ei_block *eblock)
{
    // 循环遍历extent数组下标
    int ei = 0;
    // 记录第一个可用extent下标，初始-1表示未找到
    uint32_t first_empty_blk = -1;

    // 遍历目录全部extent条目
    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        // 分支1：当前extent已分配磁盘块，且内部文件没装满
        // 最优空位，直接选中，跳出循环优先复用
        if (eblock->extents[ei].ee_start &&
            eblock->extents[ei].nr_files != SIMPLEFS_FILES_PER_EXT) {
            first_empty_blk = ei;
            break;
        }
        // 分支2：当前extent完全空白，未分配磁盘块，记录第一个空白extent下标备用
        else if (!eblock->extents[ei].ee_start) {
            if (first_empty_blk == -1)
                first_empty_blk = ei;
        }
        // 分支3：当前extent已分配磁盘块，且内部文件已经装满，无空位
        else {
            // 总剩余可遍历文件数减去本extent已存文件数,用于提前退出循环
            *dir_nr_files -= eblock->extents[ei].nr_files;
            // 还没找到可用extent，且剩余可遍历文件数归零，下一个extent作为候选空位
            if (first_empty_blk == -1 && !*dir_nr_files)
                first_empty_blk = ei + 1;
        }
        // 剩余可遍历文件数归零，无需继续循环查找
        if (!*dir_nr_files)
            break;
    }
    // 返回找到的可用extent下标，-1表示无任何空位
    return first_empty_blk;
}


/**
 * simplefs_put_new_ext - 为目录分配一整块全新extent目录存储区
 * @sb: 文件系统超级块
 * @ei: 目录索引块eblock中待填充的extent数组下标
 * @eblock: 当前目录专属extent索引块内存指针
 * 返回值：0 分配并初始化成功；-ENOSPC 无空闲磁盘块；-EIO 读目录块IO失败
 *
 * 使用场景：
 * 当目录现有所有已分配extent全部存满子文件时，上层get_available_ext_idx找到空白extent下标ei，
 * 调用本函数一次性分配连续多个磁盘块，作为新的目录存储区域，用于存放新增子文件条目。
 *
 * 核心工作：
 * 1. 一次性申请 SIMPLEFS_MAX_BLOCKS_PER_EXTENT 块连续磁盘；
 * 2. 填充当前extent元信息：起始物理块、块数量、逻辑块起始、初始文件计数0；
 * 3. 遍历新分配的所有磁盘块，清零目录块结构体，初始化目录条目分配标记nr_blk；
 *
 * 目录extent结构说明：
 * 每个extent包含多个连续simplefs_dir_block目录块，每个dir_block存放多条文件名+inode条目；
 * 分配后必须全部清零，避免旧磁盘残留脏数据干扰目录遍历。
 */
static int simplefs_put_new_ext(struct super_block *sb,
                                uint32_t ei,
                                struct simplefs_file_ei_block *eblock)
{
    int bno, bi;
    struct buffer_head *bh;
    struct simplefs_dir_block *dblock;

    // 1. 一次性分配一整个extent所需的连续磁盘块，返回首块物理块号
    bno = get_free_blocks(sb, SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
    // 无空闲磁盘块，分配失败返回磁盘满
    if (!bno)
        return -ENOSPC;

    // 填充当前extent磁盘起始块号
    eblock->extents[ei].ee_start = bno;
    // 当前extent包含多少个连续磁盘目录块
    eblock->extents[ei].ee_len = SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    // 计算该extent对应的文件逻辑起始块号：前一个extent的起始+长度，第一个extent为0
    eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block + eblock->extents[ei - 1].ee_len : 0;
    // 新extent刚分配，内部还没有任何子文件
    eblock->extents[ei].nr_files = 0;

    /* 2. 遍历extent内所有磁盘块，全部清空初始化 */
    /* TODO: 硬编码固定长度，后续可改为动态读取ee_len */
    for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
        // 读取当前循环的目录磁盘块
        bh = sb_bread(sb, eblock->extents[ei].ee_start + bi);
        // 读取目录块IO失败，返回IO错误
        if (!bh)
            return -EIO;

        // 缓冲区强转为目录块结构体
        dblock = (struct simplefs_dir_block *) bh->b_data;
        // 整块目录内存清零，清除磁盘残留旧数据
        memset(dblock, 0, sizeof(struct simplefs_dir_block));
        // 初始化第一条文件槽位的可容纳数量：本块最多SIMPLEFS_FILES_PER_BLOCK个文件
        dblock->files[0].nr_blk = SIMPLEFS_FILES_PER_BLOCK;
        // 释放缓冲区
        brelse(bh);
    }
    // 分配+初始化全部完成
    return 0;
}


/**
 * simplefs_set_file_into_dir - 向目录块 simplefs_dir_block 写入一条新文件条目（文件名+inode号）
 * @dblock：待写入的单个目录磁盘块内存结构体
 * @inode_no：新文件对应的inode编号
 * @name：新文件名字符串
 *
 * 目录块存储设计：
 * 1. dblock->files[] 数组存放文件条目，每个条目有 inode(文件标识)、filename、nr_blk(占用连续槽位数量)
 * 2. nr_blk 空间分片机制：整块空闲时 files[0].nr_blk = 总容量；新增文件会把一块大空闲拆分：
 *    原条目占用1格存新文件，剩余空闲放到下一条目，实现连续空间管理、减少碎片
 * 3. dblock->nr_files：当前目录块内有效文件总数，写入完成统一+1
 *
 * 三种分支场景：
 * 1. 目录块已有文件，需要拆分空闲槽位新增条目；
 * 2. 目录块完全空白无任何文件，直接填充第一条files[0]；
 * 3. 兜底分支（理论极少触发），覆盖边界情况，直接覆盖files[0]写入。
 */
static void simplefs_set_file_into_dir(struct simplefs_dir_block *dblock,
    uint32_t inode_no,
    const char *name)
{
    int fi = 0;

    // 场景1：目录块内已有有效文件，需要拆分空闲分片插入新条目
    if (dblock->nr_files != 0 && dblock->files[0].inode != 0) {
        // 循环找到第一个 nr_blk > 1 的空闲大槽位（可以拆分出1格存新文件）
        for (fi = 0; fi < SIMPLEFS_FILES_PER_BLOCK - 1; fi++) {
            if (dblock->files[fi].nr_blk != 1)
                break;
        }
        // fi 是待拆分的空闲条目下标，新文件放到 fi+1
        dblock->files[fi + 1].inode = inode_no;
        // 新条目占用1格，剩余空闲容量 = 原槽总容量 - 1
        dblock->files[fi + 1].nr_blk = dblock->files[fi].nr_blk - 1;
        // 拷贝文件名，末尾强制补结束符防止乱码
        strncpy(dblock->files[fi + 1].filename, name,
        SIMPLEFS_FILENAME_LEN - 1);
        dblock->files[fi + 1].filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
        // 原空闲槽只保留1格给当前新文件
        dblock->files[fi].nr_blk = 1;
    }
    // 场景2：目录块是空块，没有任何文件，直接使用数组第0位
    else if (dblock->nr_files == 0) {
        dblock->files[0].inode = inode_no;
        strncpy(dblock->files[0].filename, name, SIMPLEFS_FILENAME_LEN - 1);
        dblock->files[0].filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
    }
    // 场景3：兜底分支，处理边界异常，直接覆盖第一条目写入
    else {
        dblock->files[0].inode = inode_no;
        strncpy(dblock->files[0].filename, name, SIMPLEFS_FILENAME_LEN - 1);
        dblock->files[0].filename[SIMPLEFS_FILENAME_LEN - 1] = '\0';
    }
    // 目录块内有效文件计数 +1
    dblock->nr_files++;
}

/**
 * simplefs_create - VFS目录inode_ops.create 回调函数
 * 对应系统调用 creat / open(O_CREAT)，用于创建普通文件；mkdir会封装调用此函数并强制目录mode
 * 多版本兼容：不同内核命名空间参数不同，用条件编译区分接口
 * @dir: 父目录inode，新建文件存放于此目录
 * @dentry: 待创建文件的目录项，内含文件名，输出绑定新inode
 * @mode: 文件权限+类型(S_IFREG/S_IFDIR)
 * @excl: O_EXCL标志，本实现未使用
 * 返回值：0 创建成功；负数错误码(-ENAMETOOLONG/-EIO/-ENOSPC/-EMLINK)
 *
 * 完整执行流程：
 * 1. 合法性校验：文件名长度、父目录是否已满
 * 2. 调用simplefs_new_inode分配全新inode、ei索引块
 * 3. 清空新inode专属ei_block磁盘块，清除残留脏数据
 * 4. 调用simplefs_get_available_ext_idx寻找父目录可写入空位extent下标
 * 5. 若无可用已分配extent，调用simplefs_put_new_ext分配一整块新目录extent
 * 6. 遍历extent内目录块，找到有空位的dir_block
 * 7. simplefs_set_file_into_dir写入文件名+子ino到父目录条目
 * 8. 递增三层文件计数，标记目录缓冲区脏页
 * 9. 更新父目录mtime/atime/ctime，目录新建子目录则父inode链接数+1
 * 10. d_instantiate绑定dentry与新inode，返回0
 * 资源回滚机制：任何中途失败，依次释放新分配的目录extent、ei_block、inode编号，杜绝磁盘泄漏
 */
#if SIMPLEFS_AT_LEAST(6, 3, 0)
static int simplefs_create(struct mnt_idmap *id,
                           struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
static int simplefs_create(struct user_namespace *ns,
                           struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#else
static int simplefs_create(struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#endif
{
    struct super_block *sb;
    // 新建文件对应的inode
    struct inode *inode;
    // 父目录inode私有扩展，存放父目录ei_block索引块号
    struct simplefs_inode_info *ci_dir;
    // 父目录extent索引块内存结构体
    struct simplefs_file_ei_block *eblock;
    // 单个目录数据块，用于写入新文件条目
    struct simplefs_dir_block *dblock;
    // 新文件ei_block磁盘块缓冲区指针
    char *fblock;
    // bh：父目录索引块缓冲区；bh2：临时目录块/新文件ei_block缓冲区
    struct buffer_head *bh, *bh2;
    // dir_nr_files：父目录总文件数，用于get_available_ext_idx遍历扣减
    uint32_t dir_nr_files = 0, avail;
#if SIMPLEFS_AT_LEAST(6, 6, 0) && SIMPLEFS_LESS_EQUAL(6, 7, 0)
    struct timespec64 cur_time;
#endif
    int ret = 0, alloc = false; // alloc标记是否分配了新的目录extent，失败时用于释放
    int bi = 0;

    /* 1. 校验文件名长度，超过上限直接返回文件名过长错误 */
    if (strlen(dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* 2. 读取父目录专属extent索引块ei_block */
    ci_dir = SIMPLEFS_INODE(dir);
    sb = dir->i_sb;
    bh = sb_bread(sb, ci_dir->ei_block);
    // 读取索引磁盘块IO失败，跳转统一释放缓冲区
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    /* 校验父目录是否达到最大子文件上限，满则返回过多链接 */
    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        goto end;
    }

    /* 3. 分配全新inode（核心资源分配） */
    // 内部逻辑：分配ino位图、分配ei_block索引块、初始化inode内存对象
    inode = simplefs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto end;
    }

    /* 4. 清空新文件/目录的ei_block索引块，清除磁盘残留旧数据 */
    bh2 = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh2) {
        ret = -EIO;
        goto iput; // ei_block读盘失败，回滚inode资源
    }
    fblock = (char *) bh2->b_data;
    memset(fblock, 0, SIMPLEFS_BLOCK_SIZE); // 整块清零extent索引区
    mark_buffer_dirty(bh2); // 标记脏页，持久化空白索引块
    brelse(bh2);

    /* 5. 查找父目录可用extent下标avail */
    dir_nr_files = eblock->nr_files;
    avail = simplefs_get_available_ext_idx(&dir_nr_files, eblock);

    /* 可用下标超出最大extent数组长度，目录无空位 */
    if (avail >= SIMPLEFS_MAX_EXTENTS) {
        ret = -EMLINK;
        goto iput;
    }

    /* 6. 当前avail对应extent完全空白，未分配磁盘块，分配全新目录extent */
    if (!dir_nr_files && !eblock->extents[avail].ee_start) {
        ret = simplefs_put_new_ext(sb, avail, eblock);
        switch (ret) {
        case -ENOSPC: // 无空闲磁盘块，回滚
            ret = -ENOSPC;
            goto iput;
        case -EIO:    // 分配extent时读盘失败，需要释放刚分配的extent块
            ret = -EIO;
            goto put_block;
        }
        alloc = true; // 标记已分配新extent，失败时释放
    }

    /* 7. 在avail对应的extent内，找到第一个未存满的目录数据块dblock */
    /* TODO：当前循环硬编码ee_len，后续优化动态读取 */
    for (bi = 0; bi < eblock->extents[avail].ee_len; bi++) {
        bh2 = sb_bread(sb, eblock->extents[avail].ee_start + bi);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct simplefs_dir_block *) bh2->b_data;
        // 当前目录块未满，直接跳出循环使用该块写入条目
        if (dblock->nr_files != SIMPLEFS_FILES_PER_BLOCK)
            break;
        else
            brelse(bh2); // 当前块已满，释放继续遍历下一块
    }

    /* 8. 将新文件条目（文件名+子ino）写入目录块dblock */
    simplefs_set_file_into_dir(dblock, inode->i_ino, dentry->d_name.name);

    /* 三层文件计数同步自增 */
    eblock->extents[avail].nr_files++; // 当前extent内文件数+1
    eblock->nr_files++;                // 父目录全局总文件数+1
    mark_buffer_dirty(bh2);            // 目录数据块被修改，标记脏
    mark_buffer_dirty(bh);             // 父目录extent索引块计数变更，标记脏
    brelse(bh2);
    brelse(bh);

    /* 9. 更新元数据时间戳、标记inode脏 */
    mark_inode_dirty(inode); // 新inode元数据变更，等待刷盘

    // 更新父目录atime/mtime/ctime，新建子项修改目录内容
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    simple_inode_init_ts(dir);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(dir);
    dir->i_mtime = dir->i_atime = cur_time;
    inode_set_ctime_to_ts(dir, cur_time);
#else
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
#endif

    // 如果新建的是目录，父目录硬链接数+1（新增..反向链接）
    if (S_ISDIR(mode))
        inc_nlink(dir);
    mark_inode_dirty(dir); // 父inode时间/链接数变更，标记脏

    /* 10. dentry与新建inode绑定，VFS缓存生效 */
    d_instantiate(dentry, inode);

    return 0;

/* ========== 三层失败资源回滚分支，由深到浅释放资源 ========== */
// 分支1：分配了新的目录extent但后续出错，释放整块extent磁盘块
put_block:
    if (alloc && eblock->extents[avail].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[avail].ee_start,
                   eblock->extents[avail].ee_len);
        memset(&eblock->extents[avail], 0, sizeof(struct simplefs_extent));
    }
// 分支2：inode已分配但写入目录条目失败，释放ei_block、归还ino、销毁inode缓存
iput:
    put_blocks(SIMPLEFS_SB(sb), SIMPLEFS_INODE(inode)->ei_block, 1);
    put_inode(SIMPLEFS_SB(sb), inode->i_ino);
    iput(inode);
// 分支3：仅父目录索引块读取成功，无inode分配，释放bh缓冲区
end:
    brelse(bh);
    return ret;
}


/**
 * simplefs_remove_from_dir - 从父目录中删除指定文件的目录条目
 * @dir: 父目录inode，要从中移除子项
 * @dentry: 待删除文件/目录/软链接的dentry，内含文件名、对应inode
 * 返回：0 成功找到并删除条目；负数为IO错误
 *
 * 功能逻辑：
 * 1. 读取父目录自身的extent索引块ei_block
 * 2. 三层循环遍历：extent数组 → 每个extent内所有目录磁盘块 → 块内所有文件条目
 * 3. 匹配inode号+文件名，找到目标条目后：
 *    - 清空条目inode标记为空闲
 *    - 向前合并空闲槽位（nr_blk合并，减少碎片）
 *    - 递减三层文件计数：块内nr_files、extent内nr_files、目录总nr_files
 *    - 标记目录磁盘块、索引块为脏，等待刷盘
 * 4. 找不到条目直接返回0，不修改任何数据
 *
 * 调用场景：simplefs_unlink、simplefs_rmdir、simplefs_rename删除旧目录项时使用
 */
static int simplefs_remove_from_dir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    // 待删除文件对应的inode
    struct inode *inode = d_inode(dentry);
    // bh：父目录ei索引块缓冲区；bh2：遍历中单个目录数据块缓冲区
    struct buffer_head *bh = NULL, *bh2 = NULL;
    // 父目录extent索引块内存结构体
    struct simplefs_file_ei_block *eblock = NULL;
    // 单个目录数据块结构体（存放多条文件名+inode）
    struct simplefs_dir_block *dirblk = NULL;
    // 循环下标：ei=extent下标 bi=extent内目录块下标 fi=块内文件条目下标
    int ei = 0, bi = 0, fi = 0;
    int ret = 0, found = false;

    /* 第一步：读取父目录专属extent索引块ei_block */
    bh = sb_bread(sb, SIMPLEFS_INODE(dir)->ei_block);
    // 读取索引块IO失败，直接释放资源返回错误
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    // 剩余待遍历的总文件数量，遍历过程持续扣减，用于提前退出循环
    int dir_nr_files = eblock->nr_files;

    // 第一层循环：遍历目录所有extent，还有未遍历文件则继续
    for (ei = 0; dir_nr_files; ei++) {
        // 当前extent已分配磁盘块才需要遍历，ee_start=0是空extent直接跳过
        if (eblock->extents[ei].ee_start) {
            // 扣除当前extent内全部文件，更新剩余待遍历总数
            dir_nr_files -= eblock->extents[ei].nr_files;

            // 第二层循环：遍历当前extent里每一块目录数据块
            for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
                // 读取当前目录磁盘块
                bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
                if (!bh2) {
                    ret = -EIO;
                    goto release_bh; // IO出错，跳转释放顶层索引块
                }
                dirblk = (struct simplefs_dir_block *) bh2->b_data;
                // 当前目录块内剩余有效文件数量
                int blk_nr_files = dirblk->nr_files;

                // 第三层循环：遍历块内所有文件条目
                for (fi = 0; blk_nr_files && fi < SIMPLEFS_FILES_PER_BLOCK;) {
                    // 当前条目存在有效文件（inode非0）才匹配
                    if (dirblk->files[fi].inode) {
                        // 双重匹配：inode号完全一致 + 文件名字符串相等
                        if (dirblk->files[fi].inode == inode->i_ino &&
                            !strcmp(dirblk->files[fi].filename,
                                    dentry->d_name.name)) {
                            found = true;
                            // 清空inode号，标记该条目为空闲
                            dirblk->files[fi].inode = 0;

                            /* 向前合并空闲槽位，消除目录条目碎片 */
                            // 从当前fi前一位向前遍历
                            for (int i = fi - 1; i >= 0; i--) {
                                // 遇到有效条目或走到头部，合并容量并退出
                                if (dirblk->files[i].inode != 0 || i == 0) {
                                    // 前面槽位的可用空间 += 当前空槽的容量
                                    dirblk->files[i].nr_blk += dirblk->files[fi].nr_blk;
                                    break;
                                }
                            }

                            // 三层文件计数全部-1
                            dirblk->nr_files--;         // 单块内文件数
                            eblock->extents[ei].nr_files--; // 当前extent内文件数
                            eblock->nr_files--;               // 整个目录总文件数

                            // 目录数据块被修改，标记脏页
                            mark_buffer_dirty(bh2);
                            brelse(bh2);
                            found = true;
                            goto found_data; // 找到目标，跳出全部循环
                        }
                        // 匹配失败，剩余有效文件数-1
                        blk_nr_files--;
                    }
                    // 按当前条目的占用长度，跳到下一条目
                    fi += dirblk->files[fi].nr_blk;
                }
                // 释放当前目录块缓冲区，准备遍历下一块
                brelse(bh2);
            }
        }
    }
found_data:
    // 成功删除条目：父目录extent索引块数据已变更，标记为脏
    if (found) {
        mark_buffer_dirty(bh);
    }
release_bh:
    // 释放父目录extent索引块缓冲区
    brelse(bh);
    return ret;
}


/* Remove a link for a file including the reference in the parent directory.
 * If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int simplefs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *file_block = NULL;
    char *block;
#if SIMPLEFS_AT_LEAST(6, 6, 0) && SIMPLEFS_LESS_EQUAL(6, 7, 0)
    struct timespec64 cur_time;
#endif
    int ei = 0, bi = 0;
    int ret = 0;

    uint32_t ino = inode->i_ino;
    uint32_t bno = 0;

    ret = simplefs_remove_from_dir(dir, dentry);
    if (ret != 0)
        return ret;

    if (S_ISLNK(inode->i_mode))
        goto clean_inode;

        /* Update inode stats */
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    simple_inode_init_ts(dir);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(dir);
    dir->i_mtime = dir->i_atime = cur_time;
    inode_set_ctime_to_ts(dir, cur_time);
#else
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
#endif

    if (S_ISDIR(inode->i_mode)) {
        drop_nlink(dir);
        drop_nlink(inode);
    }
    mark_inode_dirty(dir);

    if (inode->i_nlink > 1) {
        inode_dec_link_count(inode);
        return ret;
    }

    /* Cleans up pointed blocks when unlinking a file. If reading the index
     * block fails, the inode is cleaned up regardless, resulting in the
     * permanent loss of this file's blocks. If scrubbing a data block fails,
     * do not terminate the operation (as it is already too late); instead,
     * release the block and proceed.
     */
    bno = SIMPLEFS_INODE(inode)->ei_block;
    bh = sb_bread(sb, bno);
    if (!bh)
        goto clean_inode;
    file_block = (struct simplefs_file_ei_block *) bh->b_data;

    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (!file_block->extents[ei].ee_start)
            break;

        put_blocks(sbi, file_block->extents[ei].ee_start,
                   file_block->extents[ei].ee_len);

        /* Scrub the extent */
        for (bi = 0; bi < file_block->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, file_block->extents[ei].ee_start + bi);
            if (!bh2)
                continue;
            block = (char *) bh2->b_data;
            memset(block, 0, SIMPLEFS_BLOCK_SIZE);
            mark_buffer_dirty(bh2);
            brelse(bh2);
        }
    }

    /* Scrub index block */
    memset(file_block, 0, SIMPLEFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    brelse(bh);

clean_inode:
    /* Cleanup inode and mark dirty */
    inode->i_blocks = 0;
    SIMPLEFS_INODE(inode)->ei_block = 0;
    inode->i_size = 0;
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);

#if SIMPLEFS_AT_LEAST(6, 7, 0)
    inode_set_mtime(inode, 0, 0);
    inode_set_atime(inode, 0, 0);
    inode_set_ctime(inode, 0, 0);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
    inode_set_ctime(inode, 0, 0);
#else
    inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
#endif

    inode_dec_link_count(inode);

    /* Free inode and index block from bitmap */
    if (!S_ISLNK(inode->i_mode))
        put_blocks(sbi, bno, 1);
    inode->i_mode = 0;
    put_inode(sbi, ino);

    return ret;
}

#if SIMPLEFS_AT_LEAST(6, 3, 0)
static int simplefs_rename(struct mnt_idmap *id,
                           struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
static int simplefs_rename(struct user_namespace *ns,
                           struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#else
static int simplefs_rename(struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#endif
{
    struct super_block *sb = old_dir->i_sb;
    struct simplefs_inode_info *ci_new = SIMPLEFS_INODE(new_dir);
    struct inode *src = d_inode(old_dentry);
    struct buffer_head *bh_new = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *eblock_new = NULL;
    struct simplefs_dir_block *dblock = NULL;

#if SIMPLEFS_AT_LEAST(6, 6, 0) && SIMPLEFS_LESS_EQUAL(6, 7, 0)
    struct timespec64 cur_time;
#endif

    int new_pos = -1, ret = 0;
    int ei = 0, bi = 0, fi = 0, bno = 0;

    /* fail with these unsupported flags */
    if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;

    /* Check if filename is not too long */
    if (strlen(new_dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Fail if new_dentry exists or if new_dir is full */
    bh_new = sb_bread(sb, ci_new->ei_block);
    if (!bh_new)
        return -EIO;

    eblock_new = (struct simplefs_file_ei_block *) bh_new->b_data;
    for (ei = 0; new_pos < 0 && ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (!eblock_new->extents[ei].ee_start)
            break;

        for (bi = 0; new_pos < 0 && bi < eblock_new->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_new;
            }

            dblock = (struct simplefs_dir_block *) bh2->b_data;
            int blk_nr_files = dblock->nr_files;
            for (fi = 0; blk_nr_files;) {
                /* src and target are the same dir (inode is same) */
                if (new_dir == old_dir) {
                    if (dblock->files[fi].inode &&
                        !strncmp(dblock->files[fi].filename,
                                 old_dentry->d_name.name,
                                 SIMPLEFS_FILENAME_LEN)) {
                        strncpy(dblock->files[fi].filename,
                                new_dentry->d_name.name, SIMPLEFS_FILENAME_LEN);
                        mark_buffer_dirty(bh2);
                        brelse(bh2);
                        goto release_new;
                    }
                } else {
                    /* src and target are different, then check if the
                    same name in the target directory */
                    if (dblock->files[fi].inode &&
                        !strncmp(dblock->files[fi].filename,
                                 new_dentry->d_name.name,
                                 SIMPLEFS_FILENAME_LEN)) {
                        brelse(bh2);
                        ret = -EEXIST;
                        goto release_new;
                    }
                    /* find the empty index in target directory */
                    if (new_pos < 0 && dblock->files[fi].nr_blk != 1) {
                        new_pos = fi + 1;
                        break;
                    }
                }
                blk_nr_files--;
                fi += dblock->files[fi].nr_blk;
            }
            brelse(bh2);
        }
    }

    /* If new directory is full, fail */
    if (new_pos < 0 && eblock_new->nr_files == SIMPLEFS_FILES_PER_EXT) {
        ret = -EMLINK;
        goto release_new;
    }

    /* insert in new parent directory */
    /* Get new freeblocks for extent if needed*/
    if (new_pos < 0) {
        bno = get_free_blocks(sb, SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
        if (!bno) {
            ret = -ENOSPC;
            goto release_new;
        }
        eblock_new->extents[ei].ee_start = bno;
        eblock_new->extents[ei].ee_len = SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
        eblock_new->extents[ei].ee_block =
            ei ? eblock_new->extents[ei - 1].ee_block +
                     eblock_new->extents[ei - 1].ee_len
               : 0;
        bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + 0);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct simplefs_dir_block *) bh2->b_data;
        mark_buffer_dirty(bh_new);
        new_pos = 0;
    }
    dblock->files[new_pos].inode = src->i_ino;
    strncpy(dblock->files[new_pos].filename, new_dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Update new parent inode metadata */
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    simple_inode_init_ts(new_dir);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(new_dir);
    new_dir->i_atime = new_dir->i_mtime = cur_time;
    inode_set_ctime_to_ts(new_dir, cur_time);
#else
    new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
        current_time(new_dir);
#endif

    if (S_ISDIR(src->i_mode))
        inc_nlink(new_dir);
    mark_inode_dirty(new_dir);

    /* remove target from old parent directory */
    ret = simplefs_remove_from_dir(old_dir, old_dentry);
    if (ret != 0)
        goto release_new;

        /* Update old parent inode metadata */
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    simple_inode_init_ts(old_dir);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(old_dir);
    old_dir->i_atime = old_dir->i_mtime = cur_time;
    inode_set_ctime_to_ts(old_dir, cur_time);
#else
    old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
        current_time(old_dir);
#endif

    if (S_ISDIR(src->i_mode))
        drop_nlink(old_dir);
    mark_inode_dirty(old_dir);

    return ret;

put_block:
    if (eblock_new->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock_new->extents[ei].ee_start,
                   eblock_new->extents[ei].ee_len);
        memset(&eblock_new->extents[ei], 0, sizeof(struct simplefs_extent));
    }
release_new:
    brelse(bh_new);
    return ret;
}

#if SIMPLEFS_AT_LEAST(6, 15, 0)
static struct dentry *simplefs_mkdir(struct mnt_idmap *id,
                                     struct inode *dir,
                                     struct dentry *dentry,
                                     umode_t mode)
{
    int ret = simplefs_create(id, dir, dentry, mode | S_IFDIR, 0);
    return ret ? ERR_PTR(ret) : NULL;
}
#elif SIMPLEFS_AT_LEAST(6, 3, 0)
static int simplefs_mkdir(struct mnt_idmap *id,
                          struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(id, dir, dentry, mode | S_IFDIR, 0);
}
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
static int simplefs_mkdir(struct user_namespace *ns,
                          struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(ns, dir, dentry, mode | S_IFDIR, 0);
}
#else
static int simplefs_mkdir(struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(dir, dentry, mode | S_IFDIR, 0);
}
#endif

static int simplefs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh;
    struct simplefs_file_ei_block *eblock;

    /* If the directory is not empty, fail */
    if (inode->i_nlink > 2)
        return -ENOTEMPTY;

    bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    if (eblock->nr_files != 0) {
        brelse(bh);
        return -ENOTEMPTY;
    }
    brelse(bh);

    /* Remove directory with unlink */
    return simplefs_unlink(dir, dentry);
}

static int simplefs_link(struct dentry *old_dentry,
                         struct inode *dir,
                         struct dentry *dentry)
{
    struct inode *old_inode = d_inode(old_dentry);
    struct super_block *sb = old_inode->i_sb;
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false;
    int ei = 0, bi = 0;
    uint32_t avail;

    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    int dir_nr_files = eblock->nr_files;
    avail = simplefs_get_available_ext_idx(&dir_nr_files, eblock);

    /* Validate avail index is within bounds */
    if (avail >= SIMPLEFS_MAX_EXTENTS) {
        ret = -EMLINK;
        goto end;
    }

    /* if there is not any empty space, alloc new one */
    if (!dir_nr_files && !eblock->extents[avail].ee_start) {
        ret = simplefs_put_new_ext(sb, avail, eblock);
        switch (ret) {
        case -ENOSPC:
            ret = -ENOSPC;
            goto end;
        case -EIO:
            ret = -EIO;
            goto put_block;
        }
        alloc = true;
    }

    /* TODO: fix from 8 to dynamic value */
    /* Find which simplefs_dir_block has free space */
    for (bi = 0; bi < eblock->extents[avail].ee_len; bi++) {
        bh2 = sb_bread(sb, eblock->extents[avail].ee_start + bi);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct simplefs_dir_block *) bh2->b_data;
        if (dblock->nr_files != SIMPLEFS_FILES_PER_BLOCK)
            break;
        else
            brelse(bh2);
    }

    /* write the file info into simplefs_dir_block */
    simplefs_set_file_into_dir(dblock, old_inode->i_ino, dentry->d_name.name);

    eblock->extents[avail].nr_files++;
    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode_inc_link_count(old_inode);
    ihold(old_inode);
    d_instantiate(dentry, old_inode);
    return ret;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct simplefs_extent));
    }
end:
    brelse(bh);
    return ret;
}

#if SIMPLEFS_AT_LEAST(6, 3, 0)
static int simplefs_symlink(struct mnt_idmap *id,
                            struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
static int simplefs_symlink(struct user_namespace *ns,
                            struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#else
static int simplefs_symlink(struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#endif
{
    struct super_block *sb = dir->i_sb;
    unsigned int l = strlen(symname) + 1;
    struct inode *inode = simplefs_new_inode(dir, S_IFLNK | S_IRWXUGO);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false;
    int ei = 0, bi = 0;
    uint32_t avail;

    /* Check if symlink content is not too long */
    if (l > sizeof(ci->i_data)) {
        ret = -ENAMETOOLONG;
        goto iput;
    }

    /* fill directory data block */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh) {
        ret = -EIO;
        goto iput;
    }
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto iput;
    }

    int dir_nr_files = eblock->nr_files;
    avail = simplefs_get_available_ext_idx(&dir_nr_files, eblock);

    /* Validate avail index is within bounds */
    if (avail >= SIMPLEFS_MAX_EXTENTS) {
        ret = -EMLINK;
        goto iput;
    }

    /* if there is not any empty space, alloc new one */
    if (!dir_nr_files && !eblock->extents[avail].ee_start) {
        ret = simplefs_put_new_ext(sb, avail, eblock);
        switch (ret) {
        case -ENOSPC:
            ret = -ENOSPC;
            goto iput;
        case -EIO:
            ret = -EIO;
            goto put_block;
        }
        alloc = true;
    }

    /* TODO: fix from 8 to dynamic value */
    /* Find which simplefs_dir_block has free space */
    for (bi = 0; bi < eblock->extents[avail].ee_len; bi++) {
        bh2 = sb_bread(sb, eblock->extents[avail].ee_start + bi);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct simplefs_dir_block *) bh2->b_data;
        if (dblock->nr_files != SIMPLEFS_FILES_PER_BLOCK)
            break;
        else
            brelse(bh2);
    }

    /* write the file info into simplefs_dir_block */
    simplefs_set_file_into_dir(dblock, inode->i_ino, dentry->d_name.name);

    eblock->extents[avail].nr_files++;
    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode->i_link = (char *) ci->i_data;
    memcpy(inode->i_link, symname, l);
    inode->i_size = l - 1;
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);
    return 0;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct simplefs_extent));
    }
iput:
    put_blocks(SIMPLEFS_SB(sb), ci->ei_block, 1);
    put_inode(SIMPLEFS_SB(sb), inode->i_ino);
    iput(inode);
    brelse(bh);
    return ret;
}

static const char *simplefs_get_link(struct dentry *dentry,
                                     struct inode *inode,
                                     struct delayed_call *done)
{
    return inode->i_link;
}

/**
 * simplefs_inode_ops：目录类型inode专属VFS inode操作函数集
 * 当inode是目录(S_ISDIR)时，inode->i_op 指向该结构体
 * VFS在执行目录相关系统调用时，会自动调用这里注册的回调函数
 * 所有回调均是我们前面逐行讲解过的自定义实现，适配SimpleFS磁盘布局
 */
static const struct inode_operations simplefs_inode_ops = {
    .lookup = simplefs_lookup,          // lookup：open/stat等场景，根据文件名在父目录查找子文件inode
    .create = simplefs_create,          // create：creat/open(O_CREAT) 创建普通文件
    .unlink = simplefs_unlink,          // unlink：unlink() 删除文件/硬链接，移除目录项并释放磁盘资源（链接数归0时）
    .mkdir = simplefs_mkdir,            // mkdir：mkdir() 创建子目录，底层复用simplefs_create
    .rmdir = simplefs_rmdir,            // rmdir：rmdir() 删除空目录，校验目录无内容后调用unlink回收资源
    .rename = simplefs_rename,          // rename：rename() 文件/目录改名、跨目录移动
    .link = simplefs_link,              // link：link() 创建硬链接，仅新增目录条目，不新建inode
    .symlink = simplefs_symlink,        // symlink：symlink() 创建软链接，存储路径字符串到inode私有缓冲区
};


static const struct inode_operations symlink_inode_ops = {
    .get_link = simplefs_get_link,
};
