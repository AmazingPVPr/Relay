#pragma once
#include <QByteArray>
#include <QtEndian>
#include <dave/dave.h>
#include <mls/messages.h>

namespace StreamTestDave {
// MLS external sender fixture following the official libdave MIT-licensed tests.
constexpr auto selfId="3333",ownerId="4444";
constexpr quint64 groupId=5555;
constexpr quint32 videoSsrc=202,audioSsrc=303;
inline const unsigned char *bytes(const QByteArray &b){return reinterpret_cast<const unsigned char*>(b.constData());}
inline unsigned char *bytes(QByteArray &b){return reinterpret_cast<unsigned char*>(b.data());}
class Group {
public:
    mlspp::CipherSuite suite{mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    mlspp::SignaturePrivateKey signing=mlspp::SignaturePrivateKey::generate(suite);
    mlspp::ExternalSender sender;
    mlspp::bytes_ns::bytes group;
    DAVESessionHandle session=nullptr;
    DAVEEncryptorHandle encryptor=nullptr;
    QByteArray ownerIdentity;
    explicit Group(const QString &owner=QString::fromLatin1(ownerId),quint64 allocationGroupId=groupId):ownerIdentity(owner.toUtf8()){
        group.resize(8);qToBigEndian<quint64>(allocationGroupId,group.data());
        sender.signature_key=signing.public_key;sender.credential=mlspp::Credential::basic({0,1,1,0});
        session=daveSessionCreate(nullptr,nullptr,nullptr,nullptr);
        const auto external=externalSender();daveSessionSetExternalSender(session,bytes(external),external.size());
        daveSessionInit(session,1,allocationGroupId,ownerIdentity.constData());
    }
    ~Group(){if(encryptor)daveEncryptorDestroy(encryptor);if(session)daveSessionDestroy(session);}
    static QByteArray array(const std::vector<uint8_t> &b){return QByteArray(reinterpret_cast<const char*>(b.data()),b.size());}
    static std::vector<uint8_t> vector(const QByteArray &b){return {bytes(b),bytes(b)+b.size()};}
    QByteArray externalSender(){return array(mlspp::tls::marshal(sender));}
    QByteArray keyPackage(){uint8_t *data=nullptr;size_t length=0;daveSessionGetMarshalledKeyPackage(session,&data,&length);QByteArray out(reinterpret_cast<char*>(data),length);daveFree(data);return out;}
    QByteArray propose(const QByteArray &key){
        auto proposal=mlspp::Proposal{mlspp::Add{{mlspp::tls::get<mlspp::KeyPackage>(mlspp::bytes_ns::bytes(vector(key)))}}};
        auto message=mlspp::external_proposal(suite,group,0,proposal,0,signing);
        mlspp::tls::ostream stream;stream<<false;stream<<std::vector<mlspp::MLSMessage>{message};return array(stream.bytes());
    }
    std::pair<QByteArray,QByteArray> split(const QByteArray &combined){
        mlspp::tls::istream stream(mlspp::bytes_ns::bytes(vector(combined)));mlspp::MLSMessage commit;mlspp::Welcome welcome;stream>>commit;stream>>welcome;
        return {array(mlspp::tls::marshal(commit)),array(mlspp::tls::marshal(welcome))};
    }
    QByteArray commitFor(const QByteArray &key){
        const auto proposal=propose(key);const char *ids[]={selfId,ownerIdentity.constData()};uint8_t *data=nullptr;size_t length=0;
        daveSessionProcessProposals(session,bytes(proposal),proposal.size(),ids,2,&data,&length);
        QByteArray out(reinterpret_cast<char*>(data),length);daveFree(data);return out;
    }
    bool accept(const QByteArray &message,bool commit){
        if(commit){auto result=daveSessionProcessCommit(session,bytes(message),message.size());const bool ok=result&&!daveCommitResultIsFailed(result)&&!daveCommitResultIsIgnored(result);if(result)daveCommitResultDestroy(result);return ok;}
        const char *ids[]={selfId,ownerIdentity.constData()};auto result=daveSessionProcessWelcome(session,bytes(message),message.size(),ids,2);if(result)daveWelcomeResultDestroy(result);return result!=nullptr;
    }
    bool prepare(const QString &codec){
        auto ratchet=daveSessionGetKeyRatchet(session,ownerIdentity.constData());if(!ratchet)return false;
        encryptor=daveEncryptorCreate();daveEncryptorSetPassthroughMode(encryptor,false);
        daveEncryptorAssignSsrcToCodec(encryptor,videoSsrc,codec=="AV1"?DAVE_CODEC_AV1:codec=="H265"?DAVE_CODEC_H265:codec=="H264"?DAVE_CODEC_H264:DAVE_CODEC_VP8);
        daveEncryptorAssignSsrcToCodec(encryptor,audioSsrc,DAVE_CODEC_OPUS);daveEncryptorSetKeyRatchet(encryptor,ratchet);daveKeyRatchetDestroy(ratchet);return true;
    }
    QByteArray encrypt(const QByteArray &plain,DAVEMediaType type=DAVE_MEDIA_TYPE_VIDEO){
        QByteArray out(daveEncryptorGetMaxCiphertextByteSize(encryptor,type,plain.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,type,type==DAVE_MEDIA_TYPE_VIDEO?videoSsrc:audioSsrc,bytes(plain),plain.size(),bytes(out),out.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)return {};
        out.resize(length);return out;
    }
};
}
