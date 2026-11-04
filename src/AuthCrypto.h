#pragma once
#include <QByteArray>
#include <memory>

// Ephemeral RSA key material, held only in OpenSSL memory for one QR login.
class AuthCrypto final {
public:
    AuthCrypto();
    ~AuthCrypto();
    AuthCrypto(const AuthCrypto &) = delete;
    AuthCrypto &operator=(const AuthCrypto &) = delete;
    bool generate();
    QByteArray publicKeyDer() const;
    QByteArray publicKeyBase64() const;
    QByteArray fingerprint() const;
    QByteArray decrypt(const QByteArray &encodedCiphertext) const;
    static QByteArray urlBase64(const QByteArray &bytes);
    static void erase(QByteArray &bytes);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
