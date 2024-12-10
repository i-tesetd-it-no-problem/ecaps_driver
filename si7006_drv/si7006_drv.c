#include "linux/dev_printk.h"
#include "linux/err.h"
#include "linux/gfp_types.h"
#include "linux/i2c.h"
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/export.h>
#include <linux/mod_devicetable.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/delay.h> // 添加延迟需要的头文件

#define DEV_NAME "si7006"
#define DEV_CNT (1)

enum si7006_cmd {
	MEASURE_RELATIVE_HUMIDITY = 0, // 测量湿度
	MEASURE_TEMEPERATURE,		   // 测量温度
	WRITE_USER_REG_1,			   // 写用户寄存器1
	CMD_MAX,
};

static uint8_t si7006_cmd_value[CMD_MAX] = {
	[MEASURE_RELATIVE_HUMIDITY] = 0xE5, // 测量湿度
	[MEASURE_TEMEPERATURE] = 0xE3,		// 测量温度
	[WRITE_USER_REG_1] = 0xE6,			// 写用户寄存器1
};

struct SI7006_dev {
	struct i2c_client *client; // I2C 设备
	dev_t dev_id;			   // 设备号
	struct cdev cdev;		   // 字符设备
	struct class *class;	   // 类
	struct device *device;	   // 设备
	struct device_node *node;  // 设备树节点

	uint8_t read_data[4]; // 湿度 + 温度 两个 u16
};

/**
 * @brief 写入数据到指定的寄存器
 * 
 * @param si7006 全局设备 
 * @param cmd 指令
 * @param data 数据
 * @return int 成功返回0 失败其他
 */
static int si7006_write_data(struct SI7006_dev *si7006, uint8_t cmd, uint8_t data)
{
	uint8_t send_data[2] = { si7006_cmd_value[cmd], data };
	struct i2c_msg msg = {
		.addr = si7006->client->addr,
		.flags = 0, // 写操作
		.len = 2,
		.buf = send_data,
	};

	int ret = i2c_transfer(si7006->client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&si7006->client->dev, "Failed to write data, ret=%d\n", ret);
		return (ret < 0) ? ret : -EIO;
	}

	return 0;
}

/**
 * @brief 发送测量指令并读取数据
 * 
 * @param si7006 全局设备 
 * @param cmd 指令
 * @return int 成功返回0 失败其他
 */
static int si7006_read_after_measure(struct SI7006_dev *si7006, enum si7006_cmd cmd)
{
	if (!si7006)
		return -EINVAL;

	if (cmd >= CMD_MAX) {
		pr_err("invalid cmd: %u\n", cmd);
		return -EINVAL;
	}

	uint8_t command = si7006_cmd_value[cmd];
	uint8_t *p_buf = NULL;

	// 处理数据
	switch (cmd) {
	case MEASURE_RELATIVE_HUMIDITY: // 湿度
		p_buf = &si7006->read_data[0];
		break;

	case MEASURE_TEMEPERATURE: // 温度
		p_buf = &si7006->read_data[2];
		break;

	default:
		pr_err("Unexpected command: %u\n", cmd);
		return -EINVAL;
	}

	struct i2c_msg msgs[2] = {
		{
			.addr = si7006->client->addr,
			.flags = 0, // 写操作
			.len = 1,
			.buf = &command,
		},
		{
			.addr = si7006->client->addr,
			.flags = I2C_M_RD, // 读操作
			.len = 2,
			.buf = p_buf,
		},
	};

	int ret = i2c_transfer(si7006->client->adapter, msgs, 2);
	if (ret != 2) {
		dev_err(&si7006->client->dev, "I2C transfer failed with err %d\n", ret);
		return (ret < 0) ? ret : -EIO;
	}

	return 0;
}

static int SI7006_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = inode->i_cdev;
	struct SI7006_dev *SI7006 = container_of(cdev, struct SI7006_dev, cdev);

	uint8_t config_data = 0; // 默认配置

	// 写入用户寄存器1,禁用加热器并设置默认分辨率
	int ret = si7006_write_data(SI7006, WRITE_USER_REG_1, config_data);
	if (ret) {
		pr_err("init device failed with err:%d\n", ret);
		return ret;
	}

	file->private_data = SI7006;

	return 0;
}

static ssize_t SI7006_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
	struct SI7006_dev *SI7006 = file->private_data;

	// 两个数据
	if (len != sizeof(uint16_t) * 2) {
		pr_err("Invalid read length: %zu\n", len);
		return -EINVAL;
	}

	if (copy_to_user(buf, &SI7006->read_data, sizeof(uint16_t) * 2))
		return -EFAULT;

	return len;
}

static ssize_t SI7006_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
	// 指令占一个字节
	if (len != sizeof(uint8_t)) {
		pr_err("Invalid write length: %zu\n", len);
		return -EINVAL;
	}

	uint8_t user_cmd;
	if (copy_from_user(&user_cmd, buf, sizeof(uint8_t)))
		return -EFAULT;

	// 确保命令在有效范围内
	if (user_cmd >= CMD_MAX) {
		pr_err("Invalid user command from user space: %u\n", user_cmd);
		return -EINVAL;
	}

	int ret =
		si7006_read_after_measure(file->private_data, (enum si7006_cmd)user_cmd); // 发送测量指令
	return (ret == 0) ? len : ret;
}

static const struct file_operations si7006_ops = {
	.owner = THIS_MODULE,
	.open = SI7006_open,
	.write = SI7006_write,
	.read = SI7006_read,
};

static int si7006_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	struct SI7006_dev *si7006 = NULL;

	si7006 = devm_kzalloc(&client->dev, sizeof(*si7006), GFP_KERNEL);
	if (!si7006)
		return -ENOMEM;

	si7006->client = client;

	// 1. 创建设备号
	ret = alloc_chrdev_region(&si7006->dev_id, 0, DEV_CNT, DEV_NAME);
	if (ret) {
		pr_err("alloc device id failed with error %d\n", ret);
		return ret;
	}

	// 2. 初始化cdev
	cdev_init(&si7006->cdev, &si7006_ops);
	si7006->cdev.owner = THIS_MODULE;

	// 3. 添加cdev
	ret = cdev_add(&si7006->cdev, si7006->dev_id, DEV_CNT);
	if (ret) {
		pr_err("cdev_add failed with error %d\n", ret);
		goto err_free_devid;
	}

	// 4. 创建类
	si7006->class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(si7006->class)) {
		pr_err("class_create failed with error %ld\n", PTR_ERR(si7006->class));
		ret = PTR_ERR(si7006->class);
		goto err_del_cdev;
	}

	// 5. 创建设备
	si7006->device = device_create(si7006->class, NULL, si7006->dev_id, NULL, DEV_NAME);
	if (IS_ERR(si7006->device)) {
		pr_err("device_create failed with error %ld\n", PTR_ERR(si7006->device));
		ret = PTR_ERR(si7006->device);
		goto err_dest_class;
	}

	dev_set_drvdata(&client->dev, si7006);

	return 0;

err_dest_class:
	class_destroy(si7006->class);

err_del_cdev:
	cdev_del(&si7006->cdev);

err_free_devid:
	unregister_chrdev_region(si7006->dev_id, DEV_CNT);

	return ret;
}

static void si7006_remove(struct i2c_client *client)
{
	struct SI7006_dev *si7006 = dev_get_drvdata(&client->dev);

	device_destroy(si7006->class, si7006->dev_id);
	class_destroy(si7006->class);
	cdev_del(&si7006->cdev);
	unregister_chrdev_region(si7006->dev_id, DEV_CNT);
}

static const struct i2c_device_id si7006_id_table[] = {
	{ .name = "si7006", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, si7006_id_table);

static const struct of_device_id si7006_match_table[] = {
	{ .compatible = "si7006" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, si7006_match_table);

static struct i2c_driver si7006_driver = {
    .probe = si7006_probe,
    .remove = si7006_remove,
    .id_table = si7006_id_table,
    .driver = {
        .of_match_table = si7006_match_table,
        .owner = THIS_MODULE,
        .name = "si7006",
    },
};

static int __init si7006_init(void)
{
	return i2c_add_driver(&si7006_driver);
}

static void __exit si7006_exit(void)
{
	i2c_del_driver(&si7006_driver);
}

module_init(si7006_init);
module_exit(si7006_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for Si7006 Humidity and Temperature Sensor");

// scp si7006_drv.ko wenshuyu@192.168.1.6:/home/wenshuyu/ecaps_driver