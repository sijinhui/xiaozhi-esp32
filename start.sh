
```bash
#设置目标芯片为esp32s3
idf.py set-target esp32s3

# 修改配置
idf.py menuconfig

# 进行编译
idf.py build


# 刷写
idf.py build flash monitor
```

