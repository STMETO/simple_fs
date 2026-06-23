#include <linux/fs.h>
#include <linux/kernel.h>

#include "simplefs.h"

/**
 * simplefs_ext_search - 在文件Extent索引数组中查找指定逻辑块号对应的Extent条目
 * @index: 文件对应的Extent索引块结构体，内部有序存储多条有效Extent区间
 * @iblock: 需要查找的文件内部逻辑块号（文件偏移换算出的逻辑块编号）
 *
 * 整体逻辑分为两段二分查找：
 * 1. 第一轮全域二分：扫描全部Extent数组，定位第一个空闲Extent条目的下标boundary；
 *    [0, boundary-1] 区间为已使用、有序排列的有效Extent；[boundary, MAX] 为空闲条目。
 * 2. 第二轮区间二分：仅在[0, boundary-1]有效Extent范围内二分检索iblock；
 *    若iblock落在某条Extent的连续逻辑块区间内，直接返回该Extent数组下标；
 *    二分循环结束后单独校验最后一条候选Extent，避免漏判。
 *
 * 返回值规则：
 * 1. 找到包含iblock的有效Extent：返回该Extent在数组中的下标；
 * 2. 文件无任何有效Extent（boundary=0）：返回0，代表首个空闲条目；
 * 3. 存在有效Extent，但iblock不匹配任意区间：返回boundary（第一个空闲Extent下标，上层用于新增Extent）；
 * 4. Extent数组全部占满无空闲位置：返回SIMPLEFS_MAX_EXTENTS，上层可判断无法新增区间。
 *
 * 约束前提：所有有效Extent条目严格按照起始逻辑块号iblock升序存储，保证二分查找有效。
 */

uint32_t simplefs_ext_search(struct simplefs_file_ei_block *index, uint32_t iblock)
{
    // 二分查找边界变量
    uint32_t start = 0;
    uint32_t end = SIMPLEFS_MAX_EXTENTS - 1;
    uint32_t boundary;  // 第一个空闲extent的下标，也是已使用extent的右边界
    uint32_t end_block; // 最后一个已使用extent的起始块号
    uint32_t end_len;   // 最后一个已使用extent的长度

    /****************************************************************************
    * 第一轮二分：找出数组中第一个未使用的extent下标
    * 约定：extent.ee_start == 0 代表该条目空闲未占用
    ***************************************************************************/
    while (start < end) {
        // 防溢出的二分取中点
        uint32_t mid = start + (end - start) / 2;
        if (index->extents[mid].ee_start == 0) {
            // mid位置空闲，向左缩小区间，找更靠前的空闲位
            end = mid;
        } else {
            // mid已占用，空闲位一定在右侧
            start = mid + 1;
        }
    }

    // 循环结束后 start == end，判断该位置是否空闲
    if (index->extents[end].ee_start == 0) {
        boundary = end; // end是第一个空闲extent下标
    } else {
        /* File index full：全部extent都被占用，无空位 */
        boundary = end + 1;
    }

    if (boundary == 0)
    // boundary=0说明第0号extent就是空闲，整个文件没有任何已分配块，直接返回空闲位
    return boundary;

    /****************************************************************************
    * 第二轮二分：在 [0, boundary-1] 所有已使用extent中查找目标块 iblock
    * 所有extent按磁盘块升序有序存储，才能二分
    ***************************************************************************/
    start = 0;
    end = boundary - 1; // 仅遍历已占用区间
    while (start < end) {
        uint32_t mid = start + (end - start) / 2;
        // 当前中点extent的起始块、连续长度
        uint32_t block = index->extents[mid].ee_block;
        uint32_t len = index->extents[mid].ee_len;

        // iblock落在当前extent连续区间内，直接返回下标mid
        if (iblock >= block && iblock < block + len) {
            return mid;
        }
        // 目标块在当前extent左边，收缩右边界
        if (iblock < block) {
            end = mid;
        } else {
            // 目标块在当前extent右边，收缩左边界
            start = mid + 1;
        }
    }

    /****************************************************************************
    * 二分循环结束 start==end，单独校验最后一个候选extent
    ***************************************************************************/
    end_block = index->extents[end].ee_block;
    end_len = index->extents[end].ee_len;
    // 目标块落在最后这个extent区间内，返回end下标
    if (iblock >= end_block && iblock < end_block + end_len)
        return end;

    // 没找到包含iblock的extent，返回第一个空闲extent下标，供上层写入新extent
    if (boundary < SIMPLEFS_MAX_EXTENTS)
        return boundary;
    // extent数组已满，无空闲位置，返回超出最大条目数的boundary
    return boundary;
}

