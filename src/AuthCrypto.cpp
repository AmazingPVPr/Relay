#include "AuthCrypto.h"
#include <QCryptographicHash>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

struct AuthCrypto::Private {
    EVP_PKEY *key = nullptr;
    ~Private() { EVP_PKEY_free(key); }
};
AuthCrypto::AuthCrypto() : d(std::make_unique<Private>()) {}
AuthCrypto::~AuthCrypto() = default;
bool AuthCrypto::generate() {
    EVP_PKEY_free(d->key);
    d->key = nullptr;
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    if (!context) return false;
    const bool ok = EVP_PKEY_keygen_init(context) > 0 &&
        EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) > 0 &&
        EVP_PKEY_generate(context, &d->key) > 0;
    EVP_PKEY_CTX_free(context);
    if (!ok) { EVP_PKEY_free(d->key); d->key = nullptr; }
    return ok;
}
QByteArray AuthCrypto::publicKeyDer() const {
    if (!d->key) return {};
    const int length = i2d_PUBKEY(d->key, nullptr);
    if (length <= 0 || length > 4096) return {};
    QByteArray der(length, Qt::Uninitialized);
    auto *destination = reinterpret_cast<unsigned char *>(der.data());
    if (i2d_PUBKEY(d->key, &destination) != length) return {};
    return der;
}
QByteArray AuthCrypto::publicKeyBase64() const { return publicKeyDer().toBase64(); }
QByteArray AuthCrypto::urlBase64(const QByteArray &bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
QByteArray AuthCrypto::fingerprint() const {
    const auto der = publicKeyDer();
    return der.isEmpty() ? QByteArray{} : urlBase64(QCryptographicHash::hash(der, QCryptographicHash::Sha256));
}
QByteArray AuthCrypto::decrypt(const QByteArray &encodedCiphertext) const {
    if (!d->key || encodedCiphertext.isEmpty() || encodedCiphertext.size() > 1024) return {};
    const auto alphabet = encodedCiphertext.contains('-') || encodedCiphertext.contains('_') ?
        QByteArray::Base64UrlEncoding : QByteArray::Base64Encoding;
    const auto decoded = QByteArray::fromBase64Encoding(encodedCiphertext,
        alphabet | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != EVP_PKEY_get_size(d->key)) return {};
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(d->key, nullptr);
    if (!context) return {};
    size_t length = 0;
    bool ok = EVP_PKEY_decrypt_init(context) > 0 &&
        EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_OAEP_PADDING) > 0 &&
        EVP_PKEY_CTX_set_rsa_oaep_md(context, EVP_sha256()) > 0 &&
        EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) > 0 &&
        EVP_PKEY_decrypt(context, nullptr, &length,
            reinterpret_cast<const unsigned char *>(decoded.decoded.constData()), decoded.decoded.size()) > 0;
    QByteArray plaintext;
    if (ok && length > 0 && length <= 256) {
        plaintext.resize(static_cast<int>(length));
        ok = EVP_PKEY_decrypt(context, reinterpret_cast<unsigned char *>(plaintext.data()), &length,
            reinterpret_cast<const unsigned char *>(decoded.decoded.constData()), decoded.decoded.size()) > 0;
        if (ok) plaintext.resize(static_cast<int>(length));
    } else ok = false;
    EVP_PKEY_CTX_free(context);
    if (!ok) erase(plaintext);
    return plaintext;
}
void AuthCrypto::erase(QByteArray &bytes) {
    if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    bytes.clear();
}
