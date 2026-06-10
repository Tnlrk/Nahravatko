#include <QApplication>
#include <QGuiApplication>
#include <QFont>
#include <QSharedMemory>
#include <QMessageBox>

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

    // Single instance — víc běžících kopií by matlo uživatele (tray ikony, zámky zařízení)
    static QSharedMemory singleInstanceGuard(QStringLiteral("Nahravatko_SingleInstance"));
    if (!singleInstanceGuard.create(1)) {
        QMessageBox::information(nullptr, QStringLiteral("Nahrávátko"),
                                 QStringLiteral("Aplikace už běží."));
        return 0;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
