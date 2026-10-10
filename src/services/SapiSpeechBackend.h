#pragma once

// ============================================================================
//  SapiSpeechBackend.h —— Windows SAPI 语音后端（SRS 4.10 / 8.3）
//
//  需求依据（SRS 8.3 技术路线）：
//    "语音能力优先使用操作系统自带能力，避免强制引入大型模型。"
//
//  为什么选 SAPI：
//    1. 从 Windows XP 到 Windows 11 全都自带，零额外体积；
//    2. 不需要任何模型文件、不需要独立安装；
//    3. 通过 COM 调用，Qt 本身不影响它；
//    4. 朗读（合成）质量在中文环境下可用，识别则取决于系统是否装了识别引擎。
//
//  诚实的能力说明（这一点在代码里也要如实反映）：
//    - 合成（SAPI.SpVoice）：几乎所有 Windows 都可用。
//    - 识别（SAPI.SpSharedRecognizer）：
//        * Windows 7/8 通常自带识别引擎，但需要用户手动开启语音识别；
//        * Windows 10/11 的 SAPI 识别引擎往往不可用（微软把识别搬到了
//          Windows Speech Recognition / Azure，SAPI 那套留在系统里但常不工作）；
//        * 因此本后端在探测到识别不可用时会如实返回 SynthesisOnly，
//          而不是假装能用。
//
//  非 Windows 平台：本文件的条件编译会产出一个"不可用"的实现，
//  使上层逻辑保持一致（永远拿得到一个后端对象，但它会说自己不可用）。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>

#include "services/SpeechBackend.h"

namespace dc {

class SapiSpeechBackend : public SpeechBackend
{
    Q_OBJECT

public:
    explicit SapiSpeechBackend(QObject *parent = nullptr);
    ~SapiSpeechBackend() override;

    // 初始化 COM 与语音对象。返回是否至少有一项能力可用。
    // 必须在使用前调用（CoreRuntime 负责）。
    bool initialize();

    QString id() const override;
    QString displayName() const override;
    SpeechBackendKind kind() const override;
    QString unavailableReason() const override;

    QList<VoiceInfo> availableVoices() const override;
    QStringList supportedLanguages() const override;

    void recognize(const RecognitionRequest &request) override;
    void cancelRecognition() override;

    void synthesize(const SynthesisRequest &request) override;
    void cancelSynthesis() override;
    void stopPlayback() override;
    bool isSpeaking() const override;

    // 当前使用的音色 id
    QString currentVoiceId() const;
    bool setVoiceById(const QString &voiceId);

    // 语速与音量（-10..10 / 0..100）
    int rate() const;
    void setRate(int rate);
    int volume() const;
    void setVolume(int volume);

    // 诊断信息：把探测到的能力细节拼成一段文字，用于"为什么不可用"提示
    QString diagnosticReport() const;

private:
    struct Impl;                 // 隐藏所有 Windows 头文件，避免污染上层
    Impl *d = nullptr;

    SpeechBackendKind m_kind = SpeechBackendKind::None;
    QString m_unavailableReason;
    bool m_speaking = false;
};

} // namespace dc
