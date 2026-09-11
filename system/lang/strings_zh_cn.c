/* Simplified Chinese string table. Index-aligned with lang_strings.h STR_* enum. */
#include "lang_strings.h"

const char *const strings_zh_cn[STR_COUNT] = {
    /* App display names */
    "播客",
    "设置",

    /* Settings — main menu */
    "通用",
    "无线网络",
    "存储",
    "更新",
    "关于",

    /* Settings — General */
    "时区",
    "语言",
    "24 小时制",
    "睡眠超时",
    "自动关机",
    "恢复出厂设置",
    "确定清除所有设置和已下载的数据？\n设备将重启。",
    "清除并重启",
    "取消",
    "永不\n1 分钟\n2 分钟\n5 分钟\n10 分钟\n15 分钟\n30 分钟\n60 分钟",
    "永不\n5 分钟\n10 分钟\n15 分钟\n30 分钟\n60 分钟",

    /* Settings — WiFi */
    "WiFi 已连接",
    "连接失败",
    "扫描网络",
    "正在扫描...",
    "可用网络 (%d)",

    /* Settings — WiFi Connect */
    "连接",
    "输入密码",
    "密码",
    "显示密码",

    /* Settings — Storage */
    "未插入 SD 卡 ~",
    "存储用量",
    "清理存储",

    /* Settings — Update */
    "自动更新",
    "检查更新",
    "OTA 更新",
    "电量需至少 30%。\n更新过程中请勿断电。",
    "确认",
    "已是最新版本",
    "发布日期: %s",
    "立即更新",
    "未配置更新地址",
    "服务器不可达",
    "服务器响应无效",
    "无网络连接",

    /* Settings — About */
    "设备名称",
    "设备 ID",
    "固件版本",
    "SDK",
    "硬件",
    "LVGL",

    /* Settings — OTA status */
    "OTA 状态",
    "正在升级。 #FF0000 请勿断电。#",
    "更新失败",

    /* Timezone options */
    "UTC+8 北京",
    "UTC+0 伦敦",
    "UTC-5 纽约",
    "UTC+9 东京",

    /* Podcast — splash / card / tab bar */
    "欢迎使用\nNomadCast",
    "%d 集",
    "网络",
    "本地",
    "播放",
    "我的",

    /* Podcast — network */
    "无网络。\n请在设置中连接 WiFi。",
    "打开设置",
    "加载中",
    "请求超时，请重试。",
    "加载内容失败",
    "搜索...",
    "重试",
    "全部\n时事\n科技\n人文\n生活\n教育\n其他",

    /* Podcast — local */
    "删除 \"%s\"？",
    "删除",
    "这将删除该频道下\n所有已下载的音频文件。",
    "前往网络",
    "未插入 SD 卡，无本地内容。",
    "尚未下载任何音频。请前往网络页下载。",

    /* Podcast — channel */
    "M4A 需下载后播放",
    "加载剧集失败。\n请检查网络连接。",
    "未知",
    "已加入下载队列",
    "删除 %d 集？",
    "下载",
    "频道不存在",
    "全部",

    /* Podcast — player */
    "标题",
    "时长",
    "移除",
    "按时间停止",
    "分钟",
    "按集数停止",
    "集",
    "暂无播放",

    /* Podcast — profile */
    "欢迎",
    "登录",
    "注册",
    "退出登录",
    "确定要退出登录吗？",
    "下载任务",
    "下载任务(%d)",
    "未登录",
    "点击登录",
    "播放时长",
    "下载数",

    /* Podcast — search */
    "搜索",
    "搜索播客...",
    "清除搜索历史",
    "搜索历史已清除",

    /* Podcast — search results */
    "未找到结果",
    "频道 (%d)",
    "剧集 (%d)",
    "搜索结果",

    /* Podcast — login */
    "姓名",
    "输入你的姓名",
    "确认密码",
    "再次输入密码",
    "我同意服务条款",

    /* Podcast — download task */
    "等待",
    "完成",
    "失败",
    "预计",
    "状态",
    "无下载任务",
    "继续",
    "暂停",
    "删除选中的任务？\n（已下载的文件保留；\n未完成的将被移除。）",
    "%d秒",
    "%d分钟",

    /* Podcast — settings */
    "国家/地区",
    "下载音质",
    "中国",
    "美国",
    "英国",
    "日本",
    "德国",
    "法国",
    "加拿大",
    "澳大利亚",
    "低 (64kbps)\n中 (128kbps)\n高 (320kbps)",

    /* Podcast — controller toasts / errors */
    "该下载为空或已损坏",
    "姓名太短",
    "密码太短",
    "两次密码不一致",
    "请先同意服务条款",
    "内存分配失败",
    "订阅源不可用（已列入黑名单）",
    "服务器错误",

    /* Podcast — http errors */
    "HTTP 客户端初始化失败",
    "连接失败: %s",
    "内存不足 (%d 字节)",

    /* System — launcher */
    "应用",
    "未安装应用",
    "所有应用均已隐藏",
    "退出 \"%s\" 并\n返回主界面？",
    "退出",
    "应用",

    /* Podcast — player (action button) */
    "播放",

    /* Player — app / files / playback */
    "播放器",
    "文件",
    "空文件夹",
    "播放失败",
    "SD卡中没有能播放的音频",
};
