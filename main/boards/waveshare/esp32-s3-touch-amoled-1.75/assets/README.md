# 1.75C 资源包

`xiaosi/` 是 1.75C-32m 资源分区（0xA00000）里那个“小司同学”资源包解开后的文件，原样存放。
没改动时打出来的包和板子上的一模一样（sha256 `0c085371…`）。

## 唤醒词和阈值

都在 `xiaosi/index.json` 的 `multinet_model` 里：

- `commands[].command`：实际识别的词，拼音，字之间用空格，比如 `xiao si tong xue`
- `commands[].text`：显示名，开机提示“说「X」就能唤醒我”和唤醒后发给服务器的都是它
- `threshold`：0 到 1，越大越不容易误唤醒，也越难叫醒，现在是 0.4
- `duration`：一次识别的时长，毫秒

其他文件：`font_puhui_deepseek_30_4.bin` 是字体（`text_font`），`srmodels.bin` 是语音模型（mn6_cn 中文命令词），`*.gif` 是表情，`skin` 是亮暗主题颜色。

## 打包、刷写、推送

改完 index.json 不用管长度和校验，脚本会重算。

```sh
# 打包
python scripts/pack_assets.py pack main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/xiaosi build/xiaosi-assets.bin

# 直接刷到板子
python -m esptool --chip esp32s3 -p COM10 write_flash 0xA00000 build/xiaosi-assets.bin

# 或者推给 GitHub，板子下次开机自己下载（不带参数就用这个目录）
python scripts/publish_assets.py
```

换成别的资源包：先清空 `xiaosi/`，再 `python scripts/pack_assets.py unpack 新包.bin main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/xiaosi`。
