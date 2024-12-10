#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/export.h>
#include <linux/mod_devicetable.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#define DEV_NAME "digital"
#define DEV_CNT (1)

struct self_spi_dev {
	struct spi_device *spi_dev; // SPI设备
	dev_t dev_id;				// 设备号
	struct cdev cdev;			// 字符设备
	struct class *class;		// 类
	struct device *device;		// 设备
	struct device_node *node;	// 设备树节点
};

static const struct spi_device_id m74hc595_id[] = {
	{ "m74hc595", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(spi, m74hc595_id);

static const struct of_device_id m74hc595_of_match[] = {
	{ .compatible = "m74hc595" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, m74hc595_of_match);

static int m74hc595_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = inode->i_cdev;
	struct self_spi_dev *spi = container_of(cdev, struct self_spi_dev, cdev);

	file->private_data = spi;
	return 0;
}

static ssize_t m74hc595_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
	struct self_spi_dev *spi = file->private_data;
	struct spi_device *spi_dev = spi->spi_dev;

#define SEND_BYTES (2)

	uint8_t tx_data[SEND_BYTES] = { 0 };
	int ret = 0;

	if (len != SEND_BYTES)
		return -EFAULT;

	if (copy_from_user(tx_data, buf, len)) {
		pr_err("m74hc595_write: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = spi_write(spi_dev, tx_data, len);
	if (ret < 0) {
		pr_err("m74hc595_write: spi_write failed with err=%d\n", ret);
		return ret;
	}

	return len;
}

static const struct file_operations spi_ops = {
	.owner = THIS_MODULE,
	.open = m74hc595_open,
	.write = m74hc595_write,
};

static int m74hc595_probe(struct spi_device *spi)
{
	int ret = 0;
	struct self_spi_dev *p_spi_dev;

	p_spi_dev = devm_kzalloc(&spi->dev, sizeof(*p_spi_dev), GFP_KERNEL);
	if (!p_spi_dev) {
		pr_err("m74hc595_probe: devm_kzalloc failed\n");
		return -ENOMEM;
	}

	p_spi_dev->node = spi->dev.of_node;

	/* 1. 创建设备号 */
	ret = alloc_chrdev_region(&p_spi_dev->dev_id, 0, DEV_CNT, DEV_NAME);
	if (ret < 0) {
		pr_err("m74hc595_probe: Can't alloc_chrdev_region, ret=%d\n", ret);
		return ret;
	}

	/* 2. 初始化 cdev */
	p_spi_dev->cdev.owner = THIS_MODULE;
	cdev_init(&p_spi_dev->cdev, &spi_ops);

	/* 3. 添加 cdev */
	ret = cdev_add(&p_spi_dev->cdev, p_spi_dev->dev_id, DEV_CNT);
	if (ret < 0) {
		pr_err("m74hc595_probe: Can't add cdev, ret=%d\n", ret);
		goto free_chr_dev_region;
	}

	/* 4. 创建类 */
	p_spi_dev->class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(p_spi_dev->class)) {
		pr_err("m74hc595_probe: Can't create class, ret=%ld\n", PTR_ERR(p_spi_dev->class));
		ret = PTR_ERR(p_spi_dev->class);
		goto remove_cdev;
	}

	/* 5. 创建设备 */
	p_spi_dev->device = device_create(p_spi_dev->class, NULL, p_spi_dev->dev_id, NULL, DEV_NAME);
	if (IS_ERR(p_spi_dev->device)) {
		pr_err("m74hc595_probe: Can't create device, ret=%ld\n", PTR_ERR(p_spi_dev->device));
		ret = PTR_ERR(p_spi_dev->device);
		goto destroy_class;
	}

	/* 设置 SPI 参数 */
	spi->mode = SPI_MODE_0;		// MODE0,CPOL=0,CPHA=0
	spi->bits_per_word = 8;		// 每字8位
	spi->max_speed_hz = 250000; // 250KHz 最低250K

	ret = spi_setup(spi); // 初始化 SPI
	if (ret) {
		pr_err("m74hc595_probe: Failed to setup SPI, ret=%d\n", ret);
		goto destroy_device;
	}

	/* 保存 SPI 设备 */
	p_spi_dev->spi_dev = spi;
	spi_set_drvdata(spi, p_spi_dev);

	return 0;

destroy_device:
	device_destroy(p_spi_dev->class, p_spi_dev->dev_id);

destroy_class:
	class_destroy(p_spi_dev->class);

remove_cdev:
	cdev_del(&p_spi_dev->cdev);

free_chr_dev_region:
	unregister_chrdev_region(p_spi_dev->dev_id, DEV_CNT);

	return ret;
}

static void m74hc595_remove(struct spi_device *spi)
{
	struct self_spi_dev *p_spi_dev = spi_get_drvdata(spi);

	/* 删除 cdev */
	cdev_del(&p_spi_dev->cdev);

	/* 注销设备号 */
	unregister_chrdev_region(p_spi_dev->dev_id, DEV_CNT);

	/* 注销设备 */
	device_destroy(p_spi_dev->class, p_spi_dev->dev_id);

	/* 注销类 */
	class_destroy(p_spi_dev->class);
}

static struct spi_driver m74hc595_drv = {
    .probe = m74hc595_probe,
    .remove = m74hc595_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name = "m74hc595",
        .of_match_table = m74hc595_of_match,
    },
    .id_table = m74hc595_id,
};

static int __init m74hc595_init(void)
{
	return spi_register_driver(&m74hc595_drv);
}

static void __exit m74hc595_exit(void)
{
	spi_unregister_driver(&m74hc595_drv);
}

module_init(m74hc595_init);
module_exit(m74hc595_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wenshuyu");

// scp spi_drv.ko wenshuyu@192.168.1.6:/home/wenshuyu/ecaps_driver