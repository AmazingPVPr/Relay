#pragma once

#include <QLoggingCategory>
#include <QtGlobal>
#include <cstring>
extern "C" {
#include <libavutil/log.h>
}

namespace MediaLogging {
inline QLoggingCategory::CategoryFilter previousFilter = nullptr;

inline void filter(QLoggingCategory *category) {
    if (previousFilter) previousFilter(category);
    // Backend diagnostics may contain complete signed attachment/media URLs.
    // Playback errors remain available through the player's error signals.
    if (std::strncmp(category->categoryName(), "qt.multimedia", 13) == 0) {
        category->setEnabled(QtDebugMsg, false);
        category->setEnabled(QtInfoMsg, false);
        category->setEnabled(QtWarningMsg, false);
        category->setEnabled(QtCriticalMsg, false);
    }
}

inline void install() {
    // Qt's FFmpeg integration otherwise restores verbose logging at startup.
    qunsetenv("QT_FFMPEG_DEBUG");
    av_log_set_level(AV_LOG_QUIET);
    previousFilter = QLoggingCategory::installFilter(filter);
}
}
