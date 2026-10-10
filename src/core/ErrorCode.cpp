// ============================================================================
//  ErrorCode.cpp — 错误码文案
//
//  需求依据：SRS V1.1 7.2（可理解的错误处理）、6.2（不支持的功能应禁用并解释原因）
//
//  文案原则：
//    1. 说清「发生了什么」，不说「错误代码是什么」就结束
//    2. 能给出下一步动作的，明确给出（如「请在设置中开启网络」）
//    3. 不把技术细节塞给用户；技术细节走 Result::detail 进日志
// ============================================================================
#include "core/ErrorCode.h"

#include <QHash>

namespace dc {
namespace erc {

namespace {

const QHash<QString, QString> &descriptions()
{
    static const QHash<QString, QString> table = {
        // 通用
        { QStringLiteral("DC-0000"), QStringLiteral("操作成功。") },
        { QStringLiteral("DC-0001"), QStringLiteral("输入内容不合法，请检查后重试。") },
        { QStringLiteral("DC-0002"), QStringLiteral("程序尚未完成初始化，请稍后重试。") },
        { QStringLiteral("DC-0003"), QStringLiteral("操作已取消。") },
        { QStringLiteral("DC-0004"), QStringLiteral("操作超时，请稍后重试。") },
        { QStringLiteral("DC-0005"), QStringLiteral("程序内部出现异常，该功能已跳过，其他功能不受影响。") },
        { QStringLiteral("DC-0006"), QStringLiteral("当前环境不支持该功能。") },

        // 配置
        { QStringLiteral("DC-1001"), QStringLiteral("未找到配置文件，将使用默认配置重新创建。") },
        { QStringLiteral("DC-1002"), QStringLiteral("配置文件格式有误，原文件已备份，已恢复为默认配置。") },
        { QStringLiteral("DC-1003"), QStringLiteral("配置项取值不合法。") },
        { QStringLiteral("DC-1004"), QStringLiteral("配置文件版本高于当前程序支持的版本，已按只读方式加载。") },
        { QStringLiteral("DC-1005"), QStringLiteral("配置迁移失败，已保留原文件并回退到默认配置。") },
        { QStringLiteral("DC-1006"), QStringLiteral("配置写入失败，请检查文件权限或磁盘空间。") },
        { QStringLiteral("DC-1007"), QStringLiteral("已为旧配置创建备份。") },

        // 命令引擎
        { QStringLiteral("DC-2001"), QStringLiteral("没有识别出可执行的命令。可以说「帮助」查看支持的命令。") },
        { QStringLiteral("DC-2002"), QStringLiteral("这句话可以理解为多个命令，请选择你要执行的操作。") },
        { QStringLiteral("DC-2003"), QStringLiteral("命令识别把握不大，请确认后再执行。") },
        { QStringLiteral("DC-2004"), QStringLiteral("命令规则存在冲突，请检查配置。") },
        { QStringLiteral("DC-2005"), QStringLiteral("输入内容无法处理（可能为空、过长或含控制字符）。") },
        { QStringLiteral("DC-2006"), QStringLiteral("命令缺少必要参数。") },
        { QStringLiteral("DC-2007"), QStringLiteral("命令参数不符合要求。") },

        // 权限
        { QStringLiteral("DC-3001"), QStringLiteral("你取消了这次操作，未执行任何动作。") },
        { QStringLiteral("DC-3002"), QStringLiteral("该操作需要确认后才能执行。") },
        { QStringLiteral("DC-3003"), QStringLiteral("该操作风险较高，需要你明确授权。") },
        { QStringLiteral("DC-3004"), QStringLiteral("出于安全考虑，本程序不提供此类操作。") },
        { QStringLiteral("DC-3005"), QStringLiteral("你此前已拒绝过这一操作，本次不再执行。"
                                                   "如需改变，请在设置中清除拒绝记录。") },
        { QStringLiteral("DC-3006"), QStringLiteral("系统未能提升权限，操作未执行。") },

        // 路径与文件
        { QStringLiteral("DC-4001"), QStringLiteral("路径为空。") },
        { QStringLiteral("DC-4002"), QStringLiteral("路径中包含非法的上级跳转（..），已拒绝访问。") },
        { QStringLiteral("DC-4003"), QStringLiteral("该路径不在允许访问的范围内。可在设置中添加允许目录。") },
        { QStringLiteral("DC-4004"), QStringLiteral("路径不存在，请确认文件是否已被移动或删除。") },
        { QStringLiteral("DC-4005"), QStringLiteral("没有权限访问该路径。") },
        { QStringLiteral("DC-4006"), QStringLiteral("该位置属于敏感系统位置，需要额外确认。") },
        { QStringLiteral("DC-4007"), QStringLiteral("目标已存在，覆盖需要单独确认。") },
        { QStringLiteral("DC-4008"), QStringLiteral("这是一个目录，请使用打开目录的方式访问。") },
        { QStringLiteral("DC-4009"), QStringLiteral("路径中包含非法字符。") },

        // 应用启动
        { QStringLiteral("DC-5001"), QStringLiteral("没有找到这个应用。可在设置中配置应用路径和别名。") },
        { QStringLiteral("DC-5002"), QStringLiteral("已配置的应用路径不存在，请在设置中重新选择。") },
        { QStringLiteral("DC-5003"), QStringLiteral("应用启动失败，请检查程序是否完整。") },
        { QStringLiteral("DC-5004"), QStringLiteral("启动参数未通过校验，已拒绝执行。") },

        // 网络
        { QStringLiteral("DC-6001"), QStringLiteral("当前为完全离线模式，未发起任何网络请求。"
                                                   "可在设置中调整网络权限。") },
        { QStringLiteral("DC-6002"), QStringLiteral("该网络服务尚未配置或未启用。可在设置中启用。") },
        { QStringLiteral("DC-6003"), QStringLiteral("你拒绝了本次联网请求，未发起任何网络请求。") },
        { QStringLiteral("DC-6004"), QStringLiteral("该服务已被关闭，未发起请求。") },
        { QStringLiteral("DC-6005"), QStringLiteral("安全连接（TLS）建立失败，为保护你的数据未继续请求。"
                                                   "在较旧系统上可能是系统证书过旧导致。") },
        { QStringLiteral("DC-6006"), QStringLiteral("服务器证书校验未通过，出于安全考虑已中止请求。") },
        { QStringLiteral("DC-6007"), QStringLiteral("网络请求超时，请检查网络连接后重试。") },
        { QStringLiteral("DC-6008"), QStringLiteral("服务返回内容过大，已中止读取以保护内存。") },
        { QStringLiteral("DC-6009"), QStringLiteral("目标地址不在允许访问的域名列表中。") },
        { QStringLiteral("DC-6010"), QStringLiteral("仅允许使用 HTTPS 安全连接，已拒绝不安全的请求。") },
        { QStringLiteral("DC-6011"), QStringLiteral("网络请求失败，请检查网络连接。") },

        // 语音
        { QStringLiteral("DC-7001"), QStringLiteral("当前系统不支持语音功能，语音入口已禁用。"
                                                   "桌宠和本地命令功能不受影响。") },
        { QStringLiteral("DC-7002"), QStringLiteral("未找到可用的语音引擎，语音功能已禁用。") },
        { QStringLiteral("DC-7003"), QStringLiteral("音频设备不可用，请检查麦克风或扬声器连接。") },
        { QStringLiteral("DC-7004"), QStringLiteral("没有听清，请重说一次，或改用文字输入。") },
        { QStringLiteral("DC-7005"), QStringLiteral("识别结果把握不大，请确认后再执行。") },
        { QStringLiteral("DC-7006"), QStringLiteral("语音播报失败，已改为文字气泡显示。") },

        // 录音
        { QStringLiteral("DC-8001"), QStringLiteral("录音功能未启用。可在设置中开启。") },
        { QStringLiteral("DC-8002"), QStringLiteral("当前没有正在进行的录音。") },
        { QStringLiteral("DC-8003"), QStringLiteral("已达到设定的最长录音时长，录音已自动停止。") },
        { QStringLiteral("DC-8004"), QStringLiteral("已达到设定的录音空间上限，录音已自动停止。") },
        { QStringLiteral("DC-8005"), QStringLiteral("录音设备不可用，请检查麦克风连接。") },
        { QStringLiteral("DC-8006"), QStringLiteral("录音文件写入失败，请检查保存目录的磁盘空间与权限。") },

        // 历史记录
        { QStringLiteral("DC-9001"), QStringLiteral("该类历史记录已关闭，本次未保存。"
                                                   "可在设置的历史记录中重新开启。") },
        { QStringLiteral("DC-9002"), QStringLiteral("无法打开历史数据库，历史功能已禁用，"
                                                   "其余功能不受影响。") },
        { QStringLiteral("DC-9003"), QStringLiteral("历史记录检索失败。") },
        { QStringLiteral("DC-9004"), QStringLiteral("历史记录删除失败。") },
        { QStringLiteral("DC-9005"), QStringLiteral("历史记录导出失败，请检查导出位置的写入权限。") },
        { QStringLiteral("DC-9006"), QStringLiteral("历史数据库已损坏，已备份原文件并重建了新数据库。") },

        // 平台
        { QStringLiteral("DC-A001"), QStringLiteral("当前操作系统不受支持，部分功能已禁用。") },
        { QStringLiteral("DC-A002"), QStringLiteral("当前系统缺少该功能所需的能力，入口已禁用。") },
        { QStringLiteral("DC-A003"), QStringLiteral("该快捷键已被其他程序占用，请更换一个组合。") },
        { QStringLiteral("DC-A004"), QStringLiteral("快捷键注册失败，可能是系统限制或组合被占用。") },
        { QStringLiteral("DC-A005"), QStringLiteral("开机启动设置失败，请检查启动项目录权限。") },
    };
    return table;
}

} // namespace

QString describe(const QString &errorCode)
{
    return descriptions().value(errorCode);
}

QString categoryOf(const QString &errorCode)
{
    // 错误码形态固定为 DC-XXXX，前缀数字段决定分类
    if (errorCode.size() < 7 || !errorCode.startsWith(QStringLiteral("DC-")))
        return QStringLiteral("unknown");

    const QChar second = errorCode.at(3);
    if (!second.isDigit() && !second.isLetter())
        return QStringLiteral("unknown");

    // 字母段（DC-Axxx）单独归为 platform
    if (second.isLetter())
        return QStringLiteral("platform");

    switch (second.digitValue()) {
    case 0: return QStringLiteral("general");
    case 1: return QStringLiteral("config");
    case 2: return QStringLiteral("command");
    case 3: return QStringLiteral("permission");
    case 4: return QStringLiteral("path");
    case 5: return QStringLiteral("application");
    case 6: return QStringLiteral("network");
    case 7: return QStringLiteral("speech");
    case 8: return QStringLiteral("recording");
    case 9: return QStringLiteral("history");
    default: break;
    }
    return QStringLiteral("unknown");
}

} // namespace erc
} // namespace dc
