#pragma once
#include "RecordModel.h"
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QTimer>

// Stable channel models keep speaking/presence updates from recreating every
// participant delegate. Structural gateway batches are grouped once per turn.
class VoiceRoster final : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 revision READ revision NOTIFY changed)
public:
    explicit VoiceRoster(RecordModel *source, QObject *parent = nullptr);
    quint64 revision() const { return m_revision; }
    Q_INVOKABLE RecordModel *forChannel(const QString &channelId) const;
    Q_INVOKABLE QVariantList members(const QString &channelId) const;
signals:
    void changed();
private:
    void scheduleRebuild();
    void rebuild();
    void updateRows(const QModelIndex &first, const QModelIndex &last);
    QPointer<RecordModel> m_source;
    QHash<QString,RecordModel*> m_channels;
    QHash<QString,QString> m_channelForUser;
    RecordModel m_empty{this};
    QTimer m_rebuild;
    quint64 m_revision=0;
};
