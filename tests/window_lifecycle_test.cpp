#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

class WindowLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void closeMainExitsAllWindows_data() {
        QTest::addColumn<QString>("scenario");
        for (const char *name : {"main", "settings", "popout", "hidden-popout", "inline", "hidden-inline", "all", "close-popout", "close-settings", "close-inline"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void closeMainExitsAllWindows() {
        QFETCH(QString, scenario);
        QTemporaryDir files; QVERIFY(files.isValid());
        const auto path = files.filePath("generated-window-report.json");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen"); environment.insert("QT_QUICK_BACKEND", "software");
        environment.remove("PULSE_SERVER"); environment.remove("PIPEWIRE_REMOTE");
        QProcess child; child.setProcessEnvironment(environment);
        child.start(QStringLiteral(RELAY_TEST_APPLICATION_PATH), {"--demo", "--quit-after", "10000", "--window-lifecycle", scenario, "--window-lifecycle-report", path});
        QVERIFY(child.waitForStarted(2500));
        const bool exited = child.waitForFinished(3500);
        if (!exited) { child.kill(); child.waitForFinished(1500); }
        QFile report(path); QVERIFY(report.open(QIODevice::ReadOnly));
        const auto bytes = report.readAll();
        const auto state = QJsonDocument::fromJson(bytes).object();
        QVERIFY2(state.value("prepared").toBool(), bytes.constData());
        if (scenario != "main") QVERIFY2(state.value("residentAux").toInt() > 0, bytes.constData());
        if (scenario.startsWith("close-")) {
            QVERIFY2(state.value("mainVisibleAfterAuxClose").toBool(), bytes.constData());
            if (scenario == "close-popout") QVERIFY(!state.value("streamPoppedOutAfterAuxClose").toBool());
            if (scenario == "close-settings") QVERIFY(!state.value("settingsVisibleAfterAuxClose").toBool());
            if (scenario == "close-inline") QVERIFY(!state.value("inlineVisibleAfterAuxClose").toBool());
        }
        const auto output = child.readAllStandardError() + child.readAllStandardOutput();
        QVERIFY2(exited, qPrintable(QString::fromUtf8(bytes) + '\n' + QString::fromUtf8(output)));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit); QCOMPARE(child.exitCode(), 0);
        QVERIFY2(state.value("mainCloseRequested").toBool(), bytes.constData());
        QVERIFY2(state.value("mainCloseAccepted").toBool(), bytes.constData());
        // Only capability notices from the isolated offscreen environment are
        // expected. Keep all QML errors and unexpected runtime output fatal.
        QByteArray unexpected;
        for (const auto &line : output.split('\n')) {
            if (line.isEmpty() || line == "This plugin does not support raise()" ||
                line == "Failed to connect to pipewire instance \"Host is down\"" ||
                line == "PulseAudioService: pa_context_connect() failed" ||
                line.startsWith("Call to org.freedesktop.portal.Settings.ReadAll failed QDBusError(") ||
                line.startsWith("Call for getting org.freedesktop.portal.FileChooser version failed QDBusError("))
                continue;
            unexpected += line + '\n';
        }
        QVERIFY2(unexpected.isEmpty(), unexpected.constData());
    }
};
QTEST_GUILESS_MAIN(WindowLifecycleTest)
#include "window_lifecycle_test.moc"
