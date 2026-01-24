#include "ui_time_rec.h"
#include "text/bin_font_print.h"
#include "text/book_handle.h"
#include "current_book.h"
#include "../SD/SDWrapper.h"
#include "globals.h"
#include <map>
#include <vector>
#include <algorithm>
#include "ui/ui_canvas_utils.h"

// 返回按钮区域
static const int BACK_BTN_X = 203;
static const int BACK_BTN_Y = 912;
static const int BACK_BTN_WIDTH = (int)140 * 0.8f;
static const int BACK_BTN_HEIGHT = (int)60 * 0.8f;

// 解析 rec 文件的时间记录
// 返回 map: timestamp(YYYYMMDDHH) -> 分钟数
static std::map<std::string, int32_t> parseRecFile(const std::string &rec_file_path)
{
    std::map<std::string, int32_t> records;

    if (!SDW::SD.exists(rec_file_path.c_str()))
        return records;

    File rf = SDW::SD.open(rec_file_path.c_str(), "r");
    if (!rf)
        return records;

    // 跳过第一行（总时间）
    if (rf.available())
        rf.readStringUntil('\n');

    // 读取后续记录
    while (rf.available())
    {
        String line = rf.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        int colon = line.indexOf(':');
        if (colon > 0)
        {
            String ts = line.substring(0, colon);
            String val = line.substring(colon + 1);

            // 解析 xxm 或 xxhxxm
            int32_t mins = 0;
            int h_pos = val.indexOf('h');
            if (h_pos > 0)
            {
                int hours = val.substring(0, h_pos).toInt();
                int m_pos = val.indexOf('m', h_pos);
                int minutes = 0;
                if (m_pos > h_pos + 1)
                    minutes = val.substring(h_pos + 1, m_pos).toInt();
                mins = hours * 60 + minutes;
            }
            else
            {
                int m_pos = val.indexOf('m');
                if (m_pos > 0)
                    mins = val.substring(0, m_pos).toInt();
            }

            records[ts.c_str()] = mins;
        }
    }
    rf.close();

    return records;
}

// 按天汇总（YYYYMMDD）
static std::map<std::string, int32_t> aggregateByDay(const std::map<std::string, int32_t> &hourly_records)
{
    std::map<std::string, int32_t> daily_records;

    for (const auto &entry : hourly_records)
    {
        // entry.first 是 YYYYMMDDHH，截取前 8 位得到 YYYYMMDD
        if (entry.first.length() >= 8)
        {
            std::string day = entry.first.substr(0, 8);
            daily_records[day] += entry.second;
        }
    }

    return daily_records;
}

void draw_time_rec_screen(M5Canvas *canvas)
{
    if (canvas == nullptr || g_current_book == nullptr)
        return;

    // 清屏
    canvas->fillScreen(TFT_WHITE);

    // 绘制标题
    canvas->fillRect(0, 0, PAPER_S3_WIDTH, 60, TFT_BLACK);
//    canvas->fillRect(100, 0, PAPER_S3_WIDTH - 200, 60, TFT_BLACK);
    canvas->drawRect(0, 900, PAPER_S3_WIDTH, 60);
    drawScrew(canvas, 30, 30);
    drawScrew(canvas, 510, 30);
    drawScrew(canvas, 30, 930);
    drawScrew(canvas, 510, 930);

    bin_font_print("阅读时间记录", 32, TFT_BLACK, PAPER_S3_WIDTH, 0, 14, true, canvas, TEXT_ALIGN_CENTER, 0, false, false, false, true);

    // 获取 rec 文件路径
    std::string rec_file_path = getRecordFileName(g_current_book->filePath());

    // 解析 rec 文件
    auto hourly_records = parseRecFile(rec_file_path);

    // 按天汇总
    auto daily_records = aggregateByDay(hourly_records);

    // 显示总时间
    int16_t total_hour = g_current_book->getReadHour();
    int16_t total_min = g_current_book->getReadMin();
    char total_str[64];
    snprintf(total_str, sizeof(total_str), "总计: %dh%dm", total_hour, total_min);
    bin_font_print(total_str, 26, TFT_BLACK, (PAPER_S3_WIDTH) / 2, PAPER_S3_WIDTH / 2, 75, false, canvas, TEXT_ALIGN_CENTER ,(PAPER_S3_WIDTH/2));
//    bin_font_print(g_current_book->getBookName(), 26, TFT_BLACK, (PAPER_S3_WIDTH) / 2 - 20, 20, 75, false, canvas, TEXT_ALIGN_LEFT);
    bin_font_print(g_current_book->getBookName(), 26, TFT_BLACK, (PAPER_S3_WIDTH) / 2 - 20, 20, 75, false, canvas, TEXT_ALIGN_LEFT,(PAPER_S3_WIDTH/2-20));

    // 如果没有历史记录，显示提示信息
    if (daily_records.empty())
    {
        bin_font_print("暂无历史记录", 24, TFT_BLACK, PAPER_S3_WIDTH, 0, 400, true, canvas, TEXT_ALIGN_CENTER);
    }
    else
    {
        // 绘制图表区域背景（先白底，再黑色边框）
        //        canvas->fillRect(30, 130, 480, 600, TFT_WHITE);
        //        canvas->drawRect(30, 130, 480, 600, TFT_BLACK);

        std::vector<std::pair<std::string, int32_t>> sorted_records;
        for (const auto &entry : daily_records)
        {
            sorted_records.push_back(entry);
        }

        // 按日期降序排序
        std::sort(sorted_records.begin(), sorted_records.end(),
                  [](const std::pair<std::string, int32_t> &a, const std::pair<std::string, int32_t> &b)
                  {
                      return a.first > b.first;
                  });

        // 只取最近的记录（最近7天）
        size_t max_display = 7;
        if (sorted_records.size() > max_display)
        {
            // sorted_records 当前为降序(新->旧)，保留最近7天
            sorted_records.resize(max_display);
        }
        // 转为升序(旧->新)用于从上到下显示
        std::reverse(sorted_records.begin(), sorted_records.end());

        // 找出最大值用于归一化
        int32_t max_minutes = 1;
        for (const auto &entry : sorted_records)
        {
            if (entry.second > max_minutes)
                max_minutes = entry.second;
        }

        // 绘制横向条形图：始终显示最近7天（纵轴为7天），横轴为分钟数
        // 生成最近7天日期（YYYYMMDD），从最旧到最新
        time_t now = time(nullptr);
        struct tm tnow;
        localtime_r(&now, &tnow);
        std::vector<std::string> last7;
        last7.reserve(7);
        for (int i = 0; i < 7; ++i)
        {
            struct tm td = tnow;
            int days_ago = 6 - i; // i=0 -> oldest (6 days ago)
            td.tm_mday -= days_ago;
            mktime(&td);
            char ymd[32];
            int y = td.tm_year + 1900;
            int m = td.tm_mon + 1;
            int d = td.tm_mday;
            snprintf(ymd, sizeof(ymd), "%04d%02d%02d", y, m, d);
            last7.push_back(std::string(ymd));
        }

        // 为这7天准备分钟数据（无数据则为0）
        std::vector<int32_t> mins_vec(7, 0);
        for (int i = 0; i < 7; ++i)
        {
            auto it = daily_records.find(last7[i]);
            if (it != daily_records.end())
                mins_vec[i] = it->second;
        }

        // 重新计算 max_minutes 基于这7天
        max_minutes = 1;
        for (int v : mins_vec)
            if (v > max_minutes)
                max_minutes = v;

        // chart box (the big box drawn earlier) defines left/top/bottom
        const int box_left = 30;
        const int box_top = 100;
        // const int box_width = 480;
        const int box_height = 600;
        // const int box_bottom = box_top + box_height;

        const int chart_left = box_left + 50; // 内部左边距，给日期留出宽度
        const int chart_width = PAPER_S3_WIDTH - 120;
        const int bar_height = 28;
        const int row_gap = 10;
        // const int total_height = 7 * bar_height + (7 - 1) * row_gap;
        const int chart_top = box_top + 20; //

        for (int i = 0; i < 7; ++i)
        {
            int y = chart_top + i * (bar_height + row_gap);

            // 日期标签（MM/DD）
            std::string date_str = last7[i];
            if (date_str.length() >= 8)
            {
                char label[16];
                snprintf(label, sizeof(label), "%c%c/%c%c",
                         date_str[4], date_str[5], date_str[6], date_str[7]);
                bin_font_print(label, 16, TFT_BLACK, 120, 20, y + 6, false, canvas, TEXT_ALIGN_LEFT);
            }

            int32_t minutes = mins_vec[i];
            int bar_len = (minutes * chart_width) / max_minutes;
            if (bar_len < 2 && minutes > 0)
                bar_len = 2;

            // 实心黑条
            canvas->fillRect(chart_left, y, bar_len, bar_height, TFT_BLACK);

            // 分钟数标签放在条形末尾
            /*
            char mins_label[16];
            snprintf(mins_label, sizeof(mins_label), "%dm", minutes);
            int label_x = chart_left + bar_len + 6;
            if (label_x > box_left + box_width - 80)
                label_x = box_left + box_width - 80;
            bin_font_print(mins_label, 12, TFT_BLACK, 80, label_x, y + 8, false, canvas, TEXT_ALIGN_LEFT);
            */
        }

        // 绘制图例说明
        /*
        char legend[64];
        int32_t max_hours = max_minutes / 60;
        int32_t max_mins = max_minutes % 60;
        snprintf(legend, sizeof(legend), "最高: %ldh%ldm", (long)max_hours, (long)max_mins);
        bin_font_print(legend, 18, TFT_BLACK, 200, 50, 750, false, canvas, TEXT_ALIGN_LEFT);
        */

        // 在绘制返回按钮之前，增加横轴刻度（0、max/2、max）
        int x0 = chart_left;                                     // chart_left
        int x1 = chart_left + chart_width;                       // chart_left + chart_width
        int axis_y = 140 + (bar_height + row_gap) * 7 - row_gap; // 最后一行下方

        // 画横轴与刻度线
        canvas->drawLine(x0, axis_y, x1, axis_y, TFT_BLACK);
        canvas->drawLine(x0, axis_y - 4, x0, axis_y + 4, TFT_BLACK);
        canvas->drawLine(x0 + (x1 - x0) / 2, axis_y - 4, x0 + (x1 - x0) / 2, axis_y + 4, TFT_BLACK);
        canvas->drawLine(x1, axis_y - 4, x1, axis_y + 4, TFT_BLACK);

        // 标签文本（分钟转 HhMm）
        auto format_min_label = [](int32_t mins, char *buf, size_t sz)
        {
            if (mins >= 60)
            {
                int h = mins / 60;
                int m = mins % 60;
                snprintf(buf, sz, "%dh%dm", h, m);
            }
            else
            {
                snprintf(buf, sz, "%dm", mins);
            }
        };

        char lbl[32];
        // 0
        format_min_label(0, lbl, sizeof(lbl));
        bin_font_print(lbl, 18, TFT_BLACK, 80, x0 - 10, axis_y + 8, false, canvas, TEXT_ALIGN_LEFT);
        // max/2
        format_min_label(max_minutes / 2, lbl, sizeof(lbl));
        bin_font_print(lbl, 18, TFT_BLACK, 80, x0 + (x1 - x0) / 2 - 10, axis_y + 8, false, canvas, TEXT_ALIGN_LEFT);
        // max
        format_min_label(max_minutes, lbl, sizeof(lbl));
        bin_font_print(lbl, 18, TFT_BLACK, 80, x1 - 10, axis_y + 8, false, canvas, TEXT_ALIGN_LEFT);

        // 在当前图下方绘制最近6个月的每月累计分钟图（竖向柱状图：横轴=月份，纵轴=分钟数）
        const int month_chart_w = 440;
        const int month_chart_h = 100;
        const int month_left = 80;
        const int month_top = axis_y + 50; // 在日度图横轴下方留空

        // 生成最近6个月（从最旧到最新），并保存年/月
        struct tm tnow2;
        time_t now2 = time(nullptr);
        localtime_r(&now2, &tnow2);
        std::vector<std::pair<int, int>> months;
        months.reserve(6);
        for (int i = 0; i < 6; ++i)
        {
            struct tm tm2 = tnow2;
            int months_ago = 5 - i; // i=0 -> oldest (5 months ago)
            tm2.tm_mon -= months_ago;
            mktime(&tm2);
            int yy = tm2.tm_year + 1900;
            int mm = tm2.tm_mon + 1;
            months.push_back(std::make_pair(yy, mm));
        }

        // 汇总每个月的分钟数（来源于 daily_records 的 YYYYMMDD 前缀）
        std::vector<int32_t> month_mins(6, 0);
        for (const auto &dr : daily_records)
        {
            if (dr.first.length() >= 6)
            {
                std::string ymon = dr.first.substr(0, 6); // YYYYMM
                int y = atoi(ymon.substr(0, 4).c_str());
                int m = atoi(ymon.substr(4, 2).c_str());
                for (int i = 0; i < 6; ++i)
                {
                    if (months[i].first == y && months[i].second == m)
                        month_mins[i] += dr.second;
                }
            }
        }

        // 计算最大值用于归一化
        int32_t max_month = 1;
        for (int v : month_mins)
            if (v > max_month)
                max_month = v;

        // 绘制竖向柱状图（6个柱子）
        const int bar_width = 53;
        const int bar_gap = 20;
        const int chart_bottom = month_top + month_chart_h;

        canvas->fillRect(month_left, month_top, month_chart_w, month_chart_h, TFT_LIGHTGRAY);
        // 画横轴（底边）
        canvas->drawLine(month_left, chart_bottom, month_left + month_chart_w, chart_bottom, TFT_BLACK);

        for (int i = 0; i < 6; ++i)
        {
            int x = month_left + i * (bar_width + bar_gap);

            // 计算柱子高度
            int32_t mins = month_mins[i];
            int bar_h = (mins * (month_chart_h - 40)) / max_month;
            if (bar_h < 2 && mins > 0)
                bar_h = 2;

            int bar_top = chart_bottom - bar_h;

            // 实心黑色柱子
            canvas->fillRect(x, bar_top, bar_width, bar_h, TFT_BLACK);

            // 月份标签（横轴下方）
            char mon_lbl[16];
            snprintf(mon_lbl, sizeof(mon_lbl), "%02d/%02d", months[i].second, months[i].first % 100);
            bin_font_print(mon_lbl, 14, TFT_BLACK, bar_width + 20, x - 10, chart_bottom + 8, false, canvas, TEXT_ALIGN_CENTER);

            // 分钟数标签（柱子顶部上方）
            if (mins > 0)
            {
                char mval[32];
                if (mins >= 60)
                {
                    int h = mins / 60;
                    int m = mins % 60;
                    snprintf(mval, sizeof(mval), "%dh%dm", h, m);
                }
                else
                {
                    snprintf(mval, sizeof(mval), "%dm", mins);
                }
                bin_font_print(mval, 16, TFT_BLACK, bar_width + 20, x - 10, bar_top - 18, false, canvas, TEXT_ALIGN_CENTER);
            }
        }

        // bin_font_print("近六月", 20, TFT_BLACK, 120, month_left, chart_bottom + 10,false,canvas,TEXT_ALIGN_CENTER,120,true,true,true);
        bin_font_print("近六月", 24, TFT_BLACK, 120, 440 , 480 , false, canvas, TEXT_ALIGN_CENTER, 120, true, false, true);

        // 在月度图下方绘制时段分布图（高100，宽500）
        // 统计四个时段：04:00-12:00, 12:00-20:00, 20:00-04:00, 无小时信息
        // const int dist_w = 440;
        // const int dist_h = 60;
        // const int dist_left = month_left;
        const int dist_top = chart_bottom + 30; // 在月度图下方

        // 统计四个时段的分钟数
        int32_t morning_mins = 0;   // 04:00-12:00
        int32_t afternoon_mins = 0; // 12:00-20:00
        int32_t night_mins = 0;     // 20:00-04:00
        int32_t unknown_mins = 0;   // 无小时信息

        // 从书籍记录中获取总时间（分钟）
        int32_t total_mins = g_current_book->getReadHour() * 60 + g_current_book->getReadMin();

        for (const auto &entry : hourly_records)
        {
            const std::string &ts = entry.first; // YYYYMMDDHH
            int32_t mins = entry.second;

            if (ts.length() >= 10)
            {
                // 提取小时（最后两位）
                int hour = atoi(ts.substr(8, 2).c_str());

                if (hour >= 4 && hour < 12)
                {
                    morning_mins += mins;
                }
                else if (hour >= 12 && hour < 20)
                {
                    afternoon_mins += mins;
                }
                else // 20:00-04:00 (20-23, 0-3)
                {
                    night_mins += mins;
                }
            }
        }

        // unknown_mins为总时间减去已知时段的时间
        unknown_mins = total_mins - (morning_mins + afternoon_mins + night_mins);
        if (unknown_mins < 0)
            unknown_mins = 0;

        // 如果总时间为0，全部算无信息
        if (total_mins == 0)
        {
            total_mins = 1;
            unknown_mins = 1;
        }

        // 绘制饼图（半径180）
        const int pie_radius = 90;
        const int pie_center_x = 130; // 屏幕中心
        const int pie_center_y = dist_top + 120;

        // 计算每个时段的角度（360度对应总时间）
        float angle_morning = (morning_mins * 360.0f) / total_mins;
        float angle_afternoon = (afternoon_mins * 360.0f) / total_mins;
        float angle_night = (night_mins * 360.0f) / total_mins;
        float angle_unknown = (unknown_mins * 360.0f) / total_mins;

        // 起始角度（从0度开始，顺时针）
        float start_angle = 0;

        // 1. 04:00-12:00 - TFT_WHITE
        if (morning_mins > 0)
        {
            canvas->fillArc(pie_center_x, pie_center_y, pie_radius, 0, start_angle, start_angle + angle_morning, TFT_WHITE);
            // canvas->drawArc(pie_center_x, pie_center_y, pie_radius, pie_radius, start_angle, start_angle + angle_morning, TFT_BLACK);
            canvas->drawArc(pie_center_x, pie_center_y, 0, pie_radius, start_angle, start_angle + angle_morning, TFT_BLACK);
            start_angle += angle_morning;
        }

        // 2. 12:00-20:00 - TFT_BLACK
        if (afternoon_mins > 0)
        {
            canvas->fillArc(pie_center_x, pie_center_y, pie_radius, 0, start_angle, start_angle + angle_afternoon, TFT_BLACK);
            start_angle += angle_afternoon;
        }

        // 3. 20:00-04:00 - TFT_WHITE
        if (night_mins > 0)
        {
            canvas->fillArc(pie_center_x, pie_center_y, pie_radius, 0, start_angle, start_angle + angle_night, TFT_DARKGRAY);
            // canvas->drawArc(pie_center_x, pie_center_y, pie_radius, pie_radius, start_angle, start_angle + angle_night, TFT_BLACK);
            canvas->drawArc(pie_center_x, pie_center_y, 0, pie_radius, start_angle, start_angle + angle_night, TFT_BLACK);
            start_angle += angle_night;
        }

        // 4. 无信息 - TFT_DARKGREY
        if (unknown_mins > 0)
        {
            canvas->fillArc(pie_center_x, pie_center_y, pie_radius, 0, start_angle, start_angle + angle_unknown, TFT_LIGHTGRAY);
            // canvas->drawArc(pie_center_x, pie_center_y, pie_radius, pie_radius, start_angle, start_angle + angle_unknown, TFT_BLACK);
            canvas->drawArc(pie_center_x, pie_center_y, 0, pie_radius, start_angle, start_angle + angle_unknown, TFT_BLACK);
        }

        // 绘制饼图外圆边框
        canvas->drawCircle(pie_center_x, pie_center_y, pie_radius, TFT_BLACK);

        // 添加标签说明
        char label[64];
        // int label_y = pie_center_y + pie_radius + 20;

        /*
        snprintf(label, sizeof(label), "04-12: %d%% | 12-20: %d%% | 20-04: %d%% | 未知: %d%%",
                 (int)((morning_mins * 100) / total_mins),
                 (int)((afternoon_mins * 100) / total_mins),
                 (int)((night_mins * 100) / total_mins),
                 (int)((unknown_mins * 100) / total_mins));
        bin_font_print(label, 16, TFT_BLACK, dist_w, dist_left, label_y, false, canvas, TEXT_ALIGN_CENTER);
        */
        canvas->drawRect(pie_center_x + 120, pie_center_y - 60, 16, 16);
        snprintf(label, sizeof(label), "[04-12]: %d mins ", morning_mins);
        bin_font_print(label, 18, TFT_BLACK, 260, pie_center_x + 150, pie_center_y - 60, false, canvas, TEXT_ALIGN_LEFT);

        canvas->drawRect(pie_center_x + 120, pie_center_y - 20, 16, 16);
        canvas->fillRect(pie_center_x + 122, pie_center_y - 18, 12, 12, TFT_BLACK);
        snprintf(label, sizeof(label), "[12-20]: %d mins ", afternoon_mins);
        bin_font_print(label, 18, TFT_BLACK, 260, pie_center_x + 150, pie_center_y - 20, false, canvas, TEXT_ALIGN_LEFT);

        canvas->drawRect(pie_center_x + 120, pie_center_y + 20, 16, 16);
        canvas->fillRect(pie_center_x + 122, pie_center_y + 22, 12, 12, TFT_DARKGRAY);
        snprintf(label, sizeof(label), "[20-04]: %d mins ", night_mins);
        bin_font_print(label, 18, TFT_BLACK, 260, pie_center_x + 150, pie_center_y + 20, false, canvas, TEXT_ALIGN_LEFT);

        canvas->drawRect(pie_center_x + 120, pie_center_y + 60, 16, 16);
        canvas->fillRect(pie_center_x + 122, pie_center_y + 62, 12, 12, TFT_LIGHTGRAY);
        snprintf(label, sizeof(label), "[未知]: %d mins ", unknown_mins);
        bin_font_print(label, 18, TFT_BLACK, 260, pie_center_x + 150, pie_center_y + 60, false, canvas, TEXT_ALIGN_LEFT);

        bin_font_print("时段统计", 24, TFT_BLACK, 140, pie_center_y - 68, 20, false, canvas, TEXT_ALIGN_CENTER, 120, true, false, true);

        bin_font_print("完整报告请于浏览器扩展查阅和导出", 24, TFT_BLACK, PAPER_S3_WIDTH, 0, pie_center_y + 160, false, canvas, TEXT_ALIGN_CENTER);
    }

    // draw_button(canvas, BACK_BTN_X, BACK_BTN_Y, "返回阅读", true,false, 0.8f);
    bin_font_print("返回阅读", 32, 0, 540, 0, BACK_BTN_Y, false, canvas, TEXT_ALIGN_CENTER);
}

bool is_point_in_time_rec_back_button(int16_t x, int16_t y)
{
    return (x >= BACK_BTN_X && x < (BACK_BTN_X + BACK_BTN_WIDTH) &&
            y >= BACK_BTN_Y && y < (BACK_BTN_Y + BACK_BTN_HEIGHT));
}
