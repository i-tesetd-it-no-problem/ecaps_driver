#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <asm/uaccess.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#define LED_NAME "led_gpio_pinctrl"
#define LED_COUNT 3

struct led_gpio {
    int gpio;
    struct device_node *node;
    const char *label;
    struct cdev cdev;
    struct device *device;
};

struct led_gpio_pinctrl_dev {
    dev_t dev_id;
    struct class *class;
    struct led_gpio leds[LED_COUNT];
};

static struct led_gpio_pinctrl_dev led_gpio_pinctrl_dev;

/* 文件操作函数 */

/* 打开设备文件时调用 */
static int led_open(struct inode *inode, struct file *file)
{
    struct led_gpio *led;
    led = container_of(inode->i_cdev, struct led_gpio, cdev);
    file->private_data = led;
    return 0;
}

/* 释放设备文件时调用 */
static int led_release(struct inode *inode, struct file *file)
{
    // 无需特殊操作
    return 0;
}

/* 写数据到设备文件时调用 */
static ssize_t led_write(struct file *filep, const char __user *buf, size_t count, loff_t *offset)
{
    int ret;
    uint8_t write_value;
    struct led_gpio *led = filep->private_data;

    if (count < 1)
        return -EINVAL;

    ret = copy_from_user(&write_value, buf, 1);
    if (ret) {
        pr_err("copy_from_user failed\n");
        return -EFAULT;
    }

    gpio_set_value(led->gpio, write_value ? 1 : 0);

    return 1;
}

/* 文件操作结构体 */
static struct file_operations led_fops = {
   .owner = THIS_MODULE,
   .open = led_open,
   .release = led_release,
   .write = led_write,
};

/* 驱动初始化函数 */
static int __init led_init(void)
{
    int ret;
    struct device_node *leds_node;
    struct device_node *child;
    int i = 0;

    /* 查找设备树中的"user-leds"节点 */
    leds_node = of_find_node_by_path("/user-leds");
    if (!leds_node) {
        pr_err("can't find /user-leds node\n");
        return -EINVAL;
    }

    /* 分配设备号 */
    ret = alloc_chrdev_region(&led_gpio_pinctrl_dev.dev_id, 0, LED_COUNT, LED_NAME);
    if (ret < 0) {
        pr_err("alloc_chrdev_region failed\n");
        return ret;
    }

    /* 创建设备类 */
    led_gpio_pinctrl_dev.class = class_create(THIS_MODULE, LED_NAME);
    if (IS_ERR(led_gpio_pinctrl_dev.class)) {
        pr_err("class_create failed\n");
        unregister_chrdev_region(led_gpio_pinctrl_dev.dev_id, LED_COUNT);
        return PTR_ERR(led_gpio_pinctrl_dev.class);
    }

    /* 遍历"user-leds"节点下的每个LED子节点 */
    for_each_child_of_node(leds_node, child) {
        if (i >= LED_COUNT) {
            pr_warn("Maximum LED count (%d) reached, ignoring extra LEDs\n", LED_COUNT);
            break;
        }

        struct led_gpio *led = &led_gpio_pinctrl_dev.leds[i];
        led->node = child;

        /* 读取label属性 */
        ret = of_property_read_string(child, "label", &led->label);
        if (ret < 0) {
            pr_err("can't read label for LED %d\n", i + 1);
            goto err_cleanup;
        }

        /* 获取GPIO属性 */
        led->gpio = of_get_named_gpio(child, "gpios", 0);
        if (led->gpio < 0) {
            pr_err("can't get gpio for %s\n", led->label);
            goto err_cleanup;
        }

        /* 申请GPIO */
        ret = gpio_request(led->gpio, led->label);
        if (ret) {
            pr_err("request gpio %d failed for %s\n", led->gpio, led->label);
            goto err_cleanup;
        }

        /* 设置GPIO方向为输出，并设置默认状态 */
        const char *default_state;
        ret = of_property_read_string(child, "default-state", &default_state);
        if (ret == 0 && strcmp(default_state, "on") == 0) {
            ret = gpio_direction_output(led->gpio, 1);
        } else {
            ret = gpio_direction_output(led->gpio, 0);
        }

        if (ret < 0) {
            pr_err("can't set gpio %d direction for %s\n", led->gpio, led->label);
            gpio_free(led->gpio);
            goto err_cleanup;
        }

        /* 初始化cdev */
        cdev_init(&led->cdev, &led_fops);
        led->cdev.owner = THIS_MODULE;

        /* 添加cdev到系统 */
        ret = cdev_add(&led->cdev, MKDEV(MAJOR(led_gpio_pinctrl_dev.dev_id), MINOR(led_gpio_pinctrl_dev.dev_id) + i), 1);
        if (ret < 0) {
            pr_err("cdev_add failed for %s\n", led->label);
            gpio_free(led->gpio);
            goto err_cleanup;
        }

        /* 创建设备节点 */
        led->device = device_create(led_gpio_pinctrl_dev.class, NULL, MKDEV(MAJOR(led_gpio_pinctrl_dev.dev_id), MINOR(led_gpio_pinctrl_dev.dev_id) + i), NULL, led->label);
        if (IS_ERR(led->device)) {
            pr_err("device_create failed for %s\n", led->label);
            cdev_del(&led->cdev);
            gpio_free(led->gpio);
            goto err_cleanup;
        }

        i++;
    }

    if (i == 0) {
        pr_err("No LEDs found in /user-leds\n");
        ret = -EINVAL;
        goto err_cleanup;
    }

    return 0;

err_cleanup:
    while (i > 0) {
        i--;
        struct led_gpio *led = &led_gpio_pinctrl_dev.leds[i];
        device_destroy(led_gpio_pinctrl_dev.class, MKDEV(MAJOR(led_gpio_pinctrl_dev.dev_id), MINOR(led_gpio_pinctrl_dev.dev_id) + i));
        cdev_del(&led->cdev);
        gpio_free(led->gpio);
    }
    class_destroy(led_gpio_pinctrl_dev.class);
    unregister_chrdev_region(led_gpio_pinctrl_dev.dev_id, LED_COUNT);
    return ret;
}

/* 驱动卸载函数 */
static void __exit led_exit(void)
{
    int i;

    for (i = 0; i < LED_COUNT; i++) {
        struct led_gpio *led = &led_gpio_pinctrl_dev.leds[i];
        device_destroy(led_gpio_pinctrl_dev.class, MKDEV(MAJOR(led_gpio_pinctrl_dev.dev_id), MINOR(led_gpio_pinctrl_dev.dev_id) + i));
        cdev_del(&led->cdev);
        gpio_set_value(led->gpio, 0); // 可选：卸载时关闭LED
        gpio_free(led->gpio);
    }

    class_destroy(led_gpio_pinctrl_dev.class);
    unregister_chrdev_region(led_gpio_pinctrl_dev.dev_id, LED_COUNT);
}

module_init(led_init);
module_exit(led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wenshuyu");
MODULE_DESCRIPTION("LED driver for device tree");
