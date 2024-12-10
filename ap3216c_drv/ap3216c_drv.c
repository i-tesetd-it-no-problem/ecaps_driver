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
#include <linux/delay.h> 

/* AP3316C 寄存器 */
#define AP3216C_SYSTEMCONG 0x00	 /* 配置寄存器 */
#define AP3216C_INTSTATUS 0X01	 /* 中断状态寄存器 */
#define AP3216C_INTCLEAR 0X02	 /* 中断清除寄存器 */
#define AP3216C_IRDATALOW 0x0A	 /* IR 数据低字节 */
#define AP3216C_IRDATAHIGH 0x0B	 /* IR 数据高字节 */
#define AP3216C_ALSDATALOW 0x0C	 /* ALS 数据低字节 */
#define AP3216C_ALSDATAHIGH 0X0D /* ALS 数据高字节 */
#define AP3216C_PSDATALOW 0X0E	 /* PS 数据低字节 */
#define AP3216C_PSDATAHIGH 0X0F	 /* PS 数据高字节 */

#define DEV_NAME "ap3216c"
#define DEV_CNT (1)

struct ap3216c_dev {
	struct i2c_client *client; // I2C 设备
	dev_t dev_id;			   // 设备号
	struct cdev cdev;		   // 字符设备
	struct class *class;	   // 类
	struct device *device;	   // 设备
	struct device_node *node;  // 设备树节点

	uint8_t read_data[6]; // ir als ps 三个数据
};

/**
 * @brief 写入数据到指定的寄存器
 * 
 * @param ap3216c 全局设备 
 * @param cmd 指令
 * @param data 数据
 * @return int 成功返回0 失败其他
 */
static int ap3216c_write_regs(struct ap3216c_dev *ap3216c, uint8_t reg, uint8_t *data, size_t len)
{
	uint8_t send_data[256] = { 0 };
	send_data[0] = reg;
	memcpy(send_data + 1, data, len);
	struct i2c_msg msg = {
		.addr = ap3216c->client->addr,
		.flags = 0, // 写操作
		.len = 1 + len,
		.buf = send_data,
	};

	int ret = i2c_transfer(ap3216c->client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&ap3216c->client->dev, "Failed to write data, ret=%d\n", ret);
		return (ret < 0) ? ret : -EIO;
	}

	return 0;
}

/**
 * @brief 读取寄存器数据
 * 
 * @param ap3216c 全局设备 
 * @param cmd 指令
 * @param data 数据
 * @return int 成功返回0 失败其他
 */
static int ap3216c_read_regs(struct ap3216c_dev *ap3216c, uint8_t reg, uint8_t *data, size_t len)
{
	int ret = 0;
	struct i2c_client *client = ap3216c->client;

	struct i2c_msg msg[2] = {
		[0] = {
			.addr = client->addr,
			.buf = &reg,
			.flags = 0,
			.len = 1,
		},
		[1] = {
			.addr = client->addr,
			.buf = data,
			.flags = I2C_M_RD,
			.len = len,
		},
	};

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret == 2) {
		ret = 0;
	} else {
		printk("i2c rd failed=%d reg=%06x len=%d\n", ret, reg, len);
		ret = -EREMOTEIO;
	}
	return ret;
}

static int ap3216c_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = inode->i_cdev;
	struct ap3216c_dev *ap3216c = container_of(cdev, struct ap3216c_dev, cdev);

	uint8_t init_data = 0x04;

	ap3216c_write_regs(ap3216c, AP3216C_SYSTEMCONG, &init_data, 1);
	mdelay(50);
	init_data = 0x03;
	ap3216c_write_regs(ap3216c, AP3216C_SYSTEMCONG, &init_data, 1);

	file->private_data = ap3216c;

	return 0;
}

static ssize_t ap3216c_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
	struct ap3216c_dev *ap3216c = file->private_data;

	// 两个数据
	if (len != sizeof(ap3216c->read_data)) {
		pr_err("Invalid read length: %zu\n", len);
		return -EINVAL;
	}

	uint8_t *read_buf = ap3216c->read_data;

	for (int i = 0; i < 6; i++)
		ap3216c_read_regs(ap3216c, AP3216C_IRDATALOW + i, read_buf + i, 1);

	if (copy_to_user(buf, &ap3216c->read_data, sizeof(ap3216c->read_data)))
		return -EFAULT;

	return len;
}

static const struct file_operations ap3216c_ops = {
	.owner = THIS_MODULE,
	.open = ap3216c_open,
	.read = ap3216c_read,
};

static int ap3216c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	struct ap3216c_dev *ap3216c = NULL;

	ap3216c = devm_kzalloc(&client->dev, sizeof(*ap3216c), GFP_KERNEL);
	if (!ap3216c)
		return -ENOMEM;

	ap3216c->client = client;

	// 1. 创建设备号
	ret = alloc_chrdev_region(&ap3216c->dev_id, 0, DEV_CNT, DEV_NAME);
	if (ret) {
		pr_err("alloc device id failed with error %d\n", ret);
		return ret;
	}

	// 2. 初始化cdev
	cdev_init(&ap3216c->cdev, &ap3216c_ops);
	ap3216c->cdev.owner = THIS_MODULE;

	// 3. 添加cdev
	ret = cdev_add(&ap3216c->cdev, ap3216c->dev_id, DEV_CNT);
	if (ret) {
		pr_err("cdev_add failed with error %d\n", ret);
		goto err_free_devid;
	}

	// 4. 创建类
	ap3216c->class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(ap3216c->class)) {
		pr_err("class_create failed with error %ld\n", PTR_ERR(ap3216c->class));
		ret = PTR_ERR(ap3216c->class);
		goto err_del_cdev;
	}

	// 5. 创建设备
	ap3216c->device = device_create(ap3216c->class, NULL, ap3216c->dev_id, NULL, DEV_NAME);
	if (IS_ERR(ap3216c->device)) {
		pr_err("device_create failed with error %ld\n", PTR_ERR(ap3216c->device));
		ret = PTR_ERR(ap3216c->device);
		goto err_dest_class;
	}

	dev_set_drvdata(&client->dev, ap3216c);

	return 0;

err_dest_class:
	class_destroy(ap3216c->class);

err_del_cdev:
	cdev_del(&ap3216c->cdev);

err_free_devid:
	unregister_chrdev_region(ap3216c->dev_id, DEV_CNT);

	return ret;
}

static void ap3216c_remove(struct i2c_client *client)
{
	struct ap3216c_dev *ap3216c = dev_get_drvdata(&client->dev);

	device_destroy(ap3216c->class, ap3216c->dev_id);
	class_destroy(ap3216c->class);
	cdev_del(&ap3216c->cdev);
	unregister_chrdev_region(ap3216c->dev_id, DEV_CNT);
}

static const struct i2c_device_id ap3216c_id_table[] = {
	{ .name = "ap3216c", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id_table);

static const struct of_device_id ap3216c_match_table[] = {
	{ .compatible = "ap3216c" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ap3216c_match_table);

static struct i2c_driver ap3216c_driver = {
    .probe = ap3216c_probe,
    .remove = ap3216c_remove,
    .id_table = ap3216c_id_table,
    .driver = {
        .of_match_table = ap3216c_match_table,
        .owner = THIS_MODULE,
        .name = "ap3216c",
    },
};

static int __init ap3216c_init(void)
{
	return i2c_add_driver(&ap3216c_driver);
}

static void __exit ap3216c_exit(void)
{
	i2c_del_driver(&ap3216c_driver);
}

module_init(ap3216c_init);
module_exit(ap3216c_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for ap3216c Humidity and Temperature Sensor");

// scp ap3216c_drv.ko wenshuyu@192.168.1.6:/home/wenshuyu/ecaps_driver