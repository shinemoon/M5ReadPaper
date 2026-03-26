#!/usr/bin/env python3
"""
快速检查设备侧 guide API 响应
"""
import requests
import json
import sys

def check_guide_api(host='192.168.4.1'):
    """检查 device_guide API 响应"""
    url = f'http://{host}/api/device_guide'
    print(f'[*] 正在检查 {url}')
    
    try:
        resp = requests.get(url, timeout=5)
        print(f'[+] HTTP {resp.status_code}')
        
        if resp.status_code == 200:
            data = resp.json()
            print(f'[+] guide 原始响应:')
            print(json.dumps(data, indent=2, ensure_ascii=False))
            
            # 检查关键字段
            tm = data.get('timeManagement', {})
            allowed = tm.get('allowSyncTime')
            print(f'\n[*] timeManagement.allowSyncTime = {allowed}')
            
            if allowed:
                print('[✓] 时间同步应该在 webapp 中启用了')
            else:
                print('[✗] 时间同步在 webapp 中被禁用了 - 这可能是问题原因')
                
            return data
        else:
            print(f'[✗] API 返回错误状态码')
            
    except Exception as e:
        print(f'[✗] 无法连接设备: {e}')
        print(f'    确保设备 WiFi AP 已启动，并且你的电脑已连接到 {host}')
        
    return None

if __name__ == '__main__':
    host = sys.argv[1] if len(sys.argv) > 1 else '192.168.4.1'
    check_guide_api(host)
