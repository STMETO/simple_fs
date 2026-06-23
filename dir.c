#define pr_fmt(fmt) "simplefs: " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

/**
 * simplefs_iterate - VFS目录遍历回调函数，对应file_operations->iterate_shared
 * @dir: 打开的目录对应的struct file对象
 * @ctx: 目录遍历上下文，保存遍历游标pos、存放待输出目录项
 *
 * 功能：读取磁盘上simplefs目录下所有子文件/子目录，通过dir_emit上报给VFS，供readdir/ls使用
 * 遍历游标ctx->pos规则：
 * pos=0 代表当前项是 "." (当前目录)
 * pos=1 代表当前项是 ".." (父目录)
 * pos>=2 才是目录里真实存储的普通子文件
 * 返回值：成功返回0；读磁盘IO失败返回-EIO；非目录入参返回-ENOTDIR
 */
static int simplefs_iterate(struct file *dir, struct dir_context *ctx)
{
    // 从file对象取出对应的inode（目录inode）
    struct inode *inode = file_inode(dir);
    // 取出simplefs自定义inode私有信息，存放目录的extent索引块号
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    // 当前挂载的超级块
    struct super_block *sb = inode->i_sb;
    // bh：目录extent索引块的buffer_head缓存
    // bh2：遍历过程中每个目录数据块的缓存
    struct buffer_head *bh = NULL, *bh2 = NULL;
    // eblock：磁盘索引块内存映射，extent数组管理目录数据块
    struct simplefs_file_ei_block *eblock = NULL;
    // dblock：单个目录数据块，存放多条子文件条目
    struct simplefs_dir_block *dblock = NULL;
    // ei: extent数组循环下标；bi:单个extent内数据块下标；fi:块内文件条目下标
    int ei = 0, bi = 0, fi = 0;
    // 函数返回码，默认0成功
    int ret = 0;

    // 安全校验：确保传入的file确实是目录，不是普通文件/软链接
    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    // 游标上限判断：最大支持SIMPLEFS_MAX_SUBFILES个子文件，再加.和..两项
    // 游标超过最大值，说明全部遍历完毕，直接返回
    if (ctx->pos > SIMPLEFS_MAX_SUBFILES + 2)
        return 0;

    // VFS标准工具函数：自动向ctx提交 "." 和 ".." 两个固定目录项
    // 返回false代表上层用户不再需要更多目录项，直接终止遍历
    if (!dir_emit_dots(dir, ctx))
        return 0;

    // 根据inode里记录的ei_block块号，读取目录的Extent索引块到page cache
    bh = sb_bread(sb, ci->ei_block);
    // 磁盘读取失败，返回IO错误码
    if (!bh)
        return -EIO;
    // 将缓存缓冲区强转为extent索引块结构体，操作extent数组
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    // 游标pos-2等于目录总文件数：所有子文件已经遍历完成，无需继续读取数据
    if (ctx->pos - 2 == eblock->nr_files)
        goto release_bh;

    // 剩余还需要遍历的真实子文件数量
    int remained_nr_files = eblock->nr_files - (ctx->pos - 2);
    // offset：跳过前面已经遍历过的子文件条目，从当前游标对应的条目开始读取
    int offset = ctx->pos - 2;

    /****************************************************************************
     * 第一层循环：遍历extent数组，快速跳过前面extent里已遍历完的文件条目
     * 找到游标offset落在哪个extent区间，定位起始extent下标ei
     ***************************************************************************/
    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        // ee_start=0代表该extent未使用，直接跳过
        if (eblock->extents[ei].ee_start == 0)
            continue;
        // 当前extent存储的文件总数小于offset，说明目标条目不在此extent
        if (offset > eblock->extents[ei].nr_files) {
            // 减去当前extent的文件数量，继续往后找
            offset -= eblock->extents[ei].nr_files;
        } else {
            // offset落在当前extent中，停止循环，ei就是起始extent下标
            break;
        }
    }

    /****************************************************************************
     * 第二层循环：从定位好的ei开始，遍历所有有效extent
     ***************************************************************************/
    for (; remained_nr_files && ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        // 跳过未使用的空extent
        if (eblock->extents[ei].ee_start == 0)
            continue;

        /************************************************************************
         * 第三层循环：遍历单个extent内所有连续目录数据块
         ************************************************************************/
        for (bi = 0; bi < eblock->extents[ei].ee_len && remained_nr_files; bi++) {
            // 读取extent中第bi个目录数据块
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            // 读块失败，标记错误，跳转到统一缓存释放逻辑
            if (!bh2) {
                ret = -EIO;
                goto release_bh;
            }
            // 缓冲区映射为目录数据块结构体
            dblock = (struct simplefs_dir_block *) bh2->b_data;

            // 当前数据块总条目小于剩余偏移offset，整块全部跳过
            if (offset > dblock->nr_files) {
                offset -= dblock->nr_files;
                brelse(bh2); // 释放当前块缓存
                bh2 = NULL;
                continue;
            }

            /********************************************************************
             * 第四层循环：遍历单块内所有子文件条目
             ******************************************************************/
            for (fi = 0; fi < SIMPLEFS_FILES_PER_BLOCK;) {
                // inode!=0 代表该条目是有效文件/目录
                if (dblock->files[fi].inode != 0) {
                    // offset>0：代表该条目在游标之前，仅跳过计数，不输出
                    if (offset) {
                        offset--;
                    } else {
                        // offset=0：到达需要输出的目标条目
                        remained_nr_files--;
                        // dir_emit：把文件名、inode号、文件类型提交给VFS
                        // 返回false表示用户层不再需要更多目录项，直接退出
                        if (!dir_emit(ctx, dblock->files[fi].filename,
                                      SIMPLEFS_FILENAME_LEN,
                                      dblock->files[fi].inode, DT_UNKNOWN)) {
                            brelse(bh2);
                            bh2 = NULL;
                            goto release_bh;
                        }
                        // 游标向后移动一位，标记本条已遍历
                        ctx->pos++;
                    }
                }
                // nr_blk：当前文件占用多少个条目槽位，直接跳跃下标，不逐条遍历
                fi += dblock->files[fi].nr_blk;
            }
            // 释放当前目录数据块缓存，避免buffer_head泄漏
            brelse(bh2);
            bh2 = NULL;
        }
    }

// 统一资源释放出口：释放目录extent索引块缓存
release_bh:
    brelse(bh);

    return ret;
}


// 目录文件专属操作函数集：当打开simplefs目录时，VFS使用这组回调
const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    // 目录迭代回调：执行ls、readdir等读取目录内容时，VFS自动调用simplefs_iterate
    // iterate_shared是新版内核推荐接口，支持共享模式遍历目录，线程安全
    .iterate_shared = simplefs_iterate,
};

