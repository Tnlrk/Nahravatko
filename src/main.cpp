#include <QApplication>
#include <QSharedMemory>
#include <QMessageBox>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Nahravatko"));
    QApplication::setOrganizationName(QStringLiteral("Nahravatko"));
    QApplication::setQuitOnLastWindowClosed(false); // okno se zavírá do tray, ne ukončení

    // Single instance — pevné názvy rour by se mezi instancemi pobily
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
