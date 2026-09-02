# t_wakeup — GPIO3 USB 唤醒诊断

该工程只验证这一条链路：

`USB +5V → R11/R14 → D7 → WAKEUP_MCU(GPIO3) → EXT1 → MCU → LCD`

不会修改或依赖 NomadCast 主程序。

## 屏幕颜色

- 蓝色：普通启动，使用与主程序相同的 GPIO3 内部下拉模式。
- 黄色：启动时按住电源键，使用 GPIO3 无下拉对照模式。
- 绿色：GPIO3 触发 EXT1，MCU 已由插入 USB 唤醒。
- 紫色：GPIO5 电源键人工救援唤醒。
- 红色：检测到悬空/假唤醒，或者无法配置/进入深睡。

## 构建和烧录

使用 ESP-IDF v5.5.3 环境：

```powershell
cd D:\Codes\NomadCast\debug\t_wakeup
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

测试时必须连接电池。拔掉 USB 后，MCU 仍需要由电池维持 Deep-sleep。

## 实验一：复现主程序

1. 不按电源键，复位或重新烧录，屏幕应为蓝色。
2. 串口每秒打印一组 GPIO3 数据：

   ```text
   GPIO3  NO_PULL: OK n=32 ...mV D=... | PULLDOWN: OK n=32 ...mV D=...
   ```

   `NO_PULL` 用于辅助判断 USB 物理插拔；`PULLDOWN` 用于观察内部下拉造成的负载影响。进入 Deep-sleep 前要求两组 ADC 都有效且都为低；唤醒后的电压确认只依赖 `NO_PULL`，因此 `PULLDOWN: BAD` 不影响该项确认。

3. 观察至少 5 组数据，然后拔掉 USB。
4. 检测到 USB 连续 3 秒不存在后，屏幕关闭并进入 Deep-sleep。
5. 重新插入 USB：
   - 屏幕变绿色：GPIO3 成功触发 EXT1 并唤醒 MCU；下拉模式下串口重新连接后会打印 `PASS`。
   - 屏幕保持关闭：GPIO3 没有达到 EXT1 高电平门限。
6. 如果没有唤醒，可按电源键 GPIO5 人工唤醒，屏幕将变为紫色。

## 实验二：关闭内部下拉作对照

1. USB 已连接时，按住电源键并复位；看到黄色屏幕后松开电源键。
2. 重复实验一的拔线、等待关屏、重新插线步骤。
   所选模式会保存在 RTC 内存中，Deep-sleep 唤醒后日志中的 `mode` 仍表示实际完成的那组实验。GPIO5 人工救援不会自动改变模式；如需切换模式，请按住或松开 GPIO5 后执行普通复位。
3. 结果判断：
   - 下拉模式失败、无下拉模式成功：内部下拉正在拖低 USB 唤醒电平。
   - 两种模式都失败，并且 `NO_PULL` 电压接近 0V：检查 R11、R14、D7、GPIO3 走线或焊接。
   - 无下拉模式拔线后立即变绿/红：GPIO3 在无 USB 时悬空，不能可靠地直接作为 EXT1 唤醒源。
   - `NO_PULL` 有约 2.2～2.4V、但数字电平仍为 0：电路输出低于 ESP32-S3 保证高电平门限。

   无下拉模式中，GPIO3 本身也可能悬空，所以程序只能报告 `RESULT: GPIO3 caused EXT1...`，不会自动打印 `PASS`。必须结合“是否确实在关屏后插入了 USB”以及唤醒时序判断。如果拔线后 `NO_PULL` 仍长期高于 300mV，程序会继续等待而不进入 Deep-sleep；这说明该模式下 GPIO3 悬空，无法安全布防，并非程序卡死。

## 建议同时用万用表确认

插入 USB 时依次测量：

1. R11 上端：约 5V。
2. R11/R14 中点、D7 输入：约 2.5V。
3. D7 输出、GPIO3：与串口 ADC 结果应大致一致。

如果第 1 或第 2 点已经没有预期电压，问题位于 USB 电源或分压电路；如果只有第 3 点异常，重点检查 D7、GPIO3 走线以及内部下拉的负载影响。
