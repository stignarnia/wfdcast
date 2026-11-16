// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "castcontroller.h"
#include "settings.h"

#include <KAboutData>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KSignalHandler>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <gst/gst.h>

#include <csignal>

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("wfdcast");

    KAboutData about(u"wfdcast"_s, i18n("WFD Cast"), QStringLiteral(WFDCAST_VERSION),
                     i18n("Cast your screen and sound to a Wi-Fi Display (Miracast) TV"), KAboutLicense::MIT);
    about.setDesktopFileName(u"io.github.stignarnia.wfdcast"_s);
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(u"video-display"_s));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(u"org.kde.desktop"_s);
    }

    // Quit through the event loop on signals so ~CastController restores audio,
    // stops the helper and closes the portal session.
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
        KSignalHandler::self()->watchSignal(sig);
    }
    QObject::connect(KSignalHandler::self(), &KSignalHandler::signalReceived, &app, &QCoreApplication::quit);

    Settings settings;
    CastController controller(&settings);

    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    engine.rootContext()->setContextProperty(u"cast"_s, &controller);
    engine.rootContext()->setContextProperty(u"settings"_s, &settings);
    engine.rootContext()->setContextProperty(u"appVersion"_s, u"%1 · %2"_s.arg(QStringLiteral(WFDCAST_VERSION), QStringLiteral(WFDCAST_COMMIT)));
    engine.loadFromModule("io.github.stignarnia.wfdcast", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec(); // ~CastController tears everything down synchronously
}
