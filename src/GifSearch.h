#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSettings>
#include <QVariantList>
#include <memory>
class QNetworkReply;
class GifSearch final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY changed)
public:
    explicit GifSearch(const QString &settingsFile = {}, QObject *parent=nullptr);
    bool busy()const{return m_busy;} QString error()const{return m_error;}
    QVariantList results()const{return m_results;} QVariantList categories()const{return m_categories;} QVariantList favorites()const{return m_favorites;}
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void trending();
    Q_INVOKABLE void loadCategories();
    Q_INVOKABLE void showFavorites();
    Q_INVOKABLE void setFavorite(const QVariantMap &record,bool favorite);
    Q_INVOKABLE bool isFavorite(const QString &url)const;
    Q_INVOKABLE void cancel();
    static QVariantMap validatedRecord(const QVariantMap &input);
    static bool allowedMedia(const QString &url);
signals:
    void changed();
private:
    void fetch(const QString &path,bool categories);
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QSettings> m_settings;
    QVariantList m_results,m_categories,m_favorites;
    QString m_error; bool m_busy=false; quint64 m_generation=0;
};
