// 定义print系列日志统一前缀宏，所有pr_info/pr_err都会自动拼接模块名
// Linux 内核标准日志格式化宏，必须放在所有#include之前才会生效
// KBUILD_MODNAME：编译时自动赋值为本模块的文件名（这里就是fs）
/*
示例：
    代码中 pr_info("'%s' mount success\n", dev_name)
    实际输出日志：simplefs: '/dev/sda1' mount success
*/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

// 文件系统挂载回调，VFS在执行mount时自动调用，原型由struct file_system_type约定
struct dentry *simplefs_mount(struct file_system_type *fs_type, //指向我们定义的 simplefs_file_system_type，内核用来标记当前文件系统类型
                                int flags,  // 挂载标志位，来自 mount 命令参数：MS_RDONLY(只读)、MS_NOATIME、MS_SYNCHRONOUS 等挂载属性
                                const char *dev_name,
                                void *data) // 用户传入的挂载选项数据，mount -o xxx 携带的参数，字符串形式
{
    // mount_bdev：VFS提供的「块设备文件系统通用挂载工具函数」
    // 入参最后一个参数simplefs_fill_super是回调：读取磁盘、填充自定义超级块信息
    struct dentry *dentry =
    mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

    // IS_ERR 判断指针是否是内核错误码（负数编码成指针）
    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    // 返回文件系统根目录dentry给VFS，挂载完成
    return dentry;
}


// 文件系统卸载回调，对应 file_system_type->kill_sb
// 当整个simplefs分区所有文件/目录都关闭、无进程占用，内核执行umount时调用
void simplefs_kill_sb(struct super_block *sb)
{
    // SIMPLEFS_SB(sb) 容器宏：从通用super_block取出simplefs私有超级块信息结构体
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    // 内核版本兼容分支：不同内核版本操作日志块设备的API不同
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    // 内核 >= 6.9：日志块设备用struct file管理，通过fput释放文件引用
    if (sbi->s_journal_bdev_file)
        fput(sbi->s_journal_bdev_file);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    // 内核 6.7 ~ 6.8：日志块设备使用bdev句柄，调用bdev_release释放块设备引用
    if (sbi->s_journal_bdev_handle)
        bdev_release(sbi->s_journal_bdev_handle);
#endif

    // VFS标准接口：销毁块设备对应的super_block、释放内核通用超级块资源、释放块设备引用
    kill_block_super(sb);

    pr_info("unmounted disk\n");
}


// 定义simplefs文件系统类型描述结构体，VFS识别、挂载、卸载文件系统的核心接口
static struct file_system_type simplefs_file_system_type = {
    // 模块自身指针，用于内核维护模块引用计数
    // 只要有simplefs分区处于挂载状态，模块引用计数+1，阻止rmmod强制卸载导致崩溃
    .owner = THIS_MODULE,

    // 文件系统名称，执行 mount -t simplefs /dev/xxx 时VFS通过该字符串匹配驱动
    .name = "simplefs",             

    // 挂载回调函数：用户执行mount时VFS调用该函数，完成块设备打开、超级块初始化
    .mount = simplefs_mount,        

    // 卸载超级块回调：umount设备时内核调用，释放超级块私有资源、销毁超级块
    .kill_sb = simplefs_kill_sb,    

    // 文件系统特性标志：FS_REQUIRES_DEV 表示该文件系统必须绑定块设备才能挂载
    // 不能像tmpfs/proc那样无设备直接挂载，mount必须传入/dev/xx设备名
    .fs_flags = FS_REQUIRES_DEV,

    // 内核内部链表指针，用于把当前fs_type挂入VFS全局文件系统链表
    // register_filesystem()会自动修改next，手动置NULL仅初始化规范，无业务作用
    .next = NULL,
};



// __init：内核标记，函数仅模块加载时执行；内核可在初始化后释放该段内存节省空间
static int __init simplefs_init(void)
{
    int ret = simplefs_init_inode_cache(); // 1. 创建inode专属slab缓存
    if (ret) {                              
        pr_err("Failed to create inode cache\n");
        goto err;                           
    }

    // 2. 向内核VFS子系统注册我们自定义的simplefs文件系统类型
    ret = register_filesystem(&simplefs_file_system_type);
    if (ret) {                             
        pr_err("Failed to register file system\n");
        goto err_inode;                  
    }

    pr_info("module loaded\n");
    return 0; 

// 文件系统注册失败时的清理分支：缓存已经创建，需要先销毁缓存
err_inode:
    simplefs_destroy_inode_cache();
    /* RCU同步屏障：等待所有持有旧inode缓存对象的RCU读侧临界区全部退出 */
    /* 只有执行完rcu_barrier，才能保证slab内存完全释放，不会野指针 */
    rcu_barrier();
err:
    return ret;
}

// __exit：模块卸载时执行；仅编译进模块时生效，内置内核会丢弃该函数
static void __exit simplefs_exit(void)
{
    // 1. 先从VFS注销simplefs文件系统，内核不再识别"simplefs"类型
    int ret = unregister_filesystem(&simplefs_file_system_type);
    if (ret)
        pr_err("Failed to unregister file system\n");

    // 2. 销毁inode slab缓存
    simplefs_destroy_inode_cache();
    // 同步RCU，确保所有inode相关RCU访问完成，内存安全释放
    rcu_barrier();

    pr_info("module unloaded\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("a simple file system");
