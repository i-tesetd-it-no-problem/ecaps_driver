/**
 * @file led_main_drv.c
 * @author wenshuyu (wsy2161826815@163.com)
 * @brief 主板的三个LED驱动程序
 * @version 1.0
 * @date 2024-11-18
 * 
 * @copyright Copyright (c) 2024
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#define LED_NAME "led_main"

/* 单个LED信息 */
struct led_gpio {
    struct gpio_desc *desc;        /* GPIO描述符 */
    const char *label;             /* 设备树中的label属性 */
    struct cdev cdev;              /* 字符设备 */
    struct device *device;         /* 设备节点 */
    dev_t dev_id;                  /* 设备号 */
};

/* LED平台设备 */
struct led_platform_data {
    struct led_gpio *leds;         /* 动态分配的LED数组 */
    int led_count;                 /* LED数量 */
    struct class *class;           /* 设备类 */
};

static struct led_platform_data *led_pdata;

/* 打开设备 */
static int led_open(struct inode *inode, struct file *file)
{
    struct led_gpio *led = container_of(inode->i_cdev, struct led_gpio, cdev);
    file->private_data = led;
    return 0;
}

/* 释放设备 */
static int led_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* 写入LED */
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

    gpiod_set_value(led->desc, write_value ? 1 : 0);

    return 1;
}

/* 文件操作结构体 */
static struct file_operations led_fops = {
   .owner = THIS_MODULE,
   .open = led_open,
   .release = led_release,
   .write = led_write,
};

/* 移除平台设备 */
static int led_remove(struct platform_device *pdev)
{
    int i;
    struct led_platform_data *pdata = platform_get_drvdata(pdev);

    for (i = 0; i < pdata->led_count; i++) {
        struct led_gpio *led = &pdata->leds[i];
        device_destroy(pdata->class, led->dev_id);
        cdev_del(&led->cdev);
        gpiod_set_value(led->desc, 0); /* 卸载时关闭LED */
        gpiod_put(led->desc);
    }

    class_destroy(pdata->class);
    unregister_chrdev_region(pdata->leds[0].dev_id, pdata->led_count);
    kfree(pdata->leds);
    kfree(pdata);

    pr_info("LED platform driver removed\n");

    return 0;
}

/* 平台设备的匹配表 */
static const struct of_device_id led_of_match[] = {
    { .compatible = "gpio-main-leds" },
    { /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, led_of_match);

/* 平台设备的探测函数 */
static int led_probe(struct platform_device *pdev)
{
    int ret, i = 0;
    struct device_node *leds_node = pdev->dev.of_node;
    struct device_node *child;

    /* 分配平台数据结构体 */
    led_pdata = devm_kzalloc(&pdev->dev, sizeof(*led_pdata), GFP_KERNEL);
    if (!led_pdata)
        return -ENOMEM;

    /* 获取LED数量 */
    led_pdata->led_count = of_get_child_count(leds_node);
    if (led_pdata->led_count == 0) {
        pr_err("No LEDs found\n");
        return -EINVAL;
    }

    /* 分配LED数组 */
    led_pdata->leds = devm_kzalloc(&pdev->dev, sizeof(struct led_gpio) * led_pdata->led_count, GFP_KERNEL);
    if (!led_pdata->leds)
        return -ENOMEM;

    /* 分配设备号 */
    ret = alloc_chrdev_region(&led_pdata->leds[0].dev_id, 0, led_pdata->led_count, LED_NAME);
    if (ret < 0) {
        pr_err("alloc_chrdev_region failed\n");
        return ret;
    }

    /* 创建设备类 */
    led_pdata->class = class_create(THIS_MODULE, LED_NAME);
    if (IS_ERR(led_pdata->class)) {
        pr_err("class_create failed\n");
        ret = PTR_ERR(led_pdata->class);
        goto err_unregister_chrdev;
    }

    /* 遍历每个LED子节点 */
    for_each_child_of_node(leds_node, child) {
        struct led_gpio *led = &led_pdata->leds[i];
        const char *default_state;
        enum gpiod_flags flags = GPIOD_OUT_LOW;

        /* 读取label属性 */
        ret = of_property_read_string(child, "label", &led->label);
        if (ret < 0) {
            pr_err("can't read label for LED %d\n", i + 1);
            goto err_cleanup;
        }

        /* 设置默认状态 */
        ret = of_property_read_string(child, "default-state", &default_state);
        if (ret == 0 && strcmp(default_state, "on") == 0) {
            flags = GPIOD_OUT_HIGH;
        }

        /* 获取GPIO描述符 */
        led->desc = devm_gpiod_get_from_of_node(&pdev->dev, child, "gpios", 0, flags, led->label);
        if (IS_ERR(led->desc)) {
            pr_err("gpiod_get_from_of_node failed for %s\n", led->label);
            ret = PTR_ERR(led->desc);
            goto err_cleanup;
        }

        /* 初始化cdev */
        cdev_init(&led->cdev, &led_fops);
        led->cdev.owner = THIS_MODULE;

        /* 设置设备号 */
        led->dev_id = MKDEV(MAJOR(led_pdata->leds[0].dev_id), MINOR(led_pdata->leds[0].dev_id) + i);

        /* 添加cdev到系统 */
        ret = cdev_add(&led->cdev, led->dev_id, 1);
        if (ret < 0) {
            pr_err("cdev_add failed for %s\n", led->label);
            goto err_cleanup;
        }

        /* 创建设备节点 */
        led->device = device_create(led_pdata->class, NULL, led->dev_id, NULL, led->label);
        if (IS_ERR(led->device)) {
            pr_err("device_create failed for %s\n", led->label);
            cdev_del(&led->cdev);
            ret = PTR_ERR(led->device);
            goto err_cleanup;
        }

        i++;
    }

    platform_set_drvdata(pdev, led_pdata);

    pr_info("LED platform driver probed\n");

    return 0;

err_cleanup:
    while (i > 0) {
        i--;
        {
            struct led_gpio *led = &led_pdata->leds[i];
            device_destroy(led_pdata->class, led->dev_id);
            cdev_del(&led->cdev);
            gpiod_set_value(led->desc, 0);
            /* devm_* 分配的资源会在设备释放时自动释放 */
        }
    }
    class_destroy(led_pdata->class);
err_unregister_chrdev:
    unregister_chrdev_region(led_pdata->leds[0].dev_id, led_pdata->led_count);
    return ret;
}

/* 平台驱动结构体 */
static struct platform_driver led_main_platform_driver = {
    .probe = led_probe,
    .remove = led_remove,
    .driver = {
        .name = "led_main_platform_driver",
        .of_match_table = led_of_match,
    },
};

module_platform_driver(led_main_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wenshuyu");
MODULE_DESCRIPTION("LED driver for device tree using platform framework");

// scp led_main_drv.ko wenshuyu@192.168.1.6:/home/wenshuyu/ecaps_driver