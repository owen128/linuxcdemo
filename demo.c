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
    struct task_struct *task; //是Linux內核中表示進程的結構體
    static int count = 0;

    rcu_read_lock();  //RCU是Linux內核中用於同步的一種機制，可以在不阻塞讀操作的情況下進行更新
    /*在遍歷過程中，進程列表可能被修改（新進程創建、舊進程終止）。
    沒有 RCU，可能會訪問到已經被釋放的內存，導致系統崩潰。*/
    for_each_process(task) {
        if (task->state == TASK_RUNNING) //TASK_RUNNING Linux 內核源碼中定義的一個常量 表示進程正在運行或準備運行
            count++;
    }
    rcu_read_unlock();

    return count;
}

static int get_cpu_usage(void)
{
    static int usage;
    unsigned long long idle_time, total_time;
    struct kernel_cpustat *kcpustat;
    
    // 獲取在線CPU的數量
    get_online_cpus();
    kcpustat = &kcpustat_cpu(0); // 獲取第一個CPU（CPU 0）的統計信息
    idle_time = kcpustat->cpustat[CPUTIME_IDLE]; // 獲取空閒時間
    // 計算總時間（所有CPU狀態的時間總和）
    total_time = idle_time + kcpustat->cpustat[CPUTIME_USER] +
                 kcpustat->cpustat[CPUTIME_NICE] +
                 kcpustat->cpustat[CPUTIME_SYSTEM] +
                 kcpustat->cpustat[CPUTIME_IRQ] +
                 kcpustat->cpustat[CPUTIME_SOFTIRQ] +
                 kcpustat->cpustat[CPUTIME_STEAL];
    put_online_cpus(); // 釋放在線CPU的引用計數

    if (total_time > idle_time)
        usage = div64_ul(100 * (total_time - idle_time), total_time); // 使用率 = (總時間 - 空閒時間) / 總時間 * 100
    else
        usage = 0;

    return usage;
}

static int sysmonitor_open(struct inode *inode, struct file *file)
{
    /*
     * 實現設備打開操作
     * 1. 檢查設備是否已經打開
     * 2. 初始化任何必要的資源
     * 3. 返回成功或錯誤代碼
     */
    static int counter = 0;

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
    struct sysinfo si;
    si_meminfo(&si);

    current_info.mem_total = si.totalram * si.mem_unit / (1024 * 1024);
    current_info.mem_free = si.freeram * si.mem_unit / (1024 * 1024);
    current_info.cpu_usage = get_cpu_usage();
    current_info.running_processes = count_running_processes();

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
    int ret;
    // 分配主設備號
    if ((major_number = register_chrdev(0, DEVICE_NAME, &sysmonitor_fops)) < 0) {
        printk(KERN_ALERT "SysMonitor failed to register a major number\n");
        ret = major_number;
        goto fail_chrdev;
    }

    // 註冊設備類
    sysmonitor_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(sysmonitor_class)) {
        printk(KERN_ALERT "Failed to register device class\n");
        ret = PTR_ERR(sysmonitor_class);
        goto fail_class;
    }

    // 註冊設備驅動
    sysmonitor_device = device_create(sysmonitor_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(sysmonitor_device)) {
        printk(KERN_ALERT "Failed to create the device\n");
        ret = PTR_ERR(sysmonitor_device);
        goto fail_device;
    }

    // 創建proc入口
    proc_sysmonitor = proc_create(PROC_NAME, 0, NULL, &sysmonitor_proc_fops);
    if (!proc_sysmonitor) {
        printk(KERN_ALERT "Failed to create proc entry\n");
        ret = -ENOMEM;
        goto fail_proc;
    }

    ///V1.01新增部份：分配大塊內存
    large_buffer = vmalloc(1024 * 1024);  // 分配1MB內存
    if (!large_buffer) {
        printk(KERN_ALERT "SysMonitor: Failed to allocate large buffer\n");
        ret = -ENOMEM;
        goto fail_vmalloc;
    }

    //1.02新增
    tasklet_init(&irq_tasklet, irq_tasklet_func, 0);

    //V1.01新增部份：註冊中斷處理程序（示例使用IRQ 1，實際使用時應該使用正確的IRQ號）
    ret = request_irq(1, sysmonitor_interrupt, IRQF_SHARED, "sysmonitor", THIS_MODULE);
    if (ret) {
        printk(KERN_ALERT "SysMonitor: Failed to request IRQ\n");
        goto fail_irq;
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

    /*V1.01新增部份*/
    fail_irq:
        free_irq(1, THIS_MODULE);
    fail_vmalloc:
        vfree(large_buffer);
    fail_proc:
        proc_remove(proc_sysmonitor);
    fail_device:
        device_destroy(sysmonitor_class, MKDEV(major_number, 0));
    fail_class:
        class_destroy(sysmonitor_class);
    fail_chrdev:
        unregister_chrdev(major_number, DEVICE_NAME);
        return ret;
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
MODULE_VERSION("1.01");