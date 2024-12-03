import os

def collect():
    # 新建modules文件夹，保存所有的ko文件
    dest_dir_name = "modules"
    cur_path = os.getcwd()
    dest_path = os.path.join(cur_path, dest_dir_name)
    
    if not os.path.exists(dest_path):
        os.makedirs(dest_path)

    sub_dirs = []  # 除modules文件夹外的其他目录

    for sub_dir in os.listdir(cur_path):
        # 检查是否是目录
        sub_dir_path = os.path.join(cur_path, sub_dir)
        if sub_dir != dest_dir_name and os.path.isdir(sub_dir_path):
            sub_dirs.append(sub_dir)

    # 遍历除modules文件夹外的其他文件夹，把所有ko文件复制到modules文件夹
    for sub_dir in sub_dirs:
        sub_dir_path = os.path.join(cur_path, sub_dir)
        for file_name in os.listdir(sub_dir_path):
            if file_name.endswith(".ko"):
                src_file_path = os.path.join(sub_dir_path, file_name)
                dest_file_path = os.path.join(dest_path, file_name)
                os.system("cp '%s' '%s'" % (src_file_path, dest_file_path))

if __name__ == "__main__":
    collect()
