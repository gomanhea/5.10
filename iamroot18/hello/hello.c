#include <linux/init.h>
#include <linux/module.h>

static int hello_init(void) {
    printk(KERN_INFO "Hello world.\n");
    return 0;
}

static void hello_exit(void) {
    printk(KERN_INFO "Goodbye world.\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkr");

