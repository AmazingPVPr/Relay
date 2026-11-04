#include "VoiceRoster.h"

VoiceRoster::VoiceRoster(RecordModel *source,QObject *parent):QObject(parent),m_source(source){
    m_rebuild.setSingleShot(true);m_rebuild.setInterval(0);
    connect(&m_rebuild,&QTimer::timeout,this,&VoiceRoster::rebuild);
    if(source){
        connect(source,&QAbstractItemModel::rowsInserted,this,&VoiceRoster::scheduleRebuild);
        connect(source,&QAbstractItemModel::rowsRemoved,this,&VoiceRoster::scheduleRebuild);
        connect(source,&QAbstractItemModel::rowsMoved,this,&VoiceRoster::scheduleRebuild);
        connect(source,&QAbstractItemModel::modelReset,this,&VoiceRoster::scheduleRebuild);
        connect(source,&QAbstractItemModel::dataChanged,this,[this](const QModelIndex &first,const QModelIndex &last){updateRows(first,last);});
        connect(source,&QObject::destroyed,this,[this]{m_source=nullptr;scheduleRebuild();});
    }
    rebuild();
}
RecordModel *VoiceRoster::forChannel(const QString &channelId)const{return m_channels.value(channelId,const_cast<RecordModel*>(&m_empty));}
QVariantList VoiceRoster::members(const QString &channelId)const{QVariantList result;for(const auto &row:forChannel(channelId)->rows())result.append(row);return result;}
void VoiceRoster::scheduleRebuild(){if(!m_rebuild.isActive())m_rebuild.start();}
void VoiceRoster::updateRows(const QModelIndex &first,const QModelIndex &last){
    if(!m_source || m_rebuild.isActive())return;
    for(int index=first.row();index<=last.row()&&index<m_source->count();++index){
        const auto row=m_source->get(index);const auto id=row.value("recordId").toString(),channel=row.value("channelId").toString();
        auto *target=m_channels.value(channel,nullptr);
        if(m_channelForUser.value(id)!=channel || !target || target->indexOf(id)<0){scheduleRebuild();return;}
        target->upsert(row);
    }
}
void VoiceRoster::rebuild(){
    QHash<QString,QVector<QVariantMap>> groups;QHash<QString,QString> locations;
    if(m_source)for(const auto &row:m_source->rows()){
        const auto id=row.value("recordId").toString(),channel=row.value("channelId").toString();
        if(id.isEmpty()||channel.isEmpty())continue;
        groups[channel].append(row);locations.insert(id,channel);
    }
    for(auto it=groups.cbegin();it!=groups.cend();++it){
        auto *target=m_channels.value(it.key(),nullptr);
        if(!target){target=new RecordModel(this,m_source?m_source->maximumRows():1000);m_channels.insert(it.key(),target);}
        target->reconcile(it.value());
    }
    QList<RecordModel*> retired;
    for(auto it=m_channels.begin();it!=m_channels.end();){
        if(groups.contains(it.key())){++it;continue;}
        auto *target=it.value();target->clear();retired.append(target);it=m_channels.erase(it);
    }
    m_channelForUser=std::move(locations);++m_revision;emit changed();
    for(auto *target:retired)target->deleteLater();
}
