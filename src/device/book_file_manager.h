#pragma once

#include "readpaper.h"
#include "efficient_file_scanner.h"
#include <vector>
#include <string>

// 书籍文件管理器 - 专门处理书籍文件的高效扫描和缓存
class BookFileManager {
public:
    // 获取书籍文件数量（缓存优化）
    static int getBookCount();
    
    // 获取指定页面的书籍列表（分页支持）
    static std::vector<std::string> getBookList(int page, int perPage);
    
    // 获取所有书籍名称（去除.txt扩展名）；目录条目以 '/' 结尾，'..' 表示返回上级
    static std::vector<std::string> getAllBookNames();
    
    // 刷新缓存
    static void refreshCache();
    
    // 检查特定书籍是否存在
    static bool bookExists(const std::string& bookName);
    
    // 获取书籍文件大小
    static size_t getBookSize(const std::string& bookName);
    
    // 清除缓存
    static void clearCache();

    // ── 目录导航 ──
    // 返回当前扫描目录（SD 路径，以 "/book" 开头）
    static std::string getCurrentScanDir();
    // 是否处于根目录 /book
    static bool isAtRootBookDir();
    // 进入子目录（dirName 只含目录名，不含路径）
    static void navigateTo(const std::string& dirName);
    // 返回上级目录（若已在根目录则不变）
    static void navigateUp();
    
private:
    static std::vector<std::string> cachedBookNames;
    static bool cacheValid;
    static unsigned long lastScanTime;
    static std::string currentScanDir; // 当前扫描目录，默认 "/book"
    
    // 内部扫描函数
    static void scanBooks();
    
    // 检查缓存是否需要刷新
    static bool shouldRefreshCache();
    
    // 从文件名移除扩展名
    static std::string removeExtension(const std::string& filename, const std::string& ext = ".txt");
};