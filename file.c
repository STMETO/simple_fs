#define pr_fmt(fmt) "simplefs: " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mpage.h>

#include "bitmap.h"
#include "simplefs.h"

/**
 * simplefs_file_get_block - 文件系统核心块映射函数（aops全部读写逻辑底层依赖）
 * @inode: 待操作文件的VFS inode
 * @iblock: 文件内部**逻辑块号**（文件视角，从0开始计数）
 * @bh_result: 输出参数，内核buffer_head，用来绑定物理块映射关系
 * @create: 创建标记，1=需要分配磁盘块；0=仅查询，不分配
 *
 * 作用：把文件逻辑块iblock翻译成磁盘分区全局物理块号；
 * 若该逻辑块无对应磁盘块且create=1，则一次性分配一整个extent连续物理块；
 * 最后调用map_bh将物理块绑定到传入的buffer_head，供内核块层下发IO。
 *
 * 调用方：readahead/readpage/writepage/write_begin 全部依赖此函数；
 * 核心逻辑：通过extent索引查询逻辑块归属区间，按需批量分配extent。
 *
 * 返回值：
 * 0 成功；
 * -EFBIG：逻辑块号超出文件系统最大支持范围；
 * -EIO：读取extent索引块磁盘IO失败；
 * -ENOSPC：磁盘无空闲块，分配extent失败。
 */
static int simplefs_file_get_block(struct inode *inode,
                                    sector_t iblock,
                                    struct buffer_head *bh_result,
                                    int create)
{
    // 获取当前文件所属超级块
    struct super_block *sb = inode->i_sb;
    // 取出SimpleFS私有inode信息，里面存有extent索引块磁盘号ei_block
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    // extents索引块内存缓冲区指针
    struct simplefs_file_ei_block *index;
    // 存放extent索引块的buffer_head
    struct buffer_head *bh_index;
    int ret = 0;          // 函数返回码，默认0成功
    int bno;              // 最终算出/分配得到的磁盘物理块号
    uint32_t extent;      // iblock所属的extent数组下标ei_index

    // 校验1：逻辑块号超过文件系统理论最大可支持块总数，返回文件过大错误
    if (iblock >= SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_MAX_EXTENTS)
        return -EFBIG;

    // 从磁盘读取当前文件专属extent索引块到内核缓存
    bh_index = sb_bread(sb, ci->ei_block);
    // 读索引块失败，直接返回IO错误
    if (!bh_index)
        return -EIO;
    // 将缓冲区原始二进制数据强转为extent索引结构体，方便操作extents数组
    index = (struct simplefs_file_ei_block *) bh_index->b_data;

    // 调用工具函数：根据文件逻辑块iblock，查找它属于第几个extent
    extent = simplefs_ext_search(index, iblock);
    // 返回-1代表该逻辑块超出已分配extent区间，文件过大
    if (extent == -1) {
        ret = -EFBIG;
        goto brelse_index;
    }

    /* 分支：目标extent从未分配磁盘块 ee_start == 0 */
    if (index->extents[extent].ee_start == 0) {
        // 仅查询不创建，无块可映射，直接返回0（内核会处理文件空洞）
        if (!create) {
            ret = 0;
            goto brelse_index;
        }
        // create=1，需要写入，一次性分配一整个extent连续物理块
        bno = get_free_blocks(sb, SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
        // 无空闲磁盘块，返回空间不足错误
        if (!bno) {
            ret = -ENOSPC;
            goto brelse_index;
        }

        // 填充当前extent信息：磁盘起始物理块、区间容纳逻辑块数量
        index->extents[extent].ee_start = bno;
        index->extents[extent].ee_len = SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
        // 计算当前extent对应的文件起始逻辑块ee_block
        index->extents[extent].ee_block = extent ? index->extents[extent - 1].ee_block + index->extents[extent - 1].ee_len : 0;
    } else {
        /* 分支：该extent已分配磁盘块，直接计算目标物理块号 */
        // iblock - ee_block = 该逻辑块在当前extent内部的相对偏移
        bno = index->extents[extent].ee_start + iblock - index->extents[extent].ee_block;
    }

    // 内核标准接口：将算出的物理块号绑定到bh_result，完成块映射
    map_bh(bh_result, sb, bno);

    brelse_index:
        // 释放全程持有的extent索引块缓冲区，防止缓存泄漏
        brelse(bh_index);

    return ret;
}


#if SIMPLEFS_AT_LEAST(5, 19, 0)
/**
 * simplefs_readahead - 5.19及以上内核页缓存批量预读回调
 * @rac: 预读控制结构体，包含待预读页面范围、inode、地址空间等信息
 *
 * 触发时机：内核检测到顺序读时，主动批量预读多页数据到page cache，提升顺序读取性能
 * 实现逻辑：直接复用内核原生通用块层批量预读接口 mpage_readahead
 * 仅向内核传入SimpleFS私有块映射函数 simplefs_file_get_block，
 * 由内核统一完成页面管理、批量IO下发、缓存填充等通用逻辑
 */
static void simplefs_readahead(struct readahead_control *rac)
{
    // 内核标准批量预读工具，传入块映射回调，复用Linux原生页缓存IO流程
    mpage_readahead(rac, simplefs_file_get_block);
}
#else
/**
 * simplefs_readpage - 5.19以下旧内核单页缺页读取回调
 * @file: 触发缺页的文件句柄
 * @page: 待填充数据的页缓存页面
 *
 * 触发时机：进程访问mmap内存、页缓存未命中产生缺页异常时，单次读取一个页面
 * 实现逻辑：复用内核原生单页读取接口 mpage_readpage
 * 内核负责磁盘IO、页面锁、缓存管理；仅由simplefs_file_get_block提供逻辑块→物理块映射
 * 返回值：0成功，负数为IO错误码
 */
static int simplefs_readpage(struct file *file, struct page *page)
{
    // 内核标准单页读工具，传入SimpleFS块映射函数
    return mpage_readpage(page, simplefs_file_get_block);
}
#endif

#if SIMPLEFS_AT_LEAST(6, 8, 0)
/**
 * simplefs_writepage - 内核6.8及以上版本 脏页回写回调
 * @page: 需要刷盘的脏页缓存页
 * @wbc: 回写控制结构体，包含同步/异步、限流、错误标记等回写策略参数
 *
 * 触发时机：
 * 1. 系统内存不足，内核回收页面，强制把脏数据落盘；
 * 2. 应用调用 sync/fsync/fdatasync，主动同步文件缓存；
 * 3. 内核后台pdflush/kswapd周期回写脏页。
 *
 * 内核6.8起全面使用folio大页结构，不再直接操作page，因此先用page_folio转换；
 * __block_write_full_folio 是内核原生folio批量刷页工具函数，负责：
 * 1. 根据页面算出文件逻辑块号；
 * 2. 调用传入的 simplefs_file_get_block 完成逻辑块→磁盘物理块映射；
 * 3. 封装bio块IO请求，下发到块设备驱动写入磁盘；
 * 4. 页面刷写完成后清除页面脏标记。
 *
 * 返回值：0 刷写成功；负数错误码代表IO失败。
 */
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    // 由page获取对应的folio大页对象（6.8+内核folio分页机制）
    struct folio *folio = page_folio(page);
    // 调用内核原生folio完整页刷写接口，传入文件inode、folio、块映射回调、回写控制参数
    return __block_write_full_folio(page->mapping->host, folio,
                                    simplefs_file_get_block, wbc);
}
#else
/**
 * simplefs_writepage - 内核6.8以下旧版本 脏页回写回调
 * @page: 需要刷盘的脏页缓存页
 * @wbc: 回写控制结构体，同步策略、回写限制等配置
 *
 * 触发时机与新版完全一致：内存回收、sync同步、后台定时刷脏页。
 * block_write_full_page 是老内核原生单页刷写通用工具：
 * 自动处理页上锁、块映射、构造磁盘写请求、清除脏标记，
 * 仅依赖 simplefs_file_get_block 提供文件系统私有extent块映射逻辑。
 *
 * 返回值：0 成功；负数为IO错误码。
 */
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    // 老内核单页刷写标准接口，传入块映射函数
    return block_write_full_page(page, simplefs_file_get_block, wbc);
}
#endif

/**
 * simplefs_write_begin - aops 页缓存写入前置钩子函数
 * 触发时机：应用调用 write() 走缓冲IO(page cache路径)，VFS 在写入数据到页缓存**之前**调用
 * 核心职责：写入前做合法性校验、预判需要新增多少磁盘块、校验磁盘空闲空间是否充足
 * 最终调用内核通用 block_write_begin，完成页面分配、块映射准备工作
 *
 * 多版本分支说明：Linux 内核持续迭代 write_begin 入参结构
 * 1. <5.19：入参携带 flags、输出 page**
 * 2. 5.19 ~ 6.11：移除flags，page 输出参数
 * 3. 6.12 ~ 6.14：page 替换为 folio（大页结构）
 * 4. >=6.15：新增 struct kiocb* iocb 作为首参数，从iocb获取file
 *
 * 通用业务逻辑全版本统一：
 * 1. 校验写入后文件是否超过单文件最大容量 SIMPLEFS_MAX_FILESIZE，超限返回 -ENOSPC
 * 2. 计算本次写入预计新增的数据块数量 nr_allocs
 * 3. 对比文件系统全局空闲块 sbi->nr_free_blocks，空间不足直接返回 -ENOSPC
 * 4. 调用内核标准 block_write_begin，传入块映射回调 simplefs_file_get_block
 * 5. 若块分配失败打印日志，向上返回错误码终止本次写入
 *
 * 返回值：0 准备正常可写入；负数错误码(-ENOSPC/-EIO等)阻止写入
 */
#if SIMPLEFS_AT_LEAST(6, 15, 0)
static int simplefs_write_begin(const struct kiocb *iocb,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                struct folio **foliop,
                                void **fsdata)
{
    // 从IO控制块取出本次操作的文件对象
    struct file *file = iocb->ki_filp;
    // 获取SimpleFS私有超级块信息（存放全局空闲块计数sbi->nr_free_blocks）
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);
    int err;
    // 本次写入预计需要新增的数据块数量
    uint32_t nr_allocs = 0;

    // 校验1：写入结束位置超出文件系统单文件最大限制，直接返回空间不足
    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    /*
     * 计算需要占用的总逻辑块：取「原有文件末尾、本次写入末尾」更大值 / 块大小
     * 得到写入完成后文件总共需要多少个数据块
     */
    nr_allocs = max(pos + len, file->f_inode->i_size) / SIMPLEFS_BLOCK_SIZE;
    /*
     * i_blocks - 1 代表当前已分配的数据块总数（i_blocks含inode自身1块）
     * 差值 = 本次需要新分配的块数量
     * 若无需新增块，置0
     */
    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    // 校验2：空闲块不足以支撑本次新增分配，返回空间不足
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;

    // 内核通用写入前置接口：分配folio页缓存、准备块映射，传入私有get_block回调
    err = block_write_begin(mapping, pos, len, foliop, simplefs_file_get_block);
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}
#elif SIMPLEFS_AT_LEAST(6, 12, 0)
static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                struct folio **foliop,
                                void **fsdata)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);
    int err;
    uint32_t nr_allocs = 0;

    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    nr_allocs = max(pos + len, file->f_inode->i_size) / SIMPLEFS_BLOCK_SIZE;
    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;

    err = block_write_begin(mapping, pos, len, foliop, simplefs_file_get_block);
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}
#elif SIMPLEFS_AT_LEAST(5, 19, 0)
static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                struct page **pagep,
                                void **fsdata)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);
    int err;
    uint32_t nr_allocs = 0;

    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    nr_allocs = max(pos + len, file->f_inode->i_size) / SIMPLEFS_BLOCK_SIZE;
    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;

    err = block_write_begin(mapping, pos, len, pagep, simplefs_file_get_block);
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}
#else
static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                unsigned int flags,
                                struct page **pagep,
                                void **fsdata)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);
    int err;
    uint32_t nr_allocs = 0;

    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    nr_allocs = max(pos + len, file->f_inode->i_size) / SIMPLEFS_BLOCK_SIZE;
    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;

    // 老内核多传入flags参数
    err = block_write_begin(mapping, pos, len, flags, pagep,
                            simplefs_file_get_block);
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}
#endif


/**
 * simplefs_write_end - aops 缓冲写入后置钩子
 * 触发时机：write() 走page cache路径，数据拷贝进页缓存完成后 VFS 调用
 * 核心功能：
 * 1. 调用内核通用 generic_write_end 完成页面写入收尾、更新文件i_size基础值
 * 2. 重新计算文件占用磁盘块总数，更新inode->i_blocks
 * 3. 更新文件修改时间mtime、inode变更时间ctime
 * 4. 若本次写入是缩小文件（truncate）：
 *    清理页缓存、读取extent索引块、释放超出文件尺寸的全部extent整块磁盘、清空extent表项
 * 5. 标记inode/索引块为脏，持久化元数据
 *
 * 多版本区分：内核迭代 write_end 参数，仅入参不同，业务逻辑完全一致
 * 6.15+：新增kiocb作为首参，从iocb拿file/inode
 * 6.12~6.14：入参使用folio大页对象
 * <6.12：入参使用老式page结构体
 *
 * 返回值：成功写入字节数；负数代表写入失败
 */
#if SIMPLEFS_AT_LEAST(6, 15, 0)
static int simplefs_write_end(const struct kiocb *iocb,
                              struct address_space *mapping,
                              loff_t pos,
                              unsigned int len,
                              unsigned int copied,
                              struct folio *foliop,
                              void *fsdata)
{
    // 从IO控制块取出本次操作文件对应的inode
    struct inode *inode = iocb->ki_filp->f_inode;
#elif SIMPLEFS_AT_LEAST(6, 12, 0)
static int simplefs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos,
                              unsigned int len,
                              unsigned int copied,
                              struct folio *foliop,
                              void *fsdata)
{
    struct inode *inode = file->f_inode;
#else
static int simplefs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos,
                              unsigned int len,
                              unsigned int copied,
                              struct page *page,
                              void *fsdata)
{
    struct inode *inode = file->f_inode;
#endif
    // 获取SimpleFS私有inode信息（存放extent索引块磁盘号ei_block）
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    // 文件所属超级块
    struct super_block *sb = inode->i_sb;
#if SIMPLEFS_AT_LEAST(6, 6, 0)
    struct timespec64 cur_time;
#endif
    // 写入前旧的块计数，用于判断文件是否缩小
    uint32_t nr_blocks_old;

    // 1. 调用内核通用收尾函数，完成页缓存写入基础逻辑，更新inode->i_size
#if SIMPLEFS_AT_LEAST(6, 15, 0)
    int ret = generic_write_end(iocb, mapping, pos, len, copied, foliop, fsdata);
#elif SIMPLEFS_AT_LEAST(6, 12, 0)
    int ret = generic_write_end(file, mapping, pos, len, copied, foliop, fsdata);
#else
    int ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
#endif
    // 实际写入字节小于请求长度，写入异常，打印日志并返回错误
    if (ret < len) {
        pr_err("wrote less than requested.");
        return ret;
    }

    // 保存修改前文件占用块总数
    nr_blocks_old = inode->i_blocks;

    // 2. 更新inode磁盘块计数：向上取整数据块数量 + 1（inode自身占用1块）
    inode->i_blocks = DIV_ROUND_UP(inode->i_size, SIMPLEFS_BLOCK_SIZE) + 1;

    // 3. 根据内核版本更新mtime/ctime（文件修改、inode变更时间）
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    cur_time = current_time(inode);
    inode_set_mtime_to_ts(inode, cur_time);
    inode_set_ctime_to_ts(inode, cur_time);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    cur_time = current_time(inode);
    inode->i_mtime = cur_time;
    inode_set_ctime_to_ts(inode, cur_time);
#else
    inode->i_mtime = inode->i_ctime = current_time(inode);
#endif

    // 标记inode元数据被修改，需要落盘
    mark_inode_dirty(inode);

    // 4. 判断：文件缩小，需要释放尾部多余extent磁盘块
    if (nr_blocks_old > inode->i_blocks) {
        int i;
        struct buffer_head *bh_index;
        struct simplefs_file_ei_block *index;
        uint32_t first_ext;

        // 截断页缓存，清除超出新文件长度的缓存页面
        truncate_pagecache(inode, inode->i_size);

        // 读取本文件extent索引块到内存
        bh_index = sb_bread(sb, ci->ei_block);
        // 读取索引块失败，无法释放磁盘块，打印丢失块警告，直接退出
        if (!bh_index) {
#if SIMPLEFS_AT_LEAST(6, 15, 0)
            pr_err("Failed to truncate '%s'. Lost %llu blocks\n",
                   iocb->ki_filp->f_path.dentry->d_name.name,
                   nr_blocks_old - inode->i_blocks);
#else
            pr_err("Failed to truncate '%s'. Lost %llu blocks\n",
                   file->f_path.dentry->d_name.name,
                   nr_blocks_old - inode->i_blocks);
#endif
            goto end;
        }
        // 二进制缓冲区强转为extent索引结构体
        index = (struct simplefs_file_ei_block *) bh_index->b_data;

        // 查找「文件最后有效逻辑块」落在第几个extent
        first_ext = simplefs_ext_search(index, inode->i_blocks - 1);

        // 判断：若有效逻辑块不是该extent起始，则下一个extent开始全部废弃
        if (inode->i_blocks - 1 != index->extents[first_ext].ee_block)
            first_ext++;

        // 从first_ext开始，遍历后续所有extent，整块释放磁盘块
        for (i = first_ext; i < SIMPLEFS_MAX_EXTENTS; i++) {
            // 遇到未分配extent直接终止循环
            if (!index->extents[i].ee_start)
                break;
            // 批量归还该extent全部连续物理块到空闲位图
            put_blocks(SIMPLEFS_SB(sb), index->extents[i].ee_start,
                       index->extents[i].ee_len);
            // 清空extent条目，标记未分配
            memset(&index->extents[i], 0, sizeof(struct simplefs_extent));
        }
        // 索引块被修改，标记脏等待同步磁盘
        mark_buffer_dirty(bh_index);
        // 释放索引块缓冲区
        brelse(bh_index);
    }
end:
    // 返回实际成功写入字节数
    return ret;
}


/**
 * simplefs_open - SimpleFS 文件打开回调函数
 * @inode: 待打开文件对应的 VFS inode 实例
 * @filp: 本次打开操作对应的 struct file 文件句柄
 *
 * 触发时机：用户态调用 open() 系统调用打开普通文件时，VFS 自动调用本函数
 * 核心功能：处理 O_TRUNC 截断逻辑
 * 1. 判断打开标志：仅当 可写(O_WRONLY/O_RDWR) + 带截断(O_TRUNC) + 文件原有大小非0 才执行清空.其他标志不做操作
 * 2. 读取文件的 extent 索引块（ei_block），遍历所有已分配的区间
 * 3. 调用 put_blocks 释放每个 extent 占用的全部磁盘数据块，清空索引表项
 * 4. 重置 inode 元数据：文件长度置0、块计数重置为1（inode自身占用1块）
 * 5. 标记索引缓冲区、inode为脏，保证修改落盘
 *
 * 返回值：成功返回0；读取extent索引块失败返回-EIO IO错误
 * 用户执行 open("test.txt", O_WRONLY | O_TRUNC) / open("test.txt", O_RDWR | O_TRUNC) 时，VFS 执行该回调。
 */
static int simplefs_open(struct inode *inode, struct file *filp)
{
    // 解析打开标志：是否只写、是否读写、是否开启截断
    bool wronly = (filp->f_flags & O_WRONLY);
    bool rdwr = (filp->f_flags & O_RDWR);
    bool trunc = (filp->f_flags & O_TRUNC); // O_TRUNC（打开即清空文件）

    // 截断触发条件：可写模式 + 开启O_TRUNC + 文件当前存在数据（大小不为0）
    if ((wronly || rdwr) && trunc && inode->i_size) {
        struct buffer_head *bh_index;        // extent索引块缓冲区头
        struct simplefs_file_ei_block *ei_block; // extent索引块内存映射地址
        sector_t iblock;                     // 遍历extent数组循环下标

        // 从磁盘读取当前文件的extent索引块ei_block
        bh_index = sb_bread(inode->i_sb, SIMPLEFS_INODE(inode)->ei_block);
        // 读索引块失败，返回IO错误
        if (!bh_index)
            return -EIO;

        // 将缓冲区数据强转为extent索引结构体，操作区间表
        ei_block = (struct simplefs_file_ei_block *) bh_index->b_data;

        // 遍历文件所有extent区间，条件：未达最大区间数 && 当前extent已分配
        for (iblock = 0; iblock <= SIMPLEFS_MAX_EXTENTS &&
                         ei_block->extents[iblock].ee_start;
             iblock++) {
            // 释放该extent占用的连续磁盘数据块（归还到块空闲位图）
            put_blocks(SIMPLEFS_SB(inode->i_sb),
                       ei_block->extents[iblock].ee_start,
                       ei_block->extents[iblock].ee_len);
            // 清空当前extent表项，标记该区间未分配
            memset(&ei_block->extents[iblock], 0,
                   sizeof(struct simplefs_extent));
        }

        // 更新inode元数据：文件长度清零，文件块计数重置为1（仅inode自身占用）
        inode->i_size = 0;
        inode->i_blocks = 1;

        // 标记extent索引缓冲区为脏，后续需要刷写到磁盘
        mark_buffer_dirty(bh_index);
        // 释放索引块缓冲区
        brelse(bh_index);
        // 标记inode为脏，元数据改动需要持久化
        mark_inode_dirty(inode);
    }
    // 打开流程无错误，返回成功
    return 0;
}


/**
 * simplefs_read - SimpleFS 自定义文件读系统调用回调
 * @file: 当前打开的文件句柄 struct file
 * @buf: 用户态缓冲区，读到的数据拷贝到这里（__user 标记用户空间地址）
 * @len: 用户请求读取的字节长度
 * @ppos: 传入传出参数，文件当前读写偏移指针
 *
 * 触发时机：用户调用 read() / pread() 系统调用时 VFS 执行
 * 实现特点：完全绕过内核 page cache，直接操作 buffer_head 块缓存读取磁盘
 * 核心流程：
 * 1. 边界校验：偏移超出文件大小直接返回0；截断读取长度不超过文件剩余数据
 * 2. 读取当前文件的 extent 索引块，加载所有数据区间信息
 * 3. 根据文件逻辑偏移 pos 换算：逻辑块号 → 对应extent下标 → 磁盘物理块号
 * 4. 循环逐块读取磁盘数据块，将内核缓冲区数据拷贝至用户空间
 * 5. 更新已读字节、剩余长度、文件偏移，循环直到读完请求长度/文件末尾
 * 6. 释放所有块缓存资源，更新文件偏移指针并返回实际读取字节数
 *
 * 返回值：
 * 正数：成功读取的字节数；
 * 0：已到达文件末尾无数据；
 * -EIO：磁盘读块IO失败；
 * -EFAULT：拷贝到用户空间地址异常
 */
static ssize_t simplefs_read(struct file *file,
                                char __user *buf,
                                size_t len,
                                loff_t *ppos)
{
    // 获取当前文件对应的VFS inode
    struct inode *inode = file_inode(file);
    // 获取文件所属文件系统超级块
    struct super_block *sb = inode->i_sb;
    // 累计成功读取的总字节数，作为最终返回值
    ssize_t bytes_read = 0;
    // 拷贝一份当前文件偏移，函数内局部计算不直接修改外部*ppos
    loff_t pos = *ppos;

    // 边界1：读写偏移已经超过文件总大小，无数据可读，直接返回0
    if (pos > inode->i_size)
        return 0;

    /* 第一步：加载本文件的extent索引块到内存 */
    // 从inode私有信息取出extent索引块磁盘号，读取索引块缓冲区
    struct buffer_head *bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    // 将缓冲区原始二进制数据转为extent索引结构体，操作区间表
    struct simplefs_file_ei_block *ei_block = (struct simplefs_file_ei_block *) bh->b_data;

    // 边界2：若请求读取范围超出文件末尾，截断len为文件剩余可读字节
    if (pos + len > inode->i_size)
        len = inode->i_size - pos;

    /* 根据当前偏移计算起始数据块信息 */
    // 逻辑块号：当前pos落在文件第几个逻辑数据块（按块大小均分文件）
    sector_t block_index = pos / SIMPLEFS_BLOCK_SIZE;
    // ei_index：该逻辑块属于第几个extent区间（每个extent固定容纳MAX_BLOCKS_PER_EXTENT个逻辑块）
    sector_t ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    // block_offset：换算得到该逻辑块对应的磁盘物理块号
    // ee_start=区间起始物理块 + 区间内相对偏移块号
    sector_t block_offset = ei_block->extents[ei_index].ee_start + block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT;

    // 循环：还有剩余需要读取的字节时持续读块
    while (len > 0) {
        // 读取当前需要操作的磁盘数据块
        struct buffer_head *bh_data = sb_bread(sb, block_offset);
        if (!bh_data) {     // 磁盘读块失败，置IO错误标记，跳出循环
            pr_err("Failed to read data block %llu\n", block_offset);
            bytes_read = -EIO;
            break;
        }

        // 当前块内的字节偏移（pos在单个块中的偏移量）
        size_t offset = pos % SIMPLEFS_BLOCK_SIZE;
        // 本次循环最多可读字节：剩余len 和 当前块剩余空间 取较小值
        size_t bytes_to_read = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - pos % SIMPLEFS_BLOCK_SIZE);
        // 将内核缓冲区数据拷贝到用户态buf
        // copy_to_user非0代表用户地址非法（进程地址越界）
        if (copy_to_user(buf + bytes_read, bh_data->b_data + offset, bytes_to_read)) {
            // 释放当前数据块缓冲区
            brelse(bh_data);
            bytes_read = -EFAULT;
            break;
        }
        // 读完释放当前数据块buffer_head
        brelse(bh_data);

        /* 更新循环变量 */
        bytes_read += bytes_to_read; // 累加已读字节
        len -= bytes_to_read;        // 剩余待读字节减少
        pos += bytes_to_read;        // 文件读写偏移向后移动

        // 偏移前进，切换下一个逻辑块，重新计算extent下标与物理块号
        block_index++;
        ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
        block_offset = ei_block->extents[ei_index].ee_start + block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    }

    // 循环结束，释放全程持有的extent索引块缓冲区
    brelse(bh);
    // 将更新后的文件偏移写回上层*ppos（read后文件指针前进）
    *ppos = pos;

    // 返回实际读取字节数 / 错误负数
    return bytes_read;
}


/**
 * simplefs_write - SimpleFS 自定义write系统调用回调函数
 * @file: 当前操作文件的struct file句柄
 * @buf: 用户态缓冲区，待写入数据来源（__user标记用户地址）
 * @len: 用户请求写入的字节长度
 * @ppos: 传入传出参数，文件读写偏移指针
 *
 * 触发时机：用户调用 write() / pwrite() 系统调用时由VFS执行
 * 实现特点：完全绕过内核page cache，直接操作buffer_head块缓存；
 * 支持自动分配extent磁盘块：写入到未分配区域时批量申请连续数据块；
 * 写完主动同步脏缓冲区到磁盘，同步更新extent索引、inode元数据；
 *
 * 核心流程：
 * 1. 边界校验：偏移超文件尾部直接返回；限制写入长度不超过文件最大容量
 * 2. 读取文件extent索引块，获取所有数据区间信息
 * 3. 根据文件偏移换算当前逻辑块、对应extent区间下标
 * 4. 循环写入：
 *    a. 若当前extent未分配，调用get_free_blocks批量分配一整个extent连续磁盘块
 *    b. 读取目标物理数据块到内核缓冲区
 *    c. copy_from_user：用户态数据拷贝至内核磁盘缓冲区
 *    d. 标记数据块为脏并强制同步落盘，释放缓冲区
 *    e. 更新剩余写入长度、已写字节、文件偏移，切换下一个逻辑块
 * 5. 全部写入完成后：
 *    a. 标记并同步修改后的extent索引块
 *    b. 更新inode文件大小、磁盘块计数、修改/变更时间
 *    c. 标记inode为脏，回填新的文件偏移给上层
 *
 * 返回值：
 * 正数：成功写入的字节总数
 * 0：偏移超出文件末尾，无数据写入
 * -EIO：读取磁盘块IO失败
 * -ENOSPC：磁盘无空闲块，分配extent失败
 * -EFAULT：拷贝用户态数据时地址非法
 */
static ssize_t simplefs_write(struct file *file,
                                const char __user *buf,
                                size_t len,
                                loff_t *ppos)
{
    // 获取文件对应的VFS inode
    struct inode *inode = file_inode(file);
    // 获取文件所属文件系统超级块（块设备、块大小、缓存上下文）
    struct super_block *sb = inode->i_sb;
    // 累计成功写入的字节数，作为最终返回值
    ssize_t bytes_write = 0;
    // 局部副本存储文件偏移，函数内运算不直接修改外部*ppos
    loff_t pos = *ppos;

    // 边界判断：读写偏移超过原有文件大小（空洞不支持，直接返回0）
    if (pos > inode->i_size)
        return 0;
    // 截断写入长度：不能超过SimpleFS单文件最大容量限制
    len = min_t(size_t, len, SIMPLEFS_MAX_FILESIZE - pos);

    /* 加载本文件的extent索引块到内存 */
    // 从私有inode取出extent索引块磁盘号，读取索引缓冲区
    struct buffer_head *bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh)    // 读取索引块IO失败，直接返回IO错误
        return -EIO;
    // 将缓冲区二进制数据强转为extent索引结构体，操作区间表
    struct simplefs_file_ei_block *ei_block = (struct simplefs_file_ei_block *) bh->b_data;

    /* 根据当前写入偏移，换算对应逻辑块、所属extent下标 */
    // 当前写入位置对应的文件逻辑块编号
    sector_t block_index = pos / SIMPLEFS_BLOCK_SIZE;
    // 该逻辑块归属第几个extent区间
    sector_t ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;

    /* 循环写入，剩余待写字节>0时持续处理 */
    while (len > 0) {
        /* 分支1：当前extent区间未分配任何磁盘块，需要批量分配 */
        if (ei_block->extents[ei_index].ee_start == 0) {
            // 一次性分配一整个extent大小的连续空闲块
            int bno = get_free_blocks(sb, SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
            // 无空闲磁盘块，返回空间不足错误
            if (!bno) {
                bytes_write = -ENOSPC;
                break;
            }
            // 填充extent区间信息：起始物理块、区间块数量、区间起始逻辑块号
            ei_block->extents[ei_index].ee_start = bno;
            ei_block->extents[ei_index].ee_len = SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
            // 计算当前extent对应的文件起始逻辑块号
            ei_block->extents[ei_index].ee_block = ei_index ? ei_block->extents[ei_index - 1].ee_block + ei_block->extents[ei_index - 1].ee_len : 0;
        }

        // 根据extent起始物理块 + 区间内相对偏移，算出目标数据块磁盘号
        struct buffer_head *bh_data = sb_bread(sb, ei_block->extents[ei_index].ee_start + block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
        // 读取数据块失败，标记IO错误并跳出循环
        if (!bh_data) {
            pr_err("Failed to read data block %llu\n",
            ei_block->extents[ei_index].ee_start +
            block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
            bytes_write = -EIO;
            break;
        }

        // 计算当前块内字节偏移
        size_t block_inner_off = pos % SIMPLEFS_BLOCK_SIZE;
        // 本次循环最多可写入字节：剩余len 和 当前块剩余空间取最小值
        size_t bytes_to_write = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - block_inner_off);

        // copy_from_user：用户态buf数据拷贝到内核磁盘缓冲区
        // 返回非0代表用户地址非法、无法访问
        if (copy_from_user(bh_data->b_data + block_inner_off, buf + bytes_write, bytes_to_write)) {
            // 提前释放缓冲区，防止泄漏
            brelse(bh_data);
            bytes_write = -EFAULT;
            break;
        }

        // 标记数据块缓冲区为脏（数据被修改，需要落盘）
        mark_buffer_dirty(bh_data);
        // 强制同步脏缓冲区，等待数据写入磁盘（同步写，性能差）
        sync_dirty_buffer(bh_data);
        // 释放当前数据块buffer_head，减少引用计数
        brelse(bh_data);

        /* 更新循环状态变量 */
        len = len - bytes_to_write;   // 剩余待写字节减少
        bytes_write += bytes_to_write;// 累计成功写入字节
        pos += bytes_to_write;       // 文件偏移向后移动

        // 偏移前进，切换下一个逻辑块，重新计算extent下标
        block_index = pos / SIMPLEFS_BLOCK_SIZE;
        ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    }

    /* 写入循环结束，更新extent索引块并同步到磁盘 */
    mark_buffer_dirty(bh);  // 索引块存在extent新增修改，标记脏
    sync_dirty_buffer(bh);  // 强制同步索引块落盘
    brelse(bh);             // 释放全程持有的extent索引缓冲区

    /* 更新inode元数据 */
    // 文件大小取原有大小和新偏移的最大值（覆盖/追加都会扩容）
    inode->i_size = max(pos, inode->i_size);
    // i_blocks = 文件占用数据块数量向上取整 + 1（inode自身占用1块）
    inode->i_blocks = DIV_ROUND_UP(inode->i_size, SIMPLEFS_BLOCK_SIZE) + 1;

    // 根据内核版本分支更新文件修改时间(mtime)、inode变更时间(ctime)
    #if SIMPLEFS_AT_LEAST(6, 7, 0)
        struct timespec64 cur_time = current_time(inode);
        inode_set_mtime_to_ts(inode, cur_time);
        inode_set_ctime_to_ts(inode, cur_time);
    #elif SIMPLEFS_AT_LEAST(6, 6, 0)
        struct timespec64 cur_time = current_time(inode);
        inode->i_mtime = cur_time;
        inode_set_ctime_to_ts(inode, cur_time);
    #else
        inode->i_mtime = inode->i_ctime = current_time(inode);
    #endif

    mark_inode_dirty(inode); // inode元数据修改，标记为脏等待同步

    // 将更新后的文件偏移回填给上层VFS，write后文件指针前进
    *ppos = pos;

    // 返回成功写入字节数 / 负数错误码
    return bytes_write;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * simplefs_aops - SimpleFS 文件地址空间操作回调集 address_space_operations
 * 挂载在 inode->i_mapping->a_ops，管理内核页缓存(page cache)整套读写逻辑
 * 适用场景：不经过用户层 read/write 系统调用的 IO 路径
 * 触发场景包含：
 * 1. mmap() 内存映射读写文件
 * 2. 内核后台预读 readahead、页面预加载
 * 3. 内存紧张时，后台回写脏页 writepage
 * 4. ftruncate 文件截断、pagecache 页面回收
 * 5. 通用读写流程走块层封装接口 block_write_begin/block_write_end
 *
 * 与 simplefs_file_ops 区分：
 * simplefs_file_ops：用户直接调用 read()/write() 系统调用的裸读写逻辑，绕过页缓存；
 * simplefs_aops：内核页缓存机制专属回调，mmap、预读、后台刷脏页必须依赖这套接口；
 *
 * 内核版本兼容说明：
 * 1. 5.19 及以上内核提供 readahead 回调替代旧版 readpage，支持批量预读优化；
 * 2. 6.15 及以上内核移除独立 writepage 回调，由 folio 完整页写入接口统一接管，无需注册writepage；
 * 3. write_begin / write_end 是跨版本通用页缓存写入前后置钩子，用于块预分配、inode元数据更新、截断回收磁盘块
 *
 * .readahead / .readpage：页面读入回调，内核缺页、预读时加载磁盘数据到page cache
 * .writepage：脏页回写磁盘回调，内存压力大时内核自动调用刷脏
 * .write_begin：页写入前置钩子，写入页缓存前校验空间、预分配磁盘块
 * .write_end：页写入后置钩子，更新inode大小/时间、文件截断时释放多余extent数据块
 */
const struct address_space_operations simplefs_aops = {
    #if SIMPLEFS_AT_LEAST(5, 19, 0)
        // 内核5.19+：批量预读回调，支持一次读取多个连续页面，性能优于单页readpage
        .readahead = simplefs_readahead,
    #else
        // 低于5.19旧内核：单页面读取回调，缺页异常时单次加载一个page
        .readpage = simplefs_readpage,
    #endif
    #if !SIMPLEFS_AT_LEAST(6, 15, 0)
        // 内核小于6.15版本：注册脏页回写回调，单独处理单页刷盘
        .writepage = simplefs_writepage,
    #endif
        // 页缓存写入前置处理：分配文件所需磁盘块、校验最大文件尺寸、校验空闲块充足
        .write_begin = simplefs_write_begin,
        // 页缓存写入后置处理：更新inode元数据、文件扩容、文件截断时释放闲置extent块
        .write_end = simplefs_write_end,
    };
    

/**
 * simplefs_file_ops - SimpleFS 普通文件的 VFS 文件操作回调表
 * 该结构体挂载到 regular file 类型 inode 的 i_fop，用户态调用 open/read/write/lseek/fsync
 * 等系统调用时，VFS 会分发执行此处注册的对应回调函数。
 * 注意：本套 read/write 不走 address_space 页缓存流程，属于裸块读写；
 * 页缓存相关操作（mmap、预读、后台回写）由 simplefs_aops 单独处理。
 */
const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,               // 归属当前内核模块，防止模块在文件打开期间被卸载
    .open = simplefs_open,              // 文件打开回调，处理 O_TRUNC 截断清空文件数据块、释放 extent
    .read = simplefs_read,              // 自定义文件读实现，绕过页缓存，直接操作磁盘 buffer_head 读取数据
    .write = simplefs_write,            // 自定义文件写实现，手动分配 extent 数据块、写入磁盘并更新 inode 元数据
    .llseek = generic_file_llseek,      // 复用内核通用文件偏移跳转实现，无需自行实现 seek 逻辑
    .fsync = generic_file_fsync,        // 复用内核通用文件刷盘逻辑，同步文件脏数据到磁盘
};

