#ifndef SIMPLEFS_H
#define SIMPLEFS_H

// 魔数就是一段固定特殊数值，存放在分区超级块头部（simplefs 存在 simplefs_sb_info.magic）
// 核心 4 大作用：
// 1. 识别分区文件系统类型  2. 基础合法性校验，防止误加载损坏数据  
// 3. 磁盘扫描、数据恢复定位文件系统  4. 格式化工具标记分区类型s
#define SIMPLEFS_MAGIC 0xDEADCELL   // 文件系统魔数

// 超级块所在块编号：文件系统第0个数据块固定存放超级块
#define SIMPLEFS_SB_BLOCK_NR 0

// 文件系统块大小：1 << 12 = 4096 Byte，即 4KB 块
#define SIMPLEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */

// 单个extent索引块最多能存放多少个extent项
// 块头部预留1个uint32_t存文件计数，剩余空间均分每个extent结构体大小
#define SIMPLEFS_MAX_EXTENTS \
    ((SIMPLEFS_BLOCK_SIZE - sizeof(uint32_t)) / sizeof(struct simplefs_extent))

// 单个extent最多连续管理8个物理数据块
// 注释说明：值不能接近uint32最大值防止溢出
#define SIMPLEFS_MAX_BLOCKS_PER_EXTENT 8 /* It can be ~(uint32) 0 */

// 单个extent能承载的最大数据容量：extent块数 × 单块4KB
#define SIMPLEFS_MAX_SIZES_PER_EXTENT \
    (SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_BLOCK_SIZE)

// 单个文件支持的最大文件尺寸
// 连续块数 × 单块大小 × 最大extent数量；强转uint64避免32位整型溢出
#define SIMPLEFS_MAX_FILESIZE                                          \
    ((uint64_t) SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_BLOCK_SIZE * \
     SIMPLEFS_MAX_EXTENTS)

// 文件名最大长度255字节（不含结束符/含自定义存储规则）
#define SIMPLEFS_FILENAME_LEN 255

// 单个目录块内最多存储多少个目录项simplefs_file
// 4KB块总大小 / 单个目录项结构体字节大小
#define SIMPLEFS_FILES_PER_BLOCK \
    (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_file))

// 一个extent（连续8个数据块）能容纳的目录项总数
// 单块目录项数量 × extent管理的连续块数
#define SIMPLEFS_FILES_PER_EXT \
    (SIMPLEFS_FILES_PER_BLOCK * SIMPLEFS_MAX_BLOCKS_PER_EXTENT)

// 单个目录下最多支持的子文件/子目录总数上限
// 单个extent容纳文件数 × 索引块最大extent条目数
#define SIMPLEFS_MAX_SUBFILES (SIMPLEFS_FILES_PER_EXT * SIMPLEFS_MAX_EXTENTS)
/* simplefs partition layout
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 */


#ifdef __KERNEL__
#include <linux/jbd2.h>  // Linux 内核日志块设备子系统
#endif

// 磁盘上持久存储的Inode结构体，存放文件/目录/软链接的底层元数据
struct simplefs_inode {
    uint32_t i_mode;   /* 文件类型与权限模式，如普通文件/目录/软链接、rwx权限 */
    uint32_t i_uid;    /* 文件所有者用户ID */
    uint32_t i_gid;    /* 文件所属用户组ID */
    uint32_t i_size;   /* 文件当前字节大小；目录此字段无实际意义 */
    uint32_t i_ctime;  /* inode变更时间戳，权限/属主/链接数修改时更新 */
    uint32_t i_atime;  /* 文件最近访问时间戳，读文件/进入目录时更新 */
    uint32_t i_mtime;  /* 文件内容修改时间戳，写入文件、新增删除目录项时更新 */
    uint32_t i_blocks; /* 当前文件占用磁盘块总数量（4KB为一块） */
    uint32_t i_nlink;  /* 硬链接计数，普通文件默认1，目录至少2(.和自身) */
    uint32_t ei_block; /* 指向extent索引块的物理块号，保存该文件/目录所有extent区段 */
    char i_data[32];   /* 短软链接专用缓冲区：路径长度≤31时直接存在这里，无需额外数据块 */
};


// 单个4KB inode存储块里，最多能存放多少个磁盘inode结构体
#define SIMPLEFS_INODES_PER_BLOCK \
    (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))


#ifdef __KERNEL__
#include <linux/version.h>
/* compatibility macros */
#define SIMPLEFS_AT_LEAST(major, minor, rev) \
    LINUX_VERSION_CODE >= KERNEL_VERSION(major, minor, rev)     // 当前内核版本 ≥ 指定版本
#define SIMPLEFS_LESS_EQUAL(major, minor, rev) \
    LINUX_VERSION_CODE <= KERNEL_VERSION(major, minor, rev)     // 当前内核版本 ≤ 指定版本

/*
 * simplefs_inode_info：内核内存层专属inode包装结构体
 * 作用：磁盘读出的simplefs私有元数据 + Linux标准VFS inode封装在一起
 * Linux VFS规定：自定义文件系统必须把struct inode放在自定义inode结构体末尾，
 * 配合container_of宏，可通过VFS inode反向拿到simplefs私有信息
 */
struct simplefs_inode_info {
    uint32_t ei_block;      /* 磁盘extent索引块的物理块号，对应磁盘simplefs_inode的ei_block字段 */
    char i_data[32];        /* 缓存磁盘inode内的短软链接路径，和磁盘inode的i_data一一对应 */
    struct inode vfs_inode; /* Linux内核标准VFS通用inode结构，存放系统统一的文件元数据(权限、大小、时间等) */
};


// Extent区段描述符，记录一段连续磁盘块的映射关系
// 存放在 simplefs_file_ei_block（extent索引块）的extents数组中
struct simplefs_extent {
    uint32_t ee_block;  /* 逻辑起始块号：当前文件/目录内的起始块编号 */
    uint32_t ee_len;    /* 该extent包含的连续块总数，最大不超过SIMPLEFS_MAX_BLOCKS_PER_EXTENT(8) */
    uint32_t ee_start;  /* 物理起始块号：磁盘上真实起始的4KB块编号 */
    uint32_t nr_files;  /* 仅目录场景有效：本段extent所有目录块里一共存在多少个目录项 */
};


// 一个 4KB 块专门用来存放多个 extent 条目
// Extent索引块：存放在磁盘数据区，每个inode（文件/目录）对应一块该结构
// 作用：保存一个文件/目录全部的extent区段映射信息
struct simplefs_file_ei_block {
    uint32_t nr_files;                     /* 当前目录下所有子文件/子目录总数量；普通文件场景此字段无业务含义 */
    struct simplefs_extent extents[SIMPLEFS_MAX_EXTENTS];  /* 区段数组，最多存储SIMPLEFS_MAX_EXTENTS条extent记录，每条描述一段连续磁盘块 */
};


// 目录项结构体：保存在 simplefs_dir_block 目录块中，代表文件夹内一个文件/子目录条目
struct simplefs_file {
    uint32_t inode;                /* 该文件/子目录对应的inode编号，用于找到磁盘上的inode元数据 */
    uint32_t nr_blk;               /* 保留字段/备用块计数，当前simplefs未使用，预留扩展 */
    char filename[SIMPLEFS_FILENAME_LEN]; /* 文件名缓冲区，最大长度255字节，以'\0'结尾 */
};


// 目录块结构体：磁盘上标准4KB数据块，专门存放一个目录下的多条文件条目
// 属于目录专用数据块，由 simplefs_extent 区段统一管理
struct simplefs_dir_block {
    uint32_t nr_files;   /* 当前这个目录块内实际存储的 simplefs_file 目录项数量 */

    struct simplefs_file files[SIMPLEFS_FILES_PER_BLOCK];
    /* 目录项数组，整块剩余空间全部用来存文件条目，数组最大容量由宏 SIMPLEFS_FILES_PER_BLOCK 计算得出 */
};


/* superblock functions 文件系统超级块相关回调函数 */
/**
 * simplefs_fill_super - 挂载文件系统核心函数，填充super_block结构体
 * @sb: 内核传入的VFS超级块对象，用来保存本文件系统全局信息
 * @data: 挂载时传入的挂载参数（mount -o xxx 后的选项）
 * @silent: 静默标志，非0表示禁止打印错误日志
 * 返回值：成功返回0；失败返回负错误码(-EINVAL/-ENOMEM等)
 * 工作流程：读取磁盘0号超级块、校验魔数、初始化内存sb_info、加载位图、初始化文件系统全局上下文
 */
int simplefs_fill_super(struct super_block *sb, void *data, int silent);

/**
 * simplefs_kill_sb - 卸载文件系统销毁资源回调
 * @sb: 待卸载的VFS超级块对象
 * 无返回值
 * 工作流程：释放内存中的inode/块空闲位图、销毁journal日志资源、释放simplefs_sb_info私有数据、清理超级块占用资源
 */
void simplefs_kill_sb(struct super_block *sb);


/* inode functions */
/* inode functions 与inode缓存、inode读取相关接口 */
/**
 * simplefs_init_inode_cache - 创建本文件系统专用inode slab缓存
 * 返回值：成功返回0，失败返回负错误码
 * 作用：内核slab分配器，预分配simplefs_inode_info内存池，提升inode创建释放性能
 */
int simplefs_init_inode_cache(void);

/**
 * simplefs_destroy_inode_cache - 销毁inode slab缓存
 * 挂载模块卸载时调用，释放缓存占用的内核内存，避免内存泄漏
 */
void simplefs_destroy_inode_cache(void);

/**
 * simplefs_iget - 根据inode号ino，从磁盘读取并构建VFS inode对象
 * @sb：文件系统超级块
 * @ino：要读取的inode编号
 * 返回值：成功返回struct inode*；失败返回ERR_PTR(负数错误码)
 * 流程：查找缓存→缓存未命中则读磁盘inode→填充simplefs_inode_info与VFS inode→返回inode
 */
struct inode *simplefs_iget(struct super_block *sb, unsigned long ino);


/* dentry function */
/**
 * simplefs_mount - 文件系统挂载入口函数，VFS标准mount回调
 * @fs_type：当前文件系统类型对象，包含simplefs整套操作集
 * @flags：挂载标志位，如MS_RDONLY只读、MS_SYNCHRONOUS同步写等
 * @dev_name：待挂载的块设备路径，例："/dev/sda1"
 * @data：mount -o 传入的挂载选项参数字符串
 * 返回值：成功返回根目录dentry；失败返回ERR_PTR(负错误码)
 * 作用：分配超级块、调用fill_super、生成根目录dentry，完成挂载流程
 */
struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data);


/* file functions */
// 普通文件（文本/二进制文件）的文件操作函数集，挂载到inode->i_fop
extern const struct file_operations simplefs_file_ops;

// 目录（文件夹）的文件操作函数集，ls、mkdir、lookup等目录操作走这套接口
extern const struct file_operations simplefs_dir_ops;

// 页缓存地址空间操作集，负责文件数据页的读写、回写、磁盘块映射
extern const struct address_space_operations simplefs_aops;


/* extent functions */
/**
 * simplefs_ext_search - 在extent索引块中查找指定逻辑块iblock对应的物理区段信息
 * @index: 指向文件/目录的extent索引块 simplefs_file_ei_block
 * @iblock: 需要查找的文件内部逻辑块号
 * 返回值：找到则返回对应extent在extents数组的下标；未找到返回无效值（如极大uint32标记缺失）
 * 功能：遍历索引块内所有extent，判断iblock是否落在某条extent的逻辑块区间内
 */
extern uint32_t simplefs_ext_search(struct simplefs_file_ei_block *index,
                                    uint32_t iblock);


/* Getters for superblock and inode 辅助宏：快速取出文件系统私有数据 */
/**
 * SIMPLEFS_SB - 从VFS标准超级块struct super_block取出simplefs私有超级块simplefs_sb_info
 * @sb: 内核标准struct super_block指针
 * 原理：文件系统私有全局信息都挂载在sb->s_fs_info成员上
 */
#define SIMPLEFS_SB(sb) (sb->s_fs_info)

/**
 * SIMPLEFS_INODE - 从标准VFS struct inode反向取出simplefs自定义inode_info私有结构体
 * @inode: 内核标准VFS inode指针
 * 依赖container_of内核宏，利用结构体成员地址反推外层结构体首地址
 * 前提：simplefs_inode_info把struct inode vfs_inode放在结构体最后一个成员
 */
#define SIMPLEFS_INODE(inode) \
    (container_of(inode, struct simplefs_inode_info, vfs_inode))


#endif /* __KERNEL__ */

// 超级块信息结构体
// simplefs 内存版私有超级块信息结构体
// 挂载时从磁盘超级块读取数据填充，存放整个文件系统全局运行时信息
struct simplefs_sb_info {
    uint32_t magic;            /* 文件系统魔数，固定为0xDEADCELL(0xDEADCE11)，校验分区合法性 */

    uint32_t nr_blocks;       /* 分区总4KB块数量，包含超级块、inode区、位图、数据块全部 */
    uint32_t nr_inodes;       /* 文件系统支持的inode总容量上限，格式化时确定 */

    uint32_t nr_istore_blocks;/* Inode存储区占用多少个4KB块 */
    uint32_t nr_ifree_blocks; /* inode空闲位图占用的块数，位图用来标记哪些inode未被占用 */
    uint32_t nr_bfree_blocks; /* 数据块空闲位图占用的块数，位图标记哪些4KB数据块空闲可用 */

    uint32_t nr_free_inodes;  /* 当前剩余空闲inode总数，创建文件时消耗，删除文件时回收 */
    uint32_t nr_free_blocks;  /* 当前剩余空闲数据块总数，写入文件分配块时消耗 */

    unsigned long *ifree_bitmap; /* 内核内存中缓存的inode空闲位图指针，运行时常驻内存 */
    unsigned long *bfree_bitmap; /* 内核内存中缓存的数据块空闲位图指针，常驻内存 */

#ifdef __KERNEL__
    journal_t *journal;       /* jbd2日志句柄，开启日志模式时用于事务、崩溃恢复 */
    struct block_device *s_journal_bdev; /* 5.10及以上内核：外部日志对应的块设备对象 */
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    struct file *s_journal_bdev_file;    /* 6.11+新版内核：外部日志设备的file句柄 */
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    struct bdev_handle *s_journal_bdev_handle; /* 6.7 ~ 6.10：块设备新版句柄结构 */
#endif /* SIMPLEFS_AT_LEAST */
#endif /* __KERNEL__ */
};


#endif /* SIMPLEFS_H */
