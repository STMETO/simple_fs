#ifndef SIMPLEFS_BITMAP_H
#define SIMPLEFS_BITMAP_H

#include <linux/bitmap.h>
#include "simplefs.h"

/**
 * get_first_free_bits - 查找连续len个空闲bit，并标记为占用
 * @freemap: 内存中位图数组
 * @size: 位图总bit数量
 * @len: 需要连续分配的空闲bit个数
 * 返回：成功返回起始bit号(≥1)；无连续空闲返回0
 * 逻辑：只遍历bit=1（空闲）的位置，统计连续空闲长度，凑够len就批量清0（占用）
 */
static inline uint32_t get_first_free_bits(unsigned long *freemap,
                                           unsigned long size,
                                           uint32_t len)
{
    uint32_t bit, prev = 0, count = 0;
    // 遍历位图中所有值为1（空闲）的bit
    for_each_set_bit (bit, freemap, size) {
        // 当前bit和上一个空闲bit不连续，连续计数器重置
        if (prev != bit - 1)
            count = 0;
        prev = bit;
        count++; // 连续空闲bit+1
        // 凑够需要的连续长度
        if (++count == len) {
            // 批量将[bit-len+1, bit]共len个bit置0（标记占用）
            bitmap_clear(freemap, bit - len + 1, len);
            return bit - len + 1; // 返回连续空闲段起始位置
        }
    }
    // 遍历完未找到足够连续空闲bit
    return 0;
}

/**
 * get_free_inode - 分配1个空闲inode
 * @sbi: simplefs超级块私有信息
 * 返回：inode号；0表示无空闲inode
 */
static inline uint32_t get_free_inode(struct simplefs_sb_info *sbi)
{
    // 分配连续1个空闲inode位
    uint32_t ret = get_first_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, 1);
    if (ret)
        sbi->nr_free_inodes--; // 空闲inode计数减一
    return ret;
}

/**
 * get_free_blocks - 分配连续len个磁盘块，清零并落盘
 * @sb: 内核超级块结构体
 * @len: 需要连续分配的块数量
 * 返回：起始块号；0表示分配失败
 * 失败兜底：读取块失败时回滚位图与空闲计数
 */
static inline uint32_t get_free_blocks(struct super_block *sb, uint32_t len)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    // 位图查找连续len个空闲块
    uint32_t ret = get_first_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, len);
    uint32_t i;
    if (!ret)
        return 0;

    sbi->nr_free_blocks -= len;
    struct buffer_head *bh;
    // 循环读取每一块、清零、刷盘
    for (i = 0; i < len; i++) {
        bh = sb_bread(sb, ret + i); // 读取磁盘块到buffer cache,根据超级块 + 块号，去块缓存查找该块；
        if (!bh) {
            pr_err("get_free_blocks: sb_bread failed for block %d\n", ret + i);
            // 读取失败：回滚位图，把刚占用的bit全部置1（恢复空闲）
            bitmap_set(sbi->bfree_bitmap, ret, len);
            sbi->nr_free_blocks += len;
            return 0;
        }
        memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE); // 块内容清零
        mark_buffer_dirty(bh); // 标记缓存为脏，需要写回磁盘
        sync_dirty_buffer(bh); // 同步刷脏块到磁盘（同步IO，阻塞）
        brelse(bh); // 释放buffer_head引用计数
    }
    return ret;
}

/**
 * put_free_bits - 回收连续len个bit，标记为空闲(bit置1)
 * @freemap: 位图
 * @size: 位图总bit数
 * @i: 起始bit号
 * @len: 回收长度
 * 返回：0成功；-1起始+长度超出位图范围，非法
 */
static inline int put_free_bits(unsigned long *freemap,
                                unsigned long size,
                                uint32_t i,
                                uint32_t len)
{
    // 边界校验：回收区间不能超过位图总位数
    if (i + len - 1 > size)
        return -1;
    // 批量将[i, i+len-1]置1，标记空闲
    bitmap_set(freemap, i, len);
    return 0;
}

/**
 * put_inode - 回收单个inode，标记空闲
 * @sbi: 超级块私有信息
 * @ino: 待回收inode号
 */
static inline void put_inode(struct simplefs_sb_info *sbi, uint32_t ino)
{
    // 回收失败直接返回，不修改计数
    if (put_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, ino, 1))
        return;
    sbi->nr_free_inodes++; // 空闲inode计数+1
}

/**
 * put_blocks - 回收连续len个磁盘块，标记空闲
 * @sbi: 超级块私有信息
 * @bno: 待回收起始块号
 * @len: 连续块数量
 */
static inline void put_blocks(struct simplefs_sb_info *sbi,
                              uint32_t bno,
                              uint32_t len)
{
    if (put_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, bno, len))
        return;
    sbi->nr_free_blocks += len;
}

#endif /* SIMPLEFS_BITMAP_H */
