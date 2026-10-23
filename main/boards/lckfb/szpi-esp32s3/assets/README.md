# 立创开发板资源包

`xiaosi/` 是给立创开发板（lichuang-dev，320×240 屏）做的“小司同学”资源包，唤醒词、阈值、语音模型和颜色都和 1.75C 的一样（`main/boards/waveshare/esp32-s3-touch-amoled-1.75/assets/`，字段说明见那边的 README）。

和 1.75C 不同的地方：资源分区只有 8MB（`partitions/v2/16m.csv`，0x800000），1.75C 的包有 10.4MB 放不下，所以

- 字体用 20px 的 `font_puhui_common_20_4.bin`（1.75C 是同一字体的 30px）
- 表情用 64px 的 noto 动图（1.75C 是同一套 128px 的），来自 xiaozhi-fonts 1.6.0 的 `gif/noto-emoji_64`

打出来约 6.5MB。改唤醒词或阈值时两边的 index.json 要一起改。

```sh
python scripts/pack_assets.py pack main/boards/lckfb/szpi-esp32s3/assets/xiaosi build/lichuang-assets.bin
python -m esptool --chip esp32s3 -p COMx write_flash 0x800000 build/lichuang-assets.bin
python scripts/publish_assets.py --board lichuang-dev
```
