#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/mm.h>
#include <linux/math64.h>
#include <linux/interrupt.h>  //V1.01新增部份：用於中斷處理
#include <linux/vmalloc.h>    //V1.01新增部份：用於vmalloc
#include <linux/workqueue.h>  //V1.01新增部份：用於工作隊列
#include <linux/string.h>
#include <linux/cred.h>

#define DEVICE_NAME "sysmonitor"
#define CLASS_NAME "sysmon"
#define BUF_LEN 100
#define SUCCESS 0
#define PROC_NAME "sysmonitor_info"  // 新增：定義proc文件名
#define CUSTOM_IRQ 42  // 新增1.02： 使用一個不太可能被系統使用的軟中斷號

static int major_number; //這個變量用於存儲設備的主設備號。在Linux設備驅動模型中，每個設備都有一個主設備號和次設備號。主設備號是識別設備驅動程序的關鍵。它允許內核將設備文件操作映射到正確的驅動程序函數。
static struct class* sysmonitor_class = NULL;
static struct timer_list update_timer;
static struct tasklet_struct irq_tasklet; // 新增1.02


static int Device_Open = 0;
static char kernel_buffer[BUF_LEN];

//V1.01新增部份
static void *large_buffer;  // 新增：用於演示vmalloc的使用
static struct work_struct update_work;  // 新增：用於演示工作隊列的使用
static struct device* sysmonitor_device = NULL; //V1.02新增部份
static struct proc_dir_entry *proc_sysmonitor = NULL; //V1.02新增部份

static unsigned long long last_idle = 0, last_total = 0; //V1.03新增部份

/* 與update_sysinfo和sysmonitor_proc_show配合 */
struct sysinfo_data {
    unsigned long mem_total;
    unsigned long mem_free;
    int cpu_usage;
    int running_processes;
};

static struct sysinfo_data current_info;

static int count_running_processes(void)
{
    struct task_struct *task;
    int count = 0;

    rcu_read_lock();  //RCU是Linux內核中用於同步的一種機制，可以在不阻塞讀操作的情況下進行更新
    /*在遍歷過程中，進程列表可能被修改（新進程創建、舊進程終止）。
    沒有 RCU，可能會訪問到已經被釋放的內存，導致系統崩潰。*/
    for_each_process(task) {
        if (task_is_running(task))
            count++;
    }
    rcu_read_unlock();

    return count;
}

static int get_cpu_usage(void)
{
    struct file *file;
    char buf[256];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total, idle_time, usage = 0;
    int ret;

    file = filp_open("/proc/stat", O_RDONLY, 0);
    if (IS_ERR(file)) {
        printk(KERN_ERR "SysMonitor: Failed to open /proc/stat\n");
        return 0;
    }

    ret = kernel_read(file, buf, sizeof(buf), &file->f_pos);
    if (ret > 0) {
        sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

        idle_time = idle + iowait;
        total = user + nice + system + idle + iowait + irq + softirq + steal;

        if (last_total != 0 && last_idle != 0 && total > last_total && idle_time > last_idle) {
            unsigned long long total_delta = total - last_total;
            unsigned long long idle_delta = idle_time - last_idle;
            usage = 100 - div64_u64(100 * idle_delta, total_delta);
        }

        last_total = total;
        last_idle = idle_time;
    }

    filp_close(file, NULL);
    return (int)usage;
}

static int sysmonitor_open(struct inode *inode, struct file *file)
{
    /*
     * 實現設備打開操作
     * 1. 檢查設備是否已經打開
     * 2. 初始化任何必要的資源
     * 3. 返回成功或錯誤代碼
     */

    if (Device_Open)
        return -EBUSY;

    Device_Open++;

    try_module_get(THIS_MODULE);
    printk(KERN_INFO "SysMonitor: Device opened\n");
    return SUCCESS;
}

static int sysmonitor_release(struct inode *inode, struct file *file)
{
    /*
     * 實現設備關閉操作
     * 1. 釋放在 open 中分配的資源
     * 2. 執行任何必要的清理操作
     * 3. 返回成功或錯誤代碼
     */
    Device_Open--;
    module_put(THIS_MODULE);
    printk(KERN_INFO "SysMonitor: Device closed\n");
    return 0;
}

static ssize_t sysmonitor_read(struct file *file, char __user *user_buffer, size_t count, loff_t *ppos)
{
    /*
     * 實現從設備讀取數據
     * 1. 檢查讀取的長度和偏移
     * 2. 準備要發送給用戶的數據
     * 3. 使用 copy_to_user 將數據複製到用戶空間
     * 4. 更新偏移並返回實際讀取的字節數
     */
    int len = strlen(kernel_buffer);

    if (count < len) //這個檢查是為了確保用戶提供的緩衝區足夠大，能夠容納所有要傳輸的數據
        return -EINVAL;

    if (*ppos != 0) // 如果偏移不為0，表示已經讀取過了
        return 0;

    if (copy_to_user(user_buffer, kernel_buffer, len)) {
        return -EFAULT; // 如果複製失敗，返回錯誤
    }

    *ppos = len; // 更新文件位置
    return len; // 返回複製的字節數
}

static ssize_t sysmonitor_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
    /*
     * 實現向設備寫入數據
     * 1. 檢查寫入的長度
     * 2. 使用 copy_from_user 從用戶空間複製數據
     * 3. 解析並執行接收到的命令
     * 4. 返回實際寫入的字節數或錯誤代碼
     */
    int bytes_to_copy = min(count, (size_t)(BUF_LEN - 1));

    if (copy_from_user(kernel_buffer, user_buffer, bytes_to_copy)) {
        printk(KERN_ERR "Failed to copy data from user space\n");
        return -EFAULT;
    }

    kernel_buffer[bytes_to_copy] = '\0'; // 確保字符串結束

    printk(KERN_INFO "Received from user: %s\n", kernel_buffer);

    // 這裡可以對接收到的數據進行進一步處理

    *ppos += bytes_to_copy;
    return bytes_to_copy;
}

static const struct file_operations sysmonitor_fops = {
    .open = sysmonitor_open,
    .release = sysmonitor_release,
    .read = sysmonitor_read,
    .write = sysmonitor_write,
};

static int sysmonitor_proc_show(struct seq_file *m, void *v)
{
    /*
     * 顯示系統信息
     * 1. 收集系統信息（CPU 使用率、內存使用情況等）
     * 2. 使用 seq_printf 將信息寫入 seq_file
     */
    printk(KERN_DEBUG "SysMonitor: Proc show called\n");

    seq_printf(m, "系統信息:\n");
    seq_printf(m, "總內存: %lu MB\n", current_info.mem_total);
    seq_printf(m, "可用內存: %lu MB\n", current_info.mem_free);
    seq_printf(m, "CPU使用率: %d%%\n", current_info.cpu_usage);
    seq_printf(m, "運行進程數: %d\n", current_info.running_processes);

    return 0;
}

static int sysmonitor_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, sysmonitor_proc_show, NULL);
}

static const struct proc_ops sysmonitor_proc_fops = {
    .proc_open = sysmonitor_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static void update_sysinfo(struct timer_list *t)
{
    /*
     * 定期更新系統信息
     * 1. 收集並更新系統信息
     * 2. 重新設置定時器
     */
    printk(KERN_DEBUG "SysMonitor: Updating system info\n");

    struct sysinfo si;
    int cpu_usage;

    si_meminfo(&si);

    current_info.mem_total = si.totalram * si.mem_unit / (1024 * 1024);
    current_info.mem_free = si.freeram * si.mem_unit / (1024 * 1024);
    
    cpu_usage = get_cpu_usage();
    if (cpu_usage < 0) {
        printk(KERN_ERR "SysMonitor: Failed to get CPU usage\n");
        cpu_usage = 0;  // 使用默認值
    }
    current_info.cpu_usage = cpu_usage;

    current_info.running_processes = count_running_processes();

    printk(KERN_DEBUG "SysMonitor: Updated - Total RAM: %lu MB, Free RAM: %lu MB, CPU Usage: %d%%, Running Processes: %d\n",
           current_info.mem_total, current_info.mem_free, current_info.cpu_usage, current_info.running_processes);

    mod_timer(&update_timer, jiffies + msecs_to_jiffies(1000));
}

// 新增1.02
static void irq_tasklet_func(unsigned long data)
{
    printk(KERN_INFO "SysMonitor: Tasklet function called\n");
    // 這裡可以執行中斷相關的操作
}

//V1.01新增部份：中斷處理函數
static irqreturn_t sysmonitor_interrupt(int irq, void *dev_id)
{
    pr_debug("SysMonitor: Interrupt received\n");
    tasklet_schedule(&irq_tasklet);
    return IRQ_HANDLED;
}

//V1.01新增部份：工作隊列處理函數
static void update_work_func(struct work_struct *work)
{
    pr_debug("SysMonitor: Performing delayed work\n");
    // 在這裡執行耗時的操作
}

static int __init sysmonitor_init(void)
{
    /*
     * 模塊初始化函數
     * 1. 分配主設備號
     * 2. 創建設備類
     * 3. 創建設備
     * 4. 初始化字符設備
     * 5. 創建 proc 入口
     * 6. 初始化並啟動定時器
     */
    struct cred *new;
    new = prepare_creds();
    if (!new) {
        printk(KERN_ERR "SysMonitor: Failed to prepare new credentials\n");
        return -ENOMEM;
    }
    new->fsuid = GLOBAL_ROOT_UID;
    new->fsgid = GLOBAL_ROOT_GID;
    commit_creds(new);

    int ret = 0;
    // 分配主設備號
    major_number = register_chrdev(0, DEVICE_NAME, &sysmonitor_fops);
    if (major_number < 0) {
        printk(KERN_ALERT "SysMonitor failed to register a major number\n");
        return major_number;
    }

    // 註冊設備類
    sysmonitor_class = class_create(CLASS_NAME);
    if (IS_ERR(sysmonitor_class)) {
        printk(KERN_ALERT "Failed to register device class\n");
        ret = PTR_ERR(sysmonitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return ret;
    }

    // 註冊設備驅動
    sysmonitor_device = device_create(sysmonitor_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(sysmonitor_device)) {
        printk(KERN_ALERT "Failed to create the device\n");
        ret = PTR_ERR(sysmonitor_device);
        class_destroy(sysmonitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return ret;
    }

    // 創建proc入口
    proc_sysmonitor = proc_create(PROC_NAME, 0, NULL, &sysmonitor_proc_fops);
    if (!proc_sysmonitor) {
        printk(KERN_ALERT "Failed to create proc entry\n");
        ret = -ENOMEM;
        device_destroy(sysmonitor_class, MKDEV(major_number, 0));
        class_destroy(sysmonitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return ret;
    }

    ///V1.01新增部份：分配大塊內存
    large_buffer = vmalloc(1024 * 1024);  // 分配1MB內存
    if (!large_buffer) {
        printk(KERN_ALERT "SysMonitor: Failed to allocate large buffer\n");
        ret = -ENOMEM;
        proc_remove(proc_sysmonitor);
        device_destroy(sysmonitor_class, MKDEV(major_number, 0));
        class_destroy(sysmonitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
    }

    //1.02新增
    tasklet_init(&irq_tasklet, irq_tasklet_func, 0);

    //V1.01新增部份：註冊中斷處理程序（示例使用IRQ 1，實際使用時應該使用正確的IRQ號）
    ret = request_irq(1, sysmonitor_interrupt, IRQF_SHARED, "sysmonitor", THIS_MODULE);
    if (ret) {
        printk(KERN_ALERT "SysMonitor: Failed to request IRQ\n");
        vfree(large_buffer);
        proc_remove(proc_sysmonitor);
        device_destroy(sysmonitor_class, MKDEV(major_number, 0));
        class_destroy(sysmonitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return ret;
    }
    // 初始化定時器
    // 在較新的內核版本中（4.15+）
    timer_setup(&update_timer, update_sysinfo, 0);

    // 或在較舊的內核版本中
    // setup_timer(&update_timer, update_sysinfo, 0);
    
    mod_timer(&update_timer, jiffies + msecs_to_jiffies(1000));

    //V1.01新增部份：初始化工作隊列
    INIT_WORK(&update_work, update_work_func);

    printk(KERN_INFO "SysMonitor: module loaded\n");
    return 0;
}

static void __exit sysmonitor_exit(void)
{
    /*
     * 模塊退出函數
     * 1. 刪除定時器
     * 2. 移除 proc 入口
     * 3. 刪除字符設備
     * 4. 刪除設備
     * 5. 刪除設備類
     * 6. 釋放主設備號
     */
    del_timer_sync(&update_timer);
    proc_remove(proc_sysmonitor);
    device_destroy(sysmonitor_class, MKDEV(major_number, 0));
    class_unregister(sysmonitor_class);
    class_destroy(sysmonitor_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    /*V1.01新增部份*/
    // 新增1.02：釋放中斷
    free_irq(CUSTOM_IRQ, THIS_MODULE);
    tasklet_kill(&irq_tasklet);
    // 新增：釋放大塊內存
    vfree(large_buffer);
    // 新增：取消掉所有未完成的工作
    cancel_work_sync(&update_work);

    printk(KERN_INFO "SysMonitor: module unloaded\n");
}

module_init(sysmonitor_init);
module_exit(sysmonitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Owen");
MODULE_DESCRIPTION("System Monitor and Control Driver");
MODULE_VERSION("1.04");