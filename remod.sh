#!/bin/sh

# 卸载模块
./rmmod.sh
if [ $? -ne 0 ]; then
  echo "Failed to unload module"
  exit 1
fi

# 删除现有 .ko 文件
rm ./*.ko
if [ $? -ne 0 ]; then
  echo "Failed to remove old .ko files"
  exit 1
fi

# 复制新的 .ko 文件
cp /home/wenshuyu/ecaps_driver/*.ko ./
if [ $? -ne 0 ]; then
  echo "Failed to copy new .ko files"
  exit 1
fi

depmod

# 加载新模块
./modprobe.sh
if [ $? -ne 0 ]; then
  echo "Failed to load new module"
  exit 1
fi

echo "Kernel modules updated successfully"
