## USB MSC 枚举延迟 & SDMMC 性能全面诊断报告

---

### 一、致命 Bug：USB 枚举 2 分钟的根本原因

#### 问题 1 ★★★ — `USB.begin()` 在 `msc.begin()` **之前**调用

在 usb_msc.cpp 中：

```cpp
// ❌ 当前错误顺序
USB.begin();                    // ← 设备立即向主机发起连接
msc.vendorID("M5Stack");        // ← 此时 MSC 类还未注册
...
msc.begin(capacity, sector_size); // ← 太晚了，主机已经枚举失败
```

这是造成 2 分钟延迟的**核心原因**。Arduino ESP32 的 TinyUSB 实现中，`USB.begin()` 会立即触发 D+ 上拉，通知主机开始枚举。此时 MSC 类还未注册到 TinyUSB 栈，主机收到的描述符中没有 MSC 接口，枚举失败。Windows 会按照标准的超时退避策略（500ms → 1s → 2s → …）不断重试，最终触发完整的驱动加载流程，整个过程正好对应 1~2 分钟。

**修复方法**（改变初始化顺序）：

```cpp
// ✅ 正确顺序：所有类配置在 USB.begin() 之前完成
msc.vendorID("M5Stack");
msc.productID("Paper S3");
msc.productRevision("1.0");
msc.onRead(onRead);
msc.onWrite(onWrite);
msc.onStartStop(onStartStop);
msc.mediaPresent(true);
msc.begin(g_card->csd.capacity, g_card->csd.sector_size);
USB.begin();  // ← 最后调用，此时描述符已完整
```

---

#### 问题 2 ★★★ — SDMMC 重复初始化：`SD_MMC.end()` 后紧接全量重建

usb_msc_start() 的流程是：
1. `SD_MMC.end()` → 卸载 FAT + 释放 `sdmmc_card_t`
2. 再调 `sdmmc_host_init()` → 返回 `ESP_ERR_INVALID_STATE`（已初始化）
3. 再调 `sdmmc_host_init_slot()` → 同样 `INVALID_STATE`
4. `sdmmc_card_init()` → 完整的卡识别流程（CMD0/CMD8/ACMD41/CMD2/CMD3/CMD7…）约耗 200–500 ms

更好的方式是完全跳过重新初始化，直接复用 SD_MMC 内部的 `sdmmc_card_t*`：

```cpp
// ✅ 利用 Arduino ESP32 3.x 暴露的 card() 方法
// SD_MMC.end() 只是卸载 VFS/FAT，不必完全 deinit SDMMC 驱动
// 在 SD_MMC.end() 之前先保存指针

// usb_msc.cpp 顶部加：
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"

bool usb_msc_start() {
    ...
    // 保存 card 指针（Arduino ESP32 3.x SDMMC 有 card() 方法）
    g_card = SD_MMC.card();   // 直接复用，无需重新探测
    if (!g_card) { ... }

    // 只卸载 VFS 文件系统，不 deinit SDMMC 驱动
    esp_vfs_fat_sdcard_unmount("/sdcard", g_card);
    g_unmounted_sdmmc_for_msc = true;

    // 然后直接进入 USB 配置，省掉全部 sdmmc_host_init 重建逻辑
    ...
}
```

---

### 二、显著性能问题

#### 问题 3 ★★★ — `CONFIG_TINYUSB_MSC_BUFSIZE=512`（仅 1 个扇区）

在 sdkconfig.PaperS3：

```ini
CONFIG_TINYUSB_MSC_BUFSIZE=512   # ← 极小，每次 USB SCSI READ 只能传 512 字节
```

Windows 枚举阶段会发出多个 READ(10)（分区表、FAT header、根目录等），512 字节的缓冲区意味着每个 USB bulk 传输包只能携带 1 个扇区，大量来回握手。正常参考设计应设为 **4096**（8 扇区）。

**修复**：在 sdkconfig.PaperS3 中修改：

```ini
CONFIG_TINYUSB_MSC_BUFSIZE=4096
```

---

#### 问题 4 ★★ — TinyUSB 任务栈仅 4096 字节，MSC 回调却调用 SDMMC I/O

```ini
CONFIG_TINYUSB_TASK_STACK_SIZE=4096   # ← 危险
```

`onRead`/`onWrite` 回调直接运行在 TinyUSB 任务上下文中，调用了 `sdmmc_read_sectors` / `sdmmc_write_sectors`。ESP-IDF SDMMC 驱动内部有 DMA 配置、信号量等待、错误恢复等多层栈帧，加上本身的 FreeRTOS 开销，4096 字节极易溢出导致偶发性 panic（很难复现）。

**修复**：

```ini
CONFIG_TINYUSB_TASK_STACK_SIZE=8192
```

---

#### 问题 5 ★★ — CDC + MSC 复合设备描述符拖慢 Windows 驱动加载

`build_flags` 中同时有：
```ini
-DARDUINO_USB_CDC_ON_BOOT=1
-DTINYUSB_CDC_ENABLED=1
-DTINYUSB_MSC_ENABLED=1
```

Windows 对 CDC ACM + MSC 复合设备（IAD 描述符）的驱动匹配有历史性慢路径，尤其是第一次连接时需要同时安装 CDC 串口驱动和 MSC 存储驱动。若设备在 MSC 模式下不需要 CDC 串口功能，可以在 MSC 启动时禁用 CDC：

```cpp
// 在 usb_msc_start() 中，进入 MSC 模式时排除 CDC
// 或在 build_flags 中为 MSC 专属固件添加 -DTINYUSB_CDC_ENABLED=0
```

如不方便，至少要保证 USB 序列号唯一。目前 sdkconfig.PaperS3 中：
```ini
CONFIG_TINYUSB_DESC_SERIAL_STRING="123456"  # ← 固定，会触发 Windows USB 缓存冲突
```

**修复**：让序列号基于 ESP32 MAC 地址动态生成（在代码中调用 `USB.serialNumber(macStr)`）。

---

### 三、SDMMC 读写速度优化

#### 问题 6 ★★ — `FATFS_USE_FASTSEEK` 未启用

sdkconfig.PaperS3 中：
```ini
# CONFIG_FATFS_USE_FASTSEEK is not set   # ← 缺失
```

`FATFS_USE_FASTSEEK` 启用后，FatFS 会为已打开的文件维护一个簇链路图（CLMT），将随机 seek 的 FAT 链遍历从 O(n) 降低到 O(1)。对于本项目中大量随机读取字体文件中单个字形数据的场景（每次 seek + read 小块），这是最低成本最高回报的优化。

**修复**：在 sdkconfig.PaperS3 中添加：

```ini
CONFIG_FATFS_USE_FASTSEEK=y
```

并在代码侧为长期打开的文件调用 `f_lseek` 时自动使用（Arduino `File::seek()` 底层会走此路径）。

---

#### 问题 7 ★★ — GDMA 中断未配置为 IRAM-safe

```ini
# CONFIG_GDMA_ISR_IRAM_SAFE is not set   # ← 影响 SDMMC DMA 稳定性
```

SDMMC 使用 GDMA 传输数据。如果在 GDMA 中断触发期间 Cache 正忙（如 PSRAM 访问），中断被延迟，等待 Cache miss 完成。对于大量使用 PSRAM 的应用（本项目有 `BOARD_HAS_PSRAM`），这会导致 SDMMC 传输出现不规律的延迟毛刺。

**修复**：

```ini
CONFIG_GDMA_ISR_IRAM_SAFE=y
CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y
```

---

#### 问题 8 ★ — `readAtOffset` 的 DMA 缓冲区大小固定为 8 扇区（4096 字节）

SDWrapper.cpp：

```cpp
static constexpr size_t DMA_BUFFER_SIZE = 4096; // 8个扇区
if (sectors_needed <= 8)  // 只有 ≤8 扇区才走 DMA 池路径
```

对于读取 8 扇区以上的大块（如章节文本），会回落到 `heap_caps_malloc` 动态分配路径，每次分配/释放增加开销。可以适当扩大 `DMA_BUFFER_SIZE` 至 16384（32 扇区），或增加 `DMA_POOL_SIZE = 4`：

```cpp
static constexpr size_t DMA_POOL_SIZE = 3;
static constexpr size_t DMA_BUFFER_SIZE = 16384; // 32扇区，覆盖更大的顺序读
```

---

### 四、改动优先级汇总

| 优先级 | 问题 | 预期效果 | 改动位置 |
|--------|------|----------|----------|
| P0 | `USB.begin()` 顺序错误 | 消除 2 分钟枚举延迟 | usb_msc.cpp |
| P0 | `TINYUSB_MSC_BUFSIZE=512` | 大幅提升传输速度 | sdkconfig.PaperS3 |
| P1 | 避免 SDMMC 重复初始化 | 消除 200-500ms 启动延迟 | usb_msc.cpp |
| P1 | TinyUSB 任务栈扩至 8192 | 消除潜在栈溢出崩溃 | sdkconfig.PaperS3 |
| P2 | `FATFS_USE_FASTSEEK` | 字体随机读提速 20-40% | sdkconfig.PaperS3 |
| P2 | `GDMA_ISR_IRAM_SAFE` | 消除 PSRAM 并发时的 DMA 毛刺 | sdkconfig.PaperS3 |
| P3 | USB 序列号动态化 | 减少 Windows USB 缓存冲突 | usb_msc.cpp |
| P3 | DMA 缓冲区扩大 | 减少大块读取时的动态分配 | SDWrapper.h |

---

### 实际效果

1. P0 层级修改之后：  2 min to 30 second；
2. P1 层级修改风险较高涉及到了句柄重用等，考虑性价比之后不做实现；
3. P2 层级修改后，因为是主要从重启稳定性和字体渲染角度，暂无明显变化；
4. P3 层级修改后，大致在27 second左右；