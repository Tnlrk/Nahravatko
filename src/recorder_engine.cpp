#include "recorder_engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QTimer>
#include <QPointer>
#include <QImage>

namespace {
// Pevné názvy rour (jediná instance — viz single-instance guard v main.cpp).
const QString kPipeVideo = QStringLiteral("\\\\.\\pipe\\rec_video");
const QString kPipeLoopback = QStringLiteral("\\\\.\\pipe\\rec_loopback");
const QString kPipeMic = QStringLiteral("\\\\.\\pipe\\rec_mic");
} // namespace

RecorderEngine::RecorderEngine(QObject* parent) : QObject(parent) {}

RecorderEngine::~RecorderEngine()
{
    // Synchronní úklid při zániku (např. ukončení aplikace).
    // Nejprve zastavit capture (zavře roury → EOF), pak dát ffmpegu čas dopsat soubor.
    if (m_vidCap) m_vidCap->stop();
    if (m_micCap) m_micCap->stop();
    if (m_sysCap) m_sysCap->stop();
    if (m_ffmpeg && m_ffmpeg->state() != QProcess::NotRunning) {
        if (!m_ffmpeg->waitForFinished(5000))
            m_ffmpeg->kill();
    }
}

QString RecorderEngine::ffmpegPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg.exe"));
}

int RecorderEngine::crfForQuality(Quality q)
{
    switch (q) {
        case Quality::High:   return 20;
        case Quality::Medium: return 24;
        case Quality::Low:    return 28;
    }
    return 24;
}

QString RecorderEngine::scaleExprForQuality(Quality q)
{
    // Cílem je vždy přesné plátno (1920×1080 / 1280×720):
    //  1) scale s force_original_aspect_ratio=decrease + min(): zmenší do plátna,
    //     zachová poměr stran a NEzvětšuje (menší zdroj zůstane ve své velikosti),
    //  2) pad: doplní na přesný rozměr a obsah vycentruje (černé okraje).
    auto canvas = [](int w, int h) {
        return QStringLiteral(
                   "scale='min(%1,iw)':'min(%2,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2,"
                   "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black")
            .arg(w).arg(h);
    };
    switch (q) {
        case Quality::High:   return QString();          // nativní rozlišení
        case Quality::Medium: return canvas(1920, 1080); // přesně Full HD
        case Quality::Low:    return canvas(1280, 720);  // přesně HD
    }
    return QString();
}

QStringList RecorderEngine::buildFfmpegArgs(const Config& cfg) const
{
    QStringList a;
    a << QStringLiteral("-y");

    const bool hasVideo = (cfg.mode == Mode::VideoPlusAudio);

    int inputIndex = 0;
    int videoIdx = -1;
    QList<int> audioIdx;

    // --- vstupy ---
    if (hasVideo) {
        // BGRA (WGC dodává BGRA; FFmpeg si je převede na YUV). NV12 optimalizace přijde později.
        a << QStringLiteral("-f") << QStringLiteral("rawvideo")
          << QStringLiteral("-pix_fmt") << QStringLiteral("bgra")
          << QStringLiteral("-s") << QStringLiteral("%1x%2").arg(cfg.sourceWidth).arg(cfg.sourceHeight)
          << QStringLiteral("-r") << QStringLiteral("30")
          << QStringLiteral("-i") << kPipeVideo;
        videoIdx = inputIndex++;
    }

    auto addAudioInput = [&](const AudioFormatInfo& f, const QString& pipe) {
        a << QStringLiteral("-f") << (f.isFloat ? QStringLiteral("f32le") : QStringLiteral("s16le"))
          << QStringLiteral("-ar") << QString::number(f.sampleRate)
          << QStringLiteral("-ac") << QString::number(f.channels)
          << QStringLiteral("-i") << pipe;
        audioIdx << inputIndex++;
    };
    if (cfg.useSystemAudio) addAudioInput(cfg.sysFormat, kPipeLoopback);
    if (cfg.useMicrophone)  addAudioInput(cfg.micFormat, kPipeMic);

    // --- filter_complex (video škálování + případný amix) ---
    QStringList fcParts;
    QString videoMap;
    QString audioMap;

    if (hasVideo) {
        const QString scale = scaleExprForQuality(cfg.quality);
        if (!scale.isEmpty()) {
            fcParts << QStringLiteral("[%1:v]%2[vout]").arg(videoIdx).arg(scale);
            videoMap = QStringLiteral("[vout]");
        } else {
            videoMap = QStringLiteral("%1:v").arg(videoIdx);
        }
    }

    if (audioIdx.size() >= 2) {
        QString amix;
        for (int idx : audioIdx) amix += QStringLiteral("[%1:a]").arg(idx);
        amix += QStringLiteral("amix=inputs=%1:normalize=0,aresample=async=1[aout]")
                    .arg(audioIdx.size());
        fcParts << amix;
        audioMap = QStringLiteral("[aout]");
    } else if (audioIdx.size() == 1) {
        audioMap = QStringLiteral("%1:a").arg(audioIdx.first());
    }

    if (!fcParts.isEmpty())
        a << QStringLiteral("-filter_complex") << fcParts.join(QLatin1Char(';'));

    // --- mapování ---
    if (hasVideo) a << QStringLiteral("-map") << videoMap;
    if (!audioMap.isEmpty()) a << QStringLiteral("-map") << audioMap;

    // --- kodeky ---
    if (hasVideo) {
        a << QStringLiteral("-c:v") << QStringLiteral("libx264")
          << QStringLiteral("-preset") << QStringLiteral("veryfast")
          << QStringLiteral("-crf") << QString::number(crfForQuality(cfg.quality))
          << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }
    if (!audioIdx.isEmpty()) {
        const QString abr = (cfg.quality == Quality::Low) ? QStringLiteral("128k")
                                                          : QStringLiteral("192k");
        a << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a") << abr;
    }

    // Crash-safe kontejner (dopíše fragmenty i při neočekávaném pádu).
    a << QStringLiteral("-movflags") << QStringLiteral("+frag_keyframe+empty_moov");
    a << QDir::toNativeSeparators(cfg.outputFile);
    return a;
}

bool RecorderEngine::start(const Config& cfg)
{
    if (m_recording || m_stopping) return false;
    m_cfg = cfg;

    const bool wantVideo = (cfg.mode == Mode::VideoPlusAudio);

    // 1a) Připravit VIDEO (zjistí výstupní rozměr pro -s WxH).
    if (wantVideo) {
        if (cfg.monitor == nullptr && cfg.window == nullptr) {
            emit error(QStringLiteral("Není vybrán zdroj obrazu."));
            return false;
        }
        m_vidCap = std::make_unique<VideoCapture>();
        if (cfg.captureWindow) m_vidCap->setTargetWindow(cfg.window);
        else                   m_vidCap->setTargetMonitor(cfg.monitor);
        connect(m_vidCap.get(), &VideoCapture::previewFrame,
                this, &RecorderEngine::videoPreview);
        if (!m_vidCap->prepareForRecording()) {
            emit error(QStringLiteral("Nepodařilo se otevřít zdroj obrazu."));
            teardown();
            return false;
        }
        m_cfg.sourceWidth = m_vidCap->outWidth();
        m_cfg.sourceHeight = m_vidCap->outHeight();
    }

    // 1b) Otevřít zařízení a zjistit skutečné formáty zvuku (pro správné -ar/-ac/-f).
    if (cfg.useSystemAudio) {
        m_sysCap = std::make_unique<AudioCapture>();
        m_sysCap->setSource(AudioCapture::Source::SystemLoopback);
        connect(m_sysCap.get(), &AudioCapture::level, this, &RecorderEngine::sysLevel);
        if (!m_sysCap->prepareForRecording()) {
            emit error(QStringLiteral("Nepodařilo se otevřít systémový zvuk."));
            teardown();
            return false;
        }
        m_cfg.sysFormat = m_sysCap->formatInfo();
    }
    if (cfg.useMicrophone) {
        m_micCap = std::make_unique<AudioCapture>();
        m_micCap->setSource(AudioCapture::Source::Microphone);
        m_micCap->setDeviceId(cfg.micDeviceId);
        connect(m_micCap.get(), &AudioCapture::level, this, &RecorderEngine::micLevel);
        if (!m_micCap->prepareForRecording()) {
            emit error(QStringLiteral("Nepodařilo se otevřít mikrofon."));
            teardown();
            return false;
        }
        m_cfg.micFormat = m_micCap->formatInfo();
    }

    if (!wantVideo && !cfg.useMicrophone && !cfg.useSystemAudio) {
        emit error(QStringLiteral("Není vybrán žádný zdroj."));
        teardown();
        return false;
    }

    // 2) Vytvořit roury pro aktivní zdroje. Pořadí odpovídá buildFfmpegArgs (video, sys, mic).
    auto makePipe = [](const wchar_t* name) -> HANDLE {
        return CreateNamedPipeW(name, PIPE_ACCESS_OUTBOUND,
                                PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                                1u << 20, 0, 0, nullptr);
    };
    HANDLE hVid = INVALID_HANDLE_VALUE;
    HANDLE hSys = INVALID_HANDLE_VALUE;
    HANDLE hMic = INVALID_HANDLE_VALUE;
    bool pipeOk = true;
    if (wantVideo)          { hVid = makePipe(L"\\\\.\\pipe\\rec_video");    pipeOk = pipeOk && hVid != INVALID_HANDLE_VALUE; }
    if (cfg.useSystemAudio) { hSys = makePipe(L"\\\\.\\pipe\\rec_loopback"); pipeOk = pipeOk && hSys != INVALID_HANDLE_VALUE; }
    if (cfg.useMicrophone)  { hMic = makePipe(L"\\\\.\\pipe\\rec_mic");      pipeOk = pipeOk && hMic != INVALID_HANDLE_VALUE; }
    if (!pipeOk) {
        if (hVid != INVALID_HANDLE_VALUE) CloseHandle(hVid);
        if (hSys != INVALID_HANDLE_VALUE) CloseHandle(hSys);
        if (hMic != INVALID_HANDLE_VALUE) CloseHandle(hMic);
        emit error(QStringLiteral("Nepodařilo se vytvořit rouru pro záznam."));
        teardown();
        return false;
    }

    // 3) Spustit ffmpeg. Po jeho skončení (EOF z rour) se soubor dopíše → emit stopped.
    m_ffmpeg = new QProcess(this);
    m_ffmpeg->setProcessChannelMode(QProcess::SeparateChannels);
    m_ffmpeg->setStandardErrorFile(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg_last.log")));
    connect(m_ffmpeg, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) {
                const QString out = m_cfg.outputFile;
                teardown();
                emit stopped(out);
            });
    m_ffmpeg->start(ffmpegPath(), buildFfmpegArgs(m_cfg));
    if (!m_ffmpeg->waitForStarted(5000)) {
        if (hVid != INVALID_HANDLE_VALUE) CloseHandle(hVid);
        if (hSys != INVALID_HANDLE_VALUE) CloseHandle(hSys);
        if (hMic != INVALID_HANDLE_VALUE) CloseHandle(hMic);
        emit error(QStringLiteral("Nepodařilo se spustit FFmpeg."));
        teardown();
        return false;
    }

    // 4) Spustit zápis dat: capture vlákna připojí roury (ConnectNamedPipe) a začnou psát.
    if (wantVideo)          m_vidCap->beginRecording(hVid, 30);
    if (cfg.useSystemAudio) m_sysCap->beginRecording(hSys);
    if (cfg.useMicrophone)  m_micCap->beginRecording(hMic);

    m_recording = true;
    emit started();
    return true;
}

void RecorderEngine::stop()
{
    if (!m_recording || m_stopping) return;
    m_stopping = true;

    // Zastavit capture vlákna → zavřou konce rour → ffmpeg dostane EOF a korektně
    // dopíše kontejner. Na jeho doběhnutí čeká QProcess::finished (neblokujeme UI).
    if (m_vidCap) m_vidCap->stop();
    if (m_micCap) m_micCap->stop();
    if (m_sysCap) m_sysCap->stop();

    // Pojistka: kdyby ffmpeg nedoběhl, po timeoutu ho ukončíme.
    if (m_ffmpeg) {
        QPointer<QProcess> proc = m_ffmpeg;
        QTimer::singleShot(15000, this, [proc] {
            if (proc && proc->state() != QProcess::NotRunning) proc->kill();
        });
    }
}

void RecorderEngine::teardown()
{
    m_recording = false;
    m_stopping = false;
    if (m_vidCap) { m_vidCap->stop(); m_vidCap.reset(); }
    if (m_micCap) { m_micCap->stop(); m_micCap.reset(); }
    if (m_sysCap) { m_sysCap->stop(); m_sysCap.reset(); }
    if (m_ffmpeg) {
        if (m_ffmpeg->state() != QProcess::NotRunning) {
            m_ffmpeg->kill();
            m_ffmpeg->waitForFinished(2000);
        }
        m_ffmpeg->deleteLater();
        m_ffmpeg = nullptr;
    }
}
