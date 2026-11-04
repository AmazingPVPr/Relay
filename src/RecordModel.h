#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <QVector>

// One small, bounded role model shared by the server, channel and message views.
// All remote text is plain text; the UI must use Text.PlainText when rendering it.
class RecordModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    explicit RecordModel(QObject *parent = nullptr, int maximumRows = 1000);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_rows.size()); }
    int maximumRows() const { return m_maximumRows; }
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QString recordIdAt(int row) const;
    Q_INVOKABLE int indexOf(const QString &id) const;
    QVariantMap byId(const QString &id) const;
    QVector<QVariantMap> rows() const { return m_rows; }
    void replace(QVector<QVariantMap> rows);
    void reconcile(QVector<QVariantMap> rows);
    void upsert(QVariantMap row);
    void patch(const QString &id, const QVariantMap &changes);
    void move(const QString &id, int destination);
    void remove(const QString &id);
    void clear();
signals:
    void countChanged();
private:
    void reindex();
    static QVariantMap complete(QVariantMap row);
    static QVariantMap compact(QVariantMap row);
    QVector<QVariantMap> m_rows;
    QHash<QString, int> m_indices;
    int m_maximumRows;
};
