# CST836U 原模块测试（PCB V1.2）

本工程直接编译 `refer/read-pico-reader-main/components/cst836u/cst836u.c`，
并使用同目录 `include/cst836u.h`，原模块源码和头文件未修改。
本地 `components/cst836u/CMakeLists.txt` 仅负责接入原文件，避开参考项目专用板型构建依赖。
已移除 main 下自行实现的同名驱动，软件 I2C 诊断也不参与编译。

板载定义来自 `boards/esp32s3/nomadcast.h`：SDA=41、SCL=42、RST=43、INT=44；
AP 电源=46、LCD/TP 电源=21、背光=39（关闭）。上电后保持 RST LOW 100ms，
释放后等待 50ms，再创建 I2C 总线并调用原模块 `cst836u_init()`。

原模块默认地址 0x15、400kHz；初始化必须先成功读取 0xA6 芯片信息。
若初始化失败，按原模块契约停止，不伪造句柄或绕过信息读取。
成功后每 20ms 调用原模块读点 API，打印两点坐标、事件及计数。
读失败不会伪造抬手；不发送深睡命令，不执行其他协议对照。

```powershell
. C:/Espressif/frameworks/esp-idf-v5.5.3/export.ps1
idf.py -C debug/t_touch_CST836U -B D:/Codes/NomadCast/.cache/cst836u_build -p COM12 build flash
python debug/t_touch_CST836U/capture.py --port COM12 --seconds 15 --reset --output capture.log
```

源码对比见 [REFERENCE_COMPARISON.md](REFERENCE_COMPARISON.md)，历史及本轮结果见
[test-results/CST836U_RESULTS.md](test-results/CST836U_RESULTS.md)。
本轮日志命名 `cst836u_08_direct_refer.log`；更早的日志使用不同测试实现。
