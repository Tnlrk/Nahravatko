#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QElapsedTimer>
#include <QComboBox>

#include <vector>

#include "win32_utils.h"
#include "recorder_engine.h"

// QComboBox, který před rozbalením nabídky umožní obnovit svůj obsah
// (aby se objevila i okna otevřená až po startu Nahrávátka).
class SourceComboBox : public QComboBox
{
    Q_OBJECT
public:
    using QComboBox::QComboBox;
    void showPopup() override { emit aboutToShowPopup(); QComboBox::showPopup(); }
signals:
    void aboutToShowPopup();
};

class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;
class QMenu;
class QAction;
class QTimer;
class QShowEvent;
class QNetworkAccessManager;
class AudioCapture;
class VideoCapture;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onModeChanged();
    void onChangeOutputDir();
    void onRecordClicked();
    void onEngineStarted();
    void onEngineStopped(const QString& outputPath);
    void onEngineError(const QString& message);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onMicLevel(float rms);
    void onSysLevel(float rms);
    void onVideoPreview(const QImage& frame);
    void onVideoSourceChanged();
    void showAbout();
    void checkForUpdates();   // tichá kontrola nové verze na GitHubu
    void updateElapsed();

private:
    void buildUi();
    void populateAudioDevices();  // výčet mikrofonů (jen při startu)
    void buildVideoSources();     // (znovu)naplní seznam zdrojů obrazu, zachová výběr
    void onSourcePopupAboutToShow();
    void updateAudioMonitors();   // zapne/vypne VU metry (mikrofon + systém) mimo nahrávání
    void updateVideoPreview();    // spustí/zastaví živý náhled dle zdroje a režimu
    void setAlwaysOnTop(bool on);
    void fitToContent();   // přepočítá velikost okna na obsah (i po změně DPI/monitoru)
    void loadSettings();
    void saveSettings();
    RecorderEngine::Config makeConfig() const;
    QString makeOutputFile(bool audioOnly) const;
    QIcon makeStateIcon(bool recording) const;
    void setRecordingState(bool recording);

    // UI prvky
    SourceComboBox* m_videoSource = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_micCheck = nullptr;
    QComboBox* m_micDevice = nullptr;
    QProgressBar* m_vuMeter = nullptr;
    QCheckBox* m_sysCheck = nullptr;
    QProgressBar* m_vuMeterSys = nullptr;
    QComboBox* m_quality = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_outputDirLabel = nullptr;
    QPushButton* m_changeDirBtn = nullptr;
    QPushButton* m_recordBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QCheckBox* m_alwaysOnTop = nullptr;
    QWidget* m_videoGroup = nullptr;   // skryje se v režimu „Pouze zvuk"

    // Tray
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_actStop = nullptr;
    QAction* m_actOpen = nullptr;
    QAction* m_actQuit = nullptr;

    // Engine + capture
    RecorderEngine* m_engine = nullptr;
    AudioCapture* m_micMonitor = nullptr;
    AudioCapture* m_sysMonitor = nullptr;
    VideoCapture* m_videoPreview = nullptr;
    QNetworkAccessManager* m_net = nullptr;   // kontrola aktualizací

    // Stav
    QString m_outputDir;
    QTimer* m_elapsedTimer = nullptr;
    QElapsedTimer m_elapsed;
    bool m_recording = false;
    bool m_forceQuit = false;
    bool m_screenHooked = false;

    // Evidence zdrojů
    std::vector<win32util::MonitorInfo> m_monitors;
    std::vector<win32util::WindowInfo> m_windows;
    std::vector<win32util::AudioDevice> m_micDevices;

    // Role pro QComboBox položky video zdroje
    enum SourceKind { KindMonitor = 0, KindWindow = 1 };
};
