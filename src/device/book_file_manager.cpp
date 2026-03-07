#include "book_file_manager.h"
#include "globals.h"
#include "text/font_buffer.h"
#include <algorithm>
#include <cctype>
#include "readpaper.h"

extern GlobalConfig g_config;

// 静态成员初始化
std::vector<std::string> BookFileManager::cachedBookNames;
bool BookFileManager::cacheValid = false;
unsigned long BookFileManager::lastScanTime = 0;
std::string BookFileManager::currentScanDir = "/book";

int BookFileManager::getBookCount() {
    if (shouldRefreshCache()) {
        scanBooks();
    }
    return (int)cachedBookNames.size();
}

std::vector<std::string> BookFileManager::getBookList(int page, int perPage) {
    if (shouldRefreshCache()) {
        scanBooks();
    }
    
    std::vector<std::string> result;
    if (page < 1 || perPage < 1) return result;
    
    int startIndex = (page - 1) * perPage;
    int endIndex = startIndex + perPage;
    
    for (int i = startIndex; i < endIndex && i < (int)cachedBookNames.size(); i++) {
        result.push_back(cachedBookNames[i]);
    }
    
    return result;
}

std::vector<std::string> BookFileManager::getAllBookNames() {
    if (shouldRefreshCache()) {
        scanBooks();
    }
    return cachedBookNames;
}

void BookFileManager::refreshCache() {
    cacheValid = false;
    scanBooks();
}

bool BookFileManager::bookExists(const std::string& bookName) {
    // 构建完整文件路径
    std::string fullPath = "/book/" + bookName + ".txt";
    return EfficientFileScanner::fileExists(fullPath);
}

size_t BookFileManager::getBookSize(const std::string& bookName) {
    std::string fullPath = "/book/" + bookName + ".txt";
    return EfficientFileScanner::getFileSize(fullPath);
}

void BookFileManager::clearCache() {
    cachedBookNames.clear();
    cacheValid = false;
    lastScanTime = 0;
}

void BookFileManager::scanBooks() {
    if (cacheValid) return;
    
    // 检查内存状态
    if (ESP.getFreeHeap() < 8192) {
#if DBG_FILE_MANAGER
        Serial.printf("[BookFileManager] 内存不足 (%d bytes)，跳过扫描\n", ESP.getFreeHeap());
#endif
        return;
    }
    
#if DBG_FILE_MANAGER
    Serial.printf("[BookFileManager] 开始扫描目录: %s，剩余内存: %d bytes\n", currentScanDir.c_str(), ESP.getFreeHeap());
    unsigned long startTime = millis();
#endif
    
    cachedBookNames.clear();

    // 扩展扫描：无扩展名过滤，同时获取目录和文件
    std::vector<FileInfo> allItems = EfficientFileScanner::scanDirectory(currentScanDir, "");

    std::vector<std::string> dirEntries;  // 目录条目，以 '/' 结尾
    std::vector<std::string> fileEntries; // txt 文件条目，去除 .txt
    bool scanSuccess = true;

    for (const auto& item : allItems) {
        if (ESP.getFreeHeap() < 4096) {
#if DBG_FILE_MANAGER
            Serial.printf("[BookFileManager] 内存不足，停止处理\n");
#endif
            scanSuccess = false;
            break;
        }

        if (item.isDirectory) {
            if (!item.name.empty() && item.name.length() <= 255) {
                dirEntries.push_back(item.name + "/");
            }
        } else {
            // 只接受 .txt 文件
            const std::string &n = item.name;
            if (n.size() > 4) {
                std::string ext = n.substr(n.size() - 4);
                for (char &c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".txt") {
                    std::string bookName = n.substr(0, n.size() - 4);
                    if (!bookName.empty() && bookName.length() <= 255) {
                        fileEntries.push_back(bookName);
                    }
                }
            }
        }

        // 全局条目数限制
        {
            size_t runtimeLimit = (size_t)g_config.main_menu_file_count;
            size_t cap = (size_t)MAX_MAIN_MENU_FILE_COUNT;
            size_t effective = runtimeLimit < cap ? runtimeLimit : cap;
            if (dirEntries.size() + fileEntries.size() >= effective) {
#if DBG_FILE_MANAGER
                Serial.printf("[BookFileManager] 已达到%d个条目限制，停止处理\n", (int)effective);
#endif
                break;
            }
        }
    }

    if (scanSuccess && ESP.getFreeHeap() > 4096) {
        // 内部对比函数：大小写不敏感字典序（忽略结尾 '/')
        auto ci_less = [](const std::string &a, const std::string &b) {
            // 对于目录条目，比较时忽略结尾 '/'
            size_t na = a.size(); if (na > 0 && a.back() == '/') na--;
            size_t nb = b.size(); if (nb > 0 && b.back() == '/') nb--;
            for (size_t i = 0; i < na && i < nb; ++i) {
                char ca = a[i], cb = b[i];
                if ((unsigned char)ca >= 'A' && (unsigned char)ca <= 'Z') ca = ca - 'A' + 'a';
                if ((unsigned char)cb >= 'A' && (unsigned char)cb <= 'Z') cb = cb - 'A' + 'a';
                if (ca < cb) return true;
                if (ca > cb) return false;
            }
            return na < nb;
        };
        std::sort(dirEntries.begin(), dirEntries.end(), ci_less);
        std::sort(fileEntries.begin(), fileEntries.end(), ci_less);

        // 非根目录时在首位添加返回上级条目
        if (currentScanDir != "/book") {
            cachedBookNames.push_back("..");
        }
        for (auto &d : dirEntries) cachedBookNames.push_back(d);
        for (auto &f : fileEntries) cachedBookNames.push_back(f);

        cacheValid = true;
        lastScanTime = millis();

#if DBG_FILE_MANAGER
        Serial.printf("[BookFileManager] 扫描完成，目录%d个+文件%d个，耗时: %lu ms，剩余内存: %d bytes\n",
                     (int)dirEntries.size(), (int)fileEntries.size(), millis() - startTime, ESP.getFreeHeap());
#endif

        // 字体缓存仅对文件条目构建（过滤目录条目）
        if (!fileEntries.empty() && ESP.getFreeHeap() > 32768) {
            addBookNamesToCache(fileEntries);
        }
    } else {
#if DBG_FILE_MANAGER
        Serial.printf("[BookFileManager] 扫描失败或内存不足，清空缓存\n");
#endif
        cachedBookNames.clear();
        cacheValid = false;
    }
}

std::string BookFileManager::getCurrentScanDir() {
    return currentScanDir;
}

bool BookFileManager::isAtRootBookDir() {
    return currentScanDir == "/book";
}

void BookFileManager::navigateTo(const std::string& dirName) {
    // dirName 必须不含 '/' 分隔符且不为空
    if (dirName.empty() || dirName.find('/') != std::string::npos) return;
    currentScanDir = currentScanDir + "/" + dirName;
    clearCache();
}

void BookFileManager::navigateUp() {
    if (currentScanDir == "/book") return; // 已在根目录
    size_t last = currentScanDir.rfind('/');
    if (last == std::string::npos || last == 0) {
        currentScanDir = "/book";
    } else {
        currentScanDir = currentScanDir.substr(0, last);
        // 保证常规情况下不会退到 /book 以上
        if (currentScanDir.rfind("/book", 0) != 0) {
            currentScanDir = "/book";
        }
    }
    clearCache();
}

bool BookFileManager::shouldRefreshCache() {
    // 如果缓存无效，需要刷新
    if (!cacheValid) return true;
    
    // 如果超过30秒，考虑刷新缓存
    if (millis() - lastScanTime > 30000) {
        cacheValid = false;
        return true;
    }
    
    // 缓存仍然有效
    return false;
}

std::string BookFileManager::removeExtension(const std::string& filename, const std::string& ext) {
    if (filename.length() > ext.length()) {
        size_t pos = filename.length() - ext.length();
        if (filename.substr(pos) == ext) {
            return filename.substr(0, pos);
        }
    }
    return filename;
}