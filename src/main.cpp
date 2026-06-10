#include <QApplication>
#include <QGuiApplication>
#include <QFont>
#include <QSharedMemory>
#include <QMessageBox>
#include <QTranslator>
#include <QLocale>
#include <QSettings>
#include <QDir>
#include <QThread>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // Přesné (nezaokrouhlené) měřítko DPI — plynulejší přechod mezi monitory
    // s různým rozlišením (musí být před vytvořením QApplication).
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // Mírně větší základní písmo pro lepší čitelnost (přístupnost).
    {
        QFont f = app.font();
        if (f.pointSizeF() > 0) {
            f.setPointSizeF(f.pointSizeF() + 1.0);
            app.setFont(f);
        }
    }

    // Světlý/tmavý režim se řídí nastavením systému automaticky (Qt 6.5+).

    QApplication::setApplicationName(QStringLiteral("Nahravatko"));
    QApplication::setOrganizationName(QStringLiteral("Nahravatko"));
    QApplication::setQuitOnLastWindowClosed(false); // okno se zavírá do tray, ne ukončení

    // Jazyk: "auto" (podle systému) / "cs" / "en"; uložen v settings.ini (volba v „O aplikaci").
    // Čeština je zdrojový jazyk → překladač se instaluje jen pro angličtinu.
    QString locale;
    {
        const QString ini = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("settings.ini"));
        const QString lang = QSettings(ini, QSettings::IniFormat)
                                 .value(QStringLiteral("language"), QStringLiteral("auto")).toString();
        locale = (lang == QLatin1String("auto")) ? QLocale::system().name() : lang;
    }
    static QTranslator translator;
    if (locale.startsWith(QLatin1String("en"), Qt::CaseInsensitive)
        && translator.load(QStringLiteral(":/i18n/nahravatko_en.qm"))) {
        app.installTranslator(&translator);
    }

    // Single instance — víc běžících kopií by matlo uživatele (tray ikony, zámky zařízení)
    static QSharedMemory singleInstanceGuard(QStringLiteral("Nahravatko_SingleInstance"));
    bool acquired = singleInstanceGuard.create(1);
    // Při restartu (tlačítko v „O aplikaci") nová instance startuje souběžně se starou —
    // chvíli počkat, až ta uvolní zámek, ať se restart nezasekne na „Aplikace už běží".
    if (!acquired && app.arguments().contains(QStringLiteral("--restarted"))) {
        for (int i = 0; i < 30 && !acquired; ++i) {   // ~3 s
            QThread::msleep(100);
            acquired = singleInstanceGuard.create(1);
        }
    }
    if (!acquired) {
        QMessageBox::information(nullptr, appDisplayName(),
                                 QObject::tr("Aplikace už běží."));
        return 0;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
