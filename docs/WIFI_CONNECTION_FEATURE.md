# WiFi连接设置功能说明

## 功能概述
本次更新实现了从离线配置文件连接WiFi AP的功能，并添加了全局变量来跟踪连接状态。

## 实现的功能

### 1. 全局WiFi连接状态变量
- **文件**: `src/globals.h`, `src/globals.cpp`
- **变量**: `g_wifi_sta_connected`
- **类型**: `bool`
- **默认值**: `false`
- **说明**: 用于表示设备是否成功连接到WiFi AP

### 2. Token配置文件
- **文件**: `data/token.json`
- **格式**:
```json
{
    "wifi_ap_name": "YourWiFiName",
    "wifi_ap_password": "YourWiFiPassword"
}
```
- **说明**: 保存WiFi AP的SSID和密码，用户需要修改这两个字段为实际的WiFi信息

### 3. WiFi连接功能
- **文件**: `src/device/wifi_hotspot_manager.h`, `src/device/wifi_hotspot_manager.cpp`
- **函数**: 
  - `bool connectToWiFiFromToken()`: 从token.json读取配置并尝试连接WiFi
  - `void disconnectWiFi()`: 断开WiFi连接
- **特性**:
  - 自动读取并解析token.json文件
  - 连接超时时间为10秒
  - 成功时设置 `g_wifi_sta_connected = true`
  - 失败时设置 `g_wifi_sta_connected = false`

### 4. "连接设置"按钮集成
- **文件**: `src/tasks/state_2nd_level_menu.cpp`
- **功能**: 
  - 点击"连接设置"按钮时自动读取token.json
  - 显示等待画面
  - 尝试连接WiFi
  - 无论成功或失败都返回主菜单
  - 通过 `g_wifi_sta_connected` 变量可以查询连接结果

## 使用方法

### 1. 配置WiFi信息
编辑 `data/token.json` 文件，填入实际的WiFi信息：
```json
{
    "wifi_ap_name": "MyHomeWiFi",
    "wifi_ap_password": "my_secure_password"
}
```

### 2. 上传文件到SPIFFS
将token.json文件上传到设备的SPIFFS分区（路径：`/spiffs/token.json`）

### 3. 使用连接功能
1. 进入二级菜单（CONN/USB界面）
2. 点击"连接设置"按钮
3. 系统会显示等待画面并尝试连接
4. 无论成功失败都会返回主菜单

### 4. 检查连接状态
在代码中可以通过以下方式检查WiFi连接状态：
```cpp
extern bool g_wifi_sta_connected;

if (g_wifi_sta_connected) {
    // WiFi已连接
    Serial.println("WiFi is connected");
} else {
    // WiFi未连接
    Serial.println("WiFi is not connected");
}
```

## 调试信息
当 `DBG_WIFI_HOTSPOT` 宏启用时，会输出详细的调试信息：
- token.json文件读取状态
- JSON解析结果
- WiFi连接尝试过程
- 连接成功/失败信息
- IP地址（连接成功时）

## 注意事项
1. token.json必须放在SPIFFS中（/spiffs/token.json），而不是SD卡
2. 如果当前正在运行热点模式（AP模式），连接WiFi时会先停止热点
3. 连接超时时间为10秒，超时会自动失败
4. WiFi连接失败不会影响系统运行，只是设置状态变量为false
5. 可以通过 `disconnectWiFi()` 函数手动断开WiFi连接

## 代码改动文件清单
- `src/globals.h` - 添加WiFi连接状态全局变量声明
- `src/globals.cpp` - 初始化WiFi连接状态变量
- `data/token.json` - 新建WiFi配置文件
- `src/device/wifi_hotspot_manager.h` - 添加WiFi连接函数声明
- `src/device/wifi_hotspot_manager.cpp` - 实现WiFi连接功能
- `src/tasks/state_2nd_level_menu.cpp` - 集成到"连接设置"按钮处理
