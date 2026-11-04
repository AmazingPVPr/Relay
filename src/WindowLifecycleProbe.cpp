#include "WindowLifecycleProbe.h"
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QPointer>
#include <memory>

void runWindowLifecycleProbe(QQmlApplicationEngine *engine, const QString &scenario, const QString &reportFile) {
    if (!engine || engine->rootObjects().isEmpty()) return;
    const QStringList scenarios{"main", "settings", "popout", "hidden-popout", "inline", "hidden-inline", "all", "close-popout", "close-settings", "close-inline"};
    if (!scenarios.contains(scenario) || reportFile.isEmpty()) { qWarning("Invalid isolated window lifecycle probe."); return; }
    QPointer<QQuickWindow> main = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
    if (!main) return;
    auto state = std::make_shared<QJsonObject>();
    state->insert("scenario", scenario);
    const auto write = [state, reportFile] {
        QSaveFile file(reportFile);
        if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(*state).toJson(QJsonDocument::Compact)); file.commit(); }
    };
    const auto named = [](const QString &name) -> QWindow * {
        for (auto *window : QGuiApplication::topLevelWindows()) if (window->objectName() == name) return window;
        return nullptr;
    };
    QTimer::singleShot(0, engine, [=] {
        if (!main) return;
        const bool settings = scenario == "settings" || scenario == "close-settings" || scenario == "all";
        const bool popout = scenario.contains("popout") || scenario == "all";
        const bool inlineVideo = scenario.contains("inline") || scenario == "all";
        if (settings) QMetaObject::invokeMethod(main, "showSettings", Q_ARG(QVariant, QVariant("appearance")));
        if (popout) {
            QMetaObject::invokeMethod(main, "showStream");
            QMetaObject::invokeMethod(main, "popOutStream", Q_ARG(QVariant, QVariant(false)));
            if (scenario == "hidden-popout") QMetaObject::invokeMethod(main, "returnStreamToApp", Q_ARG(QVariant, QVariant(false)));
        }
        if (inlineVideo) {
            QQmlComponent component(engine, QUrl("qrc:/Relay/components/InlineVideoPlayer.qml"));
            auto *item = qobject_cast<QQuickItem *>(component.create());
            if (!item) { qWarning("Could not create generated inline lifecycle probe."); return; }
            item->setParent(main); item->setParentItem(main->contentItem()); item->setSize(QSizeF(320, 180));
            item->setObjectName("generatedInlineLifecyclePlayer");
            QMetaObject::invokeMethod(item, "enterFullscreen");
            if (scenario == "hidden-inline") QMetaObject::invokeMethod(item, "leaveFullscreen");
        }
        int residentAux = 0, visibleAux = 0;
        for (auto *window : QGuiApplication::topLevelWindows()) if (window != main) { ++residentAux; visibleAux += window->isVisible(); }
        state->insert("prepared", true); state->insert("residentAux", residentAux); state->insert("visibleAux", visibleAux); write();
        QTimer::singleShot(120, engine, [=] {
            if (!main) return;
            if (scenario == "close-popout") { if (auto *window = named("streamWindow")) window->close(); }
            if (scenario == "close-settings") { if (auto *window = named("settingsWindow")) window->close(); }
            if (scenario == "close-inline") { if (auto *window = named("inlineVideoWindow")) window->close(); }
            state->insert("mainVisibleAfterAuxClose", main->isVisible());
            state->insert("streamPoppedOutAfterAuxClose", main->property("streamPoppedOut").toBool());
            state->insert("settingsVisibleAfterAuxClose", named("settingsWindow") && named("settingsWindow")->isVisible());
            state->insert("inlineVisibleAfterAuxClose", named("inlineVideoWindow") && named("inlineVideoWindow")->isVisible());
            write();
        });
        QTimer::singleShot(280, engine, [=] {
            if (!main) return;
            state->insert("mainCloseRequested", true);
            state->insert("mainCloseAccepted", main->close());
            state->insert("mainVisibleAfterClose", main->isVisible()); write();
        });
        QTimer::singleShot(600, engine, [=] {
            state->insert("stillRunningAfterClose", true);
            state->insert("mainVisibleAfterClose", main && main->isVisible()); write();
        });
    });
}
