# 截图功能使用说明

## 概述

screenshot功能允许将当前显示的内容保存为PNG图片到SD卡。

## 使用方法

### 1. 在代码中调用

```cpp
#include "ui/screenshot.h"

// 在需要截图的地方调用
if (screenShot()) {
    // 截图成功
    Serial.println("截图已保存");
} else {
    // 截图失败
    Serial.println("截图失败");
}
```

### 2. 示例：在调试菜单中添加截图按钮

在 `state_debug_state.cpp` 或其他状态处理文件中：

```cpp
#include "ui/screenshot.h"

// 在触摸处理部分
if (某个按钮被点击) {
    if (screenShot()) {
        // 显示成功提示
        Serial.println("[DEBUG] 截图成功");
    }
}
```

## 输出文件

- **目录**: `/screenshot/`
- **命名格式**: `readpaper_screen_YYYY_MM_DD_HH_MM_SS.png`
- **示例**: `readpaper_screen_2026_01_19_14_30_45.png`

## 图片格式

- **格式**: PNG
- **颜色**: 8位灰度（从M5Paper的4位灰度扩展而来）
- **尺寸**: 540x960（PAPER_S3_WIDTH x PAPER_S3_HEIGHT）

## 目录管理

- 初始化时会自动创建 `/screenshot` 目录（如果不存在）
- 出厂重置时会清空 `/screenshot` 目录
- 清理残存功能不会影响截图目录

## 调试

在 `per_file_debug.h` 中启用调试输出：

```cpp
#define DBG_SCREENSHOT 1
```

这将在串口输出详细的截图过程信息。

## 注意事项

1. 确保SD卡有足够空间（每张截图约100-500KB，取决于内容）
2. 截图功能需要zlib库支持（已包含在platformio.ini中）
3. 截图会捕获当前 `g_canvas` 的内容，确保在截图前已经渲染完成
4. 时间戳使用系统时间，确保时间已正确设置

## API参考

### screenShot()

```cpp
bool screenShot()
```

截取当前 g_canvas 的内容并保存为PNG图片。

**返回值**:
- `true`: 截图成功
- `false`: 截图失败（可能原因：SD卡错误、内存不足、g_canvas为空等）

### ensureScreenshotFolder()

```cpp
bool ensureScreenshotFolder()
```

确保SD卡上存在 `/screenshot` 文件夹。

**返回值**:
- `true`: 文件夹存在或创建成功
- `false`: 创建失败
