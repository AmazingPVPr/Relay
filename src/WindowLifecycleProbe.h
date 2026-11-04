#pragma once
#include <QString>
class QQmlApplicationEngine;
// Generated-demo-only probe. It creates no media/session and closes only its
// isolated application process; it is never enabled for a signed-in session.
void runWindowLifecycleProbe(QQmlApplicationEngine *engine, const QString &scenario, const QString &reportFile);
