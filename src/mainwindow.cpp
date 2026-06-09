#include "mainwindow.h"

#include "audio_capture.h"
#include "video_capture.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QShowEvent>
#include <QWindow>
#include <QScreen>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QIcon>
#include <QPalette>
#include <QFont>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>

namespace {
const QString kAppVersion = QStringLiteral("1.0");

QString iniPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("settings.ini"));
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    m_engine = new RecorderEngine(this);
    m_micMonitor = new AudioCapture(this);
    m_sysMonitor = new AudioCapture(this);
    m_videoPreview = new VideoCapture(this);

    // Výchozí složka = systémové „Videa" uživatele (C:\Users\<jméno>\Videos),
    // univerzální na každém PC. Fallback na složku vedle aplikace.
    m_outputDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (m_outputDir.isEmpty())
        m_outputDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Záznamy"));

    buildUi();
    populateAudioDevices();
    buildVideoSources();
    loadSettings();
    onModeChanged();   // sjednotí viditelnost video prvků

    connect(m_engine, &RecorderEngine::started, this, &MainWindow::onEngineStarted);
    connect(m_engine, &RecorderEngine::stopped, this, &MainWindow::onEngineStopped);
    connect(m_engine, &RecorderEngine::error, this, &MainWindow::onEngineError);
    connect(m_engine, &RecorderEngine::videoPreview, this, &MainWindow::onVideoPreview);
    connect(m_videoPreview, &VideoCapture::previewFrame, this, &MainWindow::onVideoPreview);
    // VU metry: před nahráváním je krmí monitory, během nahrávání engine.
    connect(m_micMonitor, &AudioCapture::level, this, &MainWindow::onMicLevel);
    connect(m_sysMonitor, &AudioCapture::level, this, &MainWindow::onSysLevel);
    connect(m_engine, &RecorderEngine::micLevel, this, &MainWindow::onMicLevel);
    connect(m_engine, &RecorderEngine::sysLevel, this, &MainWindow::onSysLevel);

    // VU metry se zapínají/vypínají podle checkboxů a volby zařízení.
    connect(m_micCheck, &QCheckBox::toggled, this, &MainWindow::updateAudioMonitors);
    connect(m_sysCheck, &QCheckBox::toggled, this, &MainWindow::updateAudioMonitors);
    connect(m_micDevice, &QComboBox::currentIndexChanged, this, [this](int){ updateAudioMonitors(); });
    // Živý náhled při změně zdroje obrazu; obnova seznamu při rozbalení nabídky.
    connect(m_videoSource, &QComboBox::currentIndexChanged, this, [this](int){ onVideoSourceChanged(); });
    connect(m_videoSource, &SourceComboBox::aboutToShowPopup, this, &MainWindow::onSourcePopupAboutToShow);
    updateAudioMonitors();
    updateVideoPreview();

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(500);
    connect(m_elapsedTimer, &QTimer::timeout, this, &MainWindow::updateElapsed);

    setWindowTitle(QStringLiteral("Nahrávátko"));
    setWindowIcon(makeStateIcon(false));
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    // --- Režim ---
    auto* modeRow = new QFormLayout();
    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Video + zvuk (MP4)"));
    m_mode->addItem(QStringLiteral("Pouze zvuk (AAC)"));
    modeRow->addRow(QStringLiteral("Co nahrávat:"), m_mode);
    root->addLayout(modeRow);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &MainWindow::onModeChanged);

    // --- Video skupina (zdroj + kvalita + náhled) ---
    m_videoGroup = new QGroupBox(QStringLiteral("Obraz"), this);
    auto* vbox = new QVBoxLayout(m_videoGroup);

    auto* srcRow = new QFormLayout();
    m_videoSource = new SourceComboBox(this);
    srcRow->addRow(QStringLiteral("Zdroj:"), m_videoSource);

    m_quality = new QComboBox(this);
    m_quality->addItem(QStringLiteral("Vysoká kvalita (největší soubory)"));
    m_quality->addItem(QStringLiteral("Střední kvalita (doporučeno)"));
    m_quality->addItem(QStringLiteral("Nízká kvalita (malé soubory)"));
    m_quality->setCurrentIndex(1);
    srcRow->addRow(QStringLiteral("Kvalita:"), m_quality);
    vbox->addLayout(srcRow);

    m_preview = new QLabel(QStringLiteral("Náhled (připravuje se)"), this);
    m_preview->setFixedSize(320, 180);
    m_preview->setAlignment(Qt::AlignCenter);
    // Barvy z palety (role Base/Text) → automaticky se přizpůsobí světlému i tmavému
    // režimu. Zapuštěný rámeček vymezí plochu náhledu.
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_preview->setFrameShadow(QFrame::Sunken);
    m_preview->setAutoFillBackground(true);
    m_preview->setBackgroundRole(QPalette::Base);
    m_preview->setForegroundRole(QPalette::Text);
    vbox->addWidget(m_preview, 0, Qt::AlignHCenter);

    root->addWidget(m_videoGroup);

    // --- Zvuk ---
    auto* audioGroup = new QGroupBox(QStringLiteral("Zvuk"), this);
    auto* abox = new QVBoxLayout(audioGroup);

    auto* micRow = new QHBoxLayout();
    m_micCheck = new QCheckBox(QStringLiteral("Mikrofon"), this);
    m_micDevice = new QComboBox(this);
    m_micDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    micRow->addWidget(m_micCheck);
    micRow->addWidget(m_micDevice, 1);
    abox->addLayout(micRow);

    m_vuMeter = new QProgressBar(this);
    m_vuMeter->setRange(0, 100);
    m_vuMeter->setValue(0);
    m_vuMeter->setTextVisible(false);
    m_vuMeter->setFixedHeight(12);
    abox->addWidget(m_vuMeter);

    m_sysCheck = new QCheckBox(QStringLiteral("Zvuk z počítače (systémový zvuk)"), this);
    abox->addWidget(m_sysCheck);

    m_vuMeterSys = new QProgressBar(this);
    m_vuMeterSys->setRange(0, 100);
    m_vuMeterSys->setValue(0);
    m_vuMeterSys->setTextVisible(false);
    m_vuMeterSys->setFixedHeight(12);
    abox->addWidget(m_vuMeterSys);

    root->addWidget(audioGroup);

    // --- Výstupní složka ---
    auto* outRow = new QHBoxLayout();
    m_outputDirLabel = new QLabel(this);
    m_outputDirLabel->setWordWrap(true);
    m_changeDirBtn = new QPushButton(QStringLiteral("Změnit složku…"), this);
    outRow->addWidget(new QLabel(QStringLiteral("Uloží se do:"), this));
    outRow->addWidget(m_outputDirLabel, 1);
    outRow->addWidget(m_changeDirBtn);
    root->addLayout(outRow);
    connect(m_changeDirBtn, &QPushButton::clicked, this, &MainWindow::onChangeOutputDir);

    // --- Vždy nahoře + nápověda ---
    auto* bottomRow = new QHBoxLayout();
    m_alwaysOnTop = new QCheckBox(QStringLiteral("Vždy nahoře (okno nad ostatními)"), this);
    auto* helpBtn = new QPushButton(QStringLiteral("?"), this);
    helpBtn->setFixedWidth(28);
    helpBtn->setToolTip(QStringLiteral("Nápověda a informace o aplikaci"));
    bottomRow->addWidget(m_alwaysOnTop);
    bottomRow->addStretch();
    bottomRow->addWidget(helpBtn);
    root->addLayout(bottomRow);
    connect(m_alwaysOnTop, &QCheckBox::toggled, this, &MainWindow::setAlwaysOnTop);
    connect(helpBtn, &QPushButton::clicked, this, &MainWindow::showAbout);

    // --- Nahrávání ---
    auto* recRow = new QHBoxLayout();
    m_recordBtn = new QPushButton(QStringLiteral("● Nahrávat"), this);
    m_recordBtn->setMinimumHeight(44);
    {   // výraznější hlavní akce (čitelnost / přístupnost)
        QFont rf = m_recordBtn->font();
        rf.setPointSizeF(rf.pointSizeF() + 2.0);
        rf.setBold(true);
        m_recordBtn->setFont(rf);
    }
    m_timeLabel = new QLabel(QStringLiteral("00:00"), this);
    m_timeLabel->setStyleSheet(QStringLiteral("font-size:18px; font-weight:bold;"));
    recRow->addWidget(m_recordBtn, 1);
    recRow->addWidget(m_timeLabel);
    root->addLayout(recRow);
    connect(m_recordBtn, &QPushButton::clicked, this, &MainWindow::onRecordClicked);

    // Comboboxy nesmí roztahovat okno podle nejdelší položky (např. dlouhé názvy
    // oken). Omezíme zobrazenou délku; v rozbalení i tooltipu zůstává text celý.
    for (QComboBox* cb : { static_cast<QComboBox*>(m_videoSource), m_mode, m_quality, m_micDevice }) {
        cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cb->setMinimumContentsLength(26);
    }

    setCentralWidget(central);

    // --- Tray ---
    m_trayMenu = new QMenu(this);
    m_actStop = m_trayMenu->addAction(QStringLiteral("Zastavit nahrávání"));
    m_actStop->setEnabled(false);
    m_actOpen = m_trayMenu->addAction(QStringLiteral("Otevřít aplikaci"));
    m_trayMenu->addAction(QStringLiteral("O aplikaci"), this, &MainWindow::showAbout);
    m_trayMenu->addSeparator();
    m_actQuit = m_trayMenu->addAction(QStringLiteral("Ukončit"));
    connect(m_actStop, &QAction::triggered, this, [this]{ if (m_recording) onRecordClicked(); });
    connect(m_actOpen, &QAction::triggered, this, [this]{ showNormal(); raise(); activateWindow(); });
    connect(m_actQuit, &QAction::triggered, this, [this]{ m_forceQuit = true; qApp->quit(); });

    m_tray = new QSystemTrayIcon(makeStateIcon(false), this);
    m_tray->setToolTip(QStringLiteral("Nahrávátko"));
    m_tray->setContextMenu(m_trayMenu);
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

void MainWindow::populateAudioDevices()
{
    m_micDevices = win32util::enumAudioDevices(true);
    m_micDevice->clear();
    for (const auto& d : m_micDevices) {
        m_micDevice->addItem(QString::fromStdWString(d.name),
                             QString::fromStdWString(d.id));
    }
    if (m_micDevices.empty()) {
        m_micCheck->setEnabled(false);
        m_micDevice->addItem(QStringLiteral("(nenalezen žádný mikrofon)"));
    }
}

void MainWindow::buildVideoSources()
{
    // Zapamatovat aktuální výběr podle handle (přežije přečíslování).
    int prevKind = -1;
    qulonglong prevHandle = 0;
    if (m_videoSource->currentIndex() >= 0) {
        prevKind = m_videoSource->currentData(Qt::UserRole).toInt();
        prevHandle = m_videoSource->currentData(Qt::UserRole + 1).toULongLong();
    }

    m_monitors = win32util::enumMonitors();
    m_windows = win32util::enumWindows();

    QSignalBlocker block(m_videoSource);   // neměnit náhled při přestavbě
    m_videoSource->clear();

    auto addItem = [this](const QString& text, int kind, qulonglong handle) {
        m_videoSource->addItem(text);
        const int row = m_videoSource->count() - 1;
        m_videoSource->setItemData(row, kind, Qt::UserRole);
        m_videoSource->setItemData(row, handle, Qt::UserRole + 1);
    };

    if (m_monitors.size() <= 1) {
        const qulonglong h = m_monitors.empty() ? 0
            : reinterpret_cast<qulonglong>(m_monitors[0].handle);
        addItem(QStringLiteral("Celá obrazovka"), KindMonitor, h);
    } else {
        for (const auto& m : m_monitors) {
            addItem(QStringLiteral("Celá obrazovka — %1").arg(QString::fromStdWString(m.name)),
                    KindMonitor, reinterpret_cast<qulonglong>(m.handle));
        }
    }

    if (!m_windows.empty())
        m_videoSource->insertSeparator(m_videoSource->count());
    for (const auto& w : m_windows) {
        const QString app = QString::fromStdWString(w.app);
        const QString title = QString::fromStdWString(w.title);
        const QString label = app.isEmpty() ? title : QStringLiteral("%1 — %2").arg(app, title);
        addItem(QStringLiteral("Okno: %1").arg(label), KindWindow,
                reinterpret_cast<qulonglong>(w.handle));
    }

    // Obnovit původní výběr (stejný handle), jinak první položka.
    int restore = 0;
    for (int i = 0; i < m_videoSource->count(); ++i) {
        if (m_videoSource->itemData(i, Qt::UserRole).toInt() == prevKind
            && m_videoSource->itemData(i, Qt::UserRole + 1).toULongLong() == prevHandle
            && prevHandle != 0) {
            restore = i;
            break;
        }
    }
    m_videoSource->setCurrentIndex(restore);
}

void MainWindow::onSourcePopupAboutToShow()
{
    if (m_recording) return;
    const qulonglong before = m_videoSource->currentData(Qt::UserRole + 1).toULongLong();
    buildVideoSources();
    const qulonglong after = m_videoSource->currentData(Qt::UserRole + 1).toULongLong();
    if (before != after) updateVideoPreview();   // vybraný zdroj zmizel → přepnout náhled
}

void MainWindow::onModeChanged()
{
    const bool audioOnly = (m_mode->currentIndex() == 1);
    m_videoGroup->setVisible(!audioOnly);
    updateVideoPreview();
    // Odloženě (stejně jako u změny monitoru) — až se layout/DPI usadí,
    // jinak by okno po přepnutí režimu na jiném monitoru zůstalo v původní velikosti.
    QTimer::singleShot(0, this, [this] { fitToContent(); });
}

void MainWindow::onVideoSourceChanged()
{
    updateVideoPreview();
}

void MainWindow::updateAudioMonitors()
{
    // VU metry běží mimo nahrávání; během záznamu je krmí engine (přes signály).
    m_micMonitor->stop();
    m_sysMonitor->stop();
    m_vuMeter->setValue(0);
    m_vuMeterSys->setValue(0);
    if (m_recording) return;

    if (m_micCheck->isChecked() && m_micCheck->isEnabled()) {
        m_micMonitor->setSource(AudioCapture::Source::Microphone);
        m_micMonitor->setDeviceId(m_micDevice->currentData().toString().toStdWString());
        m_micMonitor->startMonitor();
    }
    if (m_sysCheck->isChecked()) {
        m_sysMonitor->setSource(AudioCapture::Source::SystemLoopback);
        m_sysMonitor->startMonitor();
    }
}

void MainWindow::updateVideoPreview()
{
    // Živý náhled běží mimo nahrávání; během záznamu posílá náhled engine.
    m_videoPreview->stop();
    if (m_recording) return;

    const bool audioOnly = (m_mode->currentIndex() == 1);
    if (audioOnly || m_videoSource->count() == 0) {
        m_preview->setText(QStringLiteral("Náhled (připravuje se)"));
        return;
    }

    const int kind = m_videoSource->currentData(Qt::UserRole).toInt();
    const qulonglong handle = m_videoSource->currentData(Qt::UserRole + 1).toULongLong();
    if (handle == 0) { m_preview->setText(QStringLiteral("Náhled (připravuje se)")); return; }
    if (kind == KindMonitor) {
        m_videoPreview->setTargetMonitor(reinterpret_cast<HMONITOR>(handle));
        m_videoPreview->startPreview();
    } else if (kind == KindWindow) {
        m_videoPreview->setTargetWindow(reinterpret_cast<HWND>(handle));
        m_videoPreview->startPreview();
    }
}

void MainWindow::showAbout()
{
    const QString html = QStringLiteral(
        "<h3 style='margin-bottom:2px;'>Nahrávátko</h3>"
        "<span style='color:gray;'>verze %1</span>"
        "<p>Jednoduché nahrávání obrazovky a zvuku.</p>"
        "<b>Jak na to:</b>"
        "<ol style='margin-top:4px;'>"
        "<li>Nahoře zvolte <b>Co nahrávat</b> (Video + zvuk, nebo Pouze zvuk).</li>"
        "<li>Vyberte <b>Zdroj</b> obrazu (celá obrazovka nebo konkrétní okno) a <b>Kvalitu</b>.</li>"
        "<li>Zaškrtněte zvuk: <b>Mikrofon</b> a/nebo <b>Zvuk z počítače</b>.</li>"
        "<li>Klikněte na <b>Nahrávat</b>, na konci na <b>Zastavit</b>.</li>"
        "<li>Hotový soubor se uloží do zvolené složky a ta se rovnou otevře.</li>"
        "</ol>"
        "<b>Tipy:</b>"
        "<ul style='margin-top:4px;'>"
        "<li>Náhled ukazuje, co se bude nahrávat.</li>"
        "<li>Ukazatele vedle zvuku potvrzují, že je signál slyšet.</li>"
        "<li><b>Vždy nahoře</b> udrží okno nad ostatními; křížkem se aplikace schová do lišty.</li>"
        "</ul>"
        "<hr>"
        "<p style='color:gray;'><i>S pomocí AI vytvořil Antonín Lerek v roce 2026</i><br>"
        "<a href='mailto:tnlrk@tnlrk.cz'>tnlrk@tnlrk.cz</a></p>")
        .arg(kAppVersion);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("O aplikaci Nahrávátko"));
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.setIconPixmap(makeStateIcon(false).pixmap(48, 48));
    box.exec();
}

void MainWindow::fitToContent()
{
    // Odemknout pevnou velikost, přepočítat layout a zafixovat na velikost obsahu.
    // Řeší i situaci po přechodu na monitor s jiným DPI, kdy rám okna nezmenší.
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    if (centralWidget() && centralWidget()->layout()) {
        centralWidget()->layout()->invalidate();
        centralWidget()->layout()->activate();   // vynutit okamžitý přepočet
    }
    adjustSize();
    setFixedSize(size());
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // Při prvním zobrazení připojit reakci na změnu obrazovky (DPI) a dorovnat velikost.
    if (!m_screenHooked && windowHandle()) {
        m_screenHooked = true;
        connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen*) {
            QTimer::singleShot(0, this, [this] { fitToContent(); });
        });
    }
    QTimer::singleShot(0, this, [this] { fitToContent(); });
}

void MainWindow::setAlwaysOnTop(bool on)
{
    // Nativní SetWindowPos místo Qt setWindowFlags() — neznovuvytváří okno,
    // takže nebliká ani nemizí do tray.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::onChangeOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Vyberte složku pro záznamy"), m_outputDir);
    if (!dir.isEmpty()) {
        m_outputDir = dir;
        m_outputDirLabel->setText(QDir::toNativeSeparators(m_outputDir));
    }
}

QString MainWindow::makeOutputFile(bool audioOnly) const
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString ext = audioOnly ? QStringLiteral("m4a") : QStringLiteral("mp4");
    return QDir(m_outputDir).filePath(QStringLiteral("zaznam_%1.%2").arg(stamp, ext));
}

RecorderEngine::Config MainWindow::makeConfig() const
{
    RecorderEngine::Config cfg;
    const bool audioOnly = (m_mode->currentIndex() == 1);
    cfg.mode = audioOnly ? RecorderEngine::Mode::AudioOnly
                         : RecorderEngine::Mode::VideoPlusAudio;

    switch (m_quality->currentIndex()) {
        case 0: cfg.quality = RecorderEngine::Quality::High; break;
        case 2: cfg.quality = RecorderEngine::Quality::Low; break;
        default: cfg.quality = RecorderEngine::Quality::Medium; break;
    }

    if (!audioOnly && m_videoSource->count() > 0) {
        const int kind = m_videoSource->currentData(Qt::UserRole).toInt();
        const qulonglong handle = m_videoSource->currentData(Qt::UserRole + 1).toULongLong();
        // Skutečný rozměr (sourceWidth/Height) doplní engine z video capture.
        if (kind == KindMonitor && handle != 0) {
            cfg.captureWindow = false;
            cfg.monitor = reinterpret_cast<HMONITOR>(handle);
        } else if (kind == KindWindow && handle != 0) {
            cfg.captureWindow = true;
            cfg.window = reinterpret_cast<HWND>(handle);
        }
    }

    cfg.useMicrophone = m_micCheck->isChecked() && m_micCheck->isEnabled();
    cfg.useSystemAudio = m_sysCheck->isChecked();
    if (cfg.useMicrophone) {
        cfg.micDeviceId = m_micDevice->currentData().toString().toStdWString();
        cfg.micFormat = m_micMonitor->formatInfo();
    }

    cfg.outputFile = makeOutputFile(audioOnly);
    return cfg;
}

void MainWindow::onRecordClicked()
{
    if (m_recording) {
        // Zastavení — graceful shutdown běží na pozadí, UI ukáže „Ukládám…".
        m_recordBtn->setEnabled(false);
        m_recordBtn->setText(QStringLiteral("Ukládám…"));
        m_engine->stop();
        return;
    }

    RecorderEngine::Config cfg = makeConfig();

    const bool audioOnly = (cfg.mode == RecorderEngine::Mode::AudioOnly);
    if (audioOnly && !cfg.useMicrophone && !cfg.useSystemAudio) {
        QMessageBox::warning(this, QStringLiteral("Nahrávátko"),
            QStringLiteral("Vyberte alespoň jeden zvukový zdroj (mikrofon nebo zvuk z počítače)."));
        return;
    }
    if (!audioOnly && cfg.monitor == nullptr && cfg.window == nullptr) {
        QMessageBox::warning(this, QStringLiteral("Nahrávátko"),
            QStringLiteral("Vyberte zdroj obrazu."));
        return;
    }
    // Video bez zvuku: upozornit (zvuk už nepůjde přidat za běhu).
    if (!audioOnly && !cfg.useMicrophone && !cfg.useSystemAudio) {
        const auto r = QMessageBox::question(this, QStringLiteral("Nahrávátko"),
            QStringLiteral("Nemáte vybraný žádný zvuk (mikrofon ani zvuk z počítače).\n"
                           "Zvuk nelze přidat během nahrávání. Nahrávat jen obraz bez zvuku?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) return;
    }

    QDir().mkpath(m_outputDir);

    // Uvolníme monitory a náhled — zařízení/zdroj převezme nahrávací vlákno.
    m_micMonitor->stop();
    m_sysMonitor->stop();
    m_videoPreview->stop();
    if (!m_engine->start(cfg)) {
        // Chyba se zobrazí přes signál error; obnovíme monitory a náhled.
        updateAudioMonitors();
        updateVideoPreview();
    }
}

void MainWindow::setRecordingState(bool recording)
{
    m_recording = recording;
    m_recordBtn->setEnabled(true);
    m_recordBtn->setText(recording ? QStringLiteral("■ Zastavit")
                                    : QStringLiteral("● Nahrávat"));
    m_recordBtn->setStyleSheet(recording
        ? QStringLiteral("background:#c0392b; color:white; font-weight:bold;")
        : QString());
    m_actStop->setEnabled(recording);
    m_tray->setIcon(makeStateIcon(recording));
    setWindowIcon(makeStateIcon(recording));   // i ikona v hlavním panelu zčervená
    m_mode->setEnabled(!recording);
    m_videoGroup->setEnabled(!recording);
    // Zvukové volby nejdou měnit za běhu (zdroje se fixují při startu) → zamknout.
    m_micCheck->setEnabled(!recording && !m_micDevices.empty());
    m_micDevice->setEnabled(!recording);
    m_sysCheck->setEnabled(!recording);

    if (recording) {
        m_elapsed.start();
        m_elapsedTimer->start();
    } else {
        m_elapsedTimer->stop();
        m_timeLabel->setText(QStringLiteral("00:00"));
        m_tray->setToolTip(QStringLiteral("Nahrávátko"));
    }
}

void MainWindow::onEngineStarted() { setRecordingState(true); }

void MainWindow::onEngineStopped(const QString& outputPath)
{
    setRecordingState(false);
    updateAudioMonitors();   // obnovit VU metry po skončení záznamu
    updateVideoPreview();    // obnovit živý náhled (nebo placeholder)

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Nahrávátko"));
    box.setText(QStringLiteral("Záznam byl uložen:\n%1").arg(QDir::toNativeSeparators(outputPath)));
    auto* openBtn = box.addButton(QStringLiteral("Otevřít složku"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Zavřít"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == openBtn) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(outputPath)});
    }
}

void MainWindow::onEngineError(const QString& message)
{
    if (m_recording) setRecordingState(false);
    m_recordBtn->setEnabled(true);
    m_recordBtn->setText(QStringLiteral("● Nahrávat"));
    QMessageBox::warning(this, QStringLiteral("Nahrávátko"), message);
    updateAudioMonitors();
    updateVideoPreview();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        showNormal();
        raise();
        activateWindow();
    }
}

void MainWindow::onMicLevel(float rms)
{
    m_vuMeter->setValue(static_cast<int>(qBound(0.0f, rms, 1.0f) * 100.0f));
}

void MainWindow::onSysLevel(float rms)
{
    m_vuMeterSys->setValue(static_cast<int>(qBound(0.0f, rms, 1.0f) * 100.0f));
}

void MainWindow::onVideoPreview(const QImage& frame)
{
    if (frame.isNull()) return;
    m_preview->setPixmap(QPixmap::fromImage(frame).scaled(
        m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void MainWindow::updateElapsed()
{
    const qint64 secs = m_elapsed.elapsed() / 1000;
    const QString t = QStringLiteral("%1:%2")
                          .arg(secs / 60, 2, 10, QLatin1Char('0'))
                          .arg(secs % 60, 2, 10, QLatin1Char('0'));
    m_timeLabel->setText(t);
    m_tray->setToolTip(QStringLiteral("Nahrávám — %1").arg(t));
}

QIcon MainWindow::makeStateIcon(bool recording) const
{
    const QIcon brand(QStringLiteral(":/app.ico"));
    if (!recording)
        return brand;

    // Při nahrávání: značka aplikace + výrazná červená tečka v rohu (indikace záznamu).
    const int S = 64;
    QPixmap pm = brand.pixmap(S, S);
    if (pm.isNull()) { pm = QPixmap(S, S); pm.fill(Qt::transparent); }
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int d = static_cast<int>(S * 0.55);          // velká tečka (~55 %)
    const int x = S - d - 1;
    const int y = S - d - 1;
    p.setPen(QPen(Qt::white, S * 0.07));                 // bílý lem pro kontrast
    p.setBrush(QColor(0xff, 0x1f, 0x1f));                // jasně červená
    p.drawEllipse(x, y, d, d);
    p.end();
    return QIcon(pm);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_forceQuit) {
        saveSettings();
        event->accept();
        return;
    }

    if (m_recording) {
        const auto r = QMessageBox::question(this, QStringLiteral("Nahrávátko"),
            QStringLiteral("Probíhá nahrávání. Zastavit nahrávání a ukončit aplikaci?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) { event->ignore(); return; }
        m_engine->stop();           // pošle EOF; finalizaci dokončí destruktor enginu
        m_forceQuit = true;
        saveSettings();
        event->accept();
        qApp->quit();
        return;
    }

    // Nabídka při zavření křížkem.
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Nahrávátko"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("Co chcete udělat?"));
    auto* quitBtn = box.addButton(QStringLiteral("Ukončit aplikaci"), QMessageBox::AcceptRole);
    auto* trayBtn = box.addButton(QStringLiteral("Nechat běžet v liště"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("Storno"), QMessageBox::RejectRole);
    box.setDefaultButton(trayBtn);
    box.exec();
    auto* clicked = box.clickedButton();

    if (clicked == quitBtn) {
        m_forceQuit = true;
        saveSettings();
        event->accept();
        qApp->quit();
    } else if (clicked == trayBtn) {
        saveSettings();
        hide();
        event->ignore();
        m_tray->showMessage(QStringLiteral("Nahrávátko"),
            QStringLiteral("Aplikace běží na pozadí. Klikněte sem pro otevření."),
            QSystemTrayIcon::Information, 3000);
    } else {
        event->ignore();   // Storno
    }
}

void MainWindow::loadSettings()
{
    QSettings s(iniPath(), QSettings::IniFormat);
    m_mode->setCurrentIndex(s.value(QStringLiteral("mode"), 0).toInt());
    m_quality->setCurrentIndex(s.value(QStringLiteral("quality"), 1).toInt());
    // Pevné výchozí hodnoty při každém startu (nepersistované):
    // oba zvuky zaškrtnuté, „Vždy nahoře" vypnuté.
    m_micCheck->setChecked(!m_micDevices.empty());
    m_sysCheck->setChecked(true);
    m_alwaysOnTop->setChecked(false);
    m_outputDir = s.value(QStringLiteral("outputDir"), m_outputDir).toString();

    const QString micName = s.value(QStringLiteral("micDevice")).toString();
    if (!micName.isEmpty()) {
        const int i = m_micDevice->findText(micName);
        if (i >= 0) m_micDevice->setCurrentIndex(i);
    }
    m_outputDirLabel->setText(QDir::toNativeSeparators(m_outputDir));
}

void MainWindow::saveSettings()
{
    QSettings s(iniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("mode"), m_mode->currentIndex());
    s.setValue(QStringLiteral("quality"), m_quality->currentIndex());
    // mikrofon / systémový zvuk / vždy-nahoře se záměrně neukládají
    // (mají pevné výchozí hodnoty při každém startu).
    s.setValue(QStringLiteral("outputDir"), m_outputDir);
    s.setValue(QStringLiteral("micDevice"), m_micDevice->currentText());
}
