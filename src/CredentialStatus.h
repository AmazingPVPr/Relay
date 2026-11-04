#pragma once
#include "CredentialStore.h"
#include <QPointer>

// QML receives sign-in progress and cancellation, never the wallet's token signal.
class CredentialStatus final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool remembered READ remembered NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit CredentialStatus(CredentialStore *store,QObject *parent=nullptr):QObject(parent),m_store(store) {
        if(store)connect(store,&CredentialStore::changed,this,&CredentialStatus::changed);
    }
    bool busy()const{return m_store && m_store->busy();}
    bool remembered()const{return m_store && m_store->remembered();}
    QString status()const{return m_store ? m_store->status() : QString{};}
    QString error()const{return m_store ? m_store->error() : QString{};}
    Q_INVOKABLE void cancelRestore(){if(m_store)m_store->cancelRestore();}
signals:
    void changed();
private:
    QPointer<CredentialStore> m_store;
};
