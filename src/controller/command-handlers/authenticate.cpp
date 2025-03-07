/*
 * Copyright (c) 2020-2024 Estonian Information System Authority
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "authenticate.hpp"

#include "signauthutils.hpp"

#include <QApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <QScopeGuard>

#include <map>

using namespace electronic_id;

namespace
{

// Use common base64-encoding defaults.
constexpr auto BASE64_OPTIONS = QByteArray::Base64Encoding | QByteArray::KeepTrailingEquals;

QVariantMap createAuthenticationToken(const QString& signatureAlgorithm,
                                      const QByteArray& certificateDer, const QByteArray& signature)
{
    return QVariantMap {
        {"unverifiedCertificate", QString(certificateDer.toBase64(BASE64_OPTIONS))},
        {"algorithm", signatureAlgorithm},
        {"signature", QString(signature)},
        {"format", QStringLiteral("web-eid:1.0")},
        {"appVersion",
         QStringLiteral("https://web-eid.eu/web-eid-app/releases/%1")
             .arg(QApplication::applicationVersion())},
    };
}

QByteArray createSignature(const QString& origin, const QString& challengeNonce,
                           const ElectronicID& eid, pcsc_cpp::byte_vector&& pin)
{
    static const std::map<JsonWebSignatureAlgorithm, QCryptographicHash::Algorithm>
        SIGNATURE_ALGO_TO_HASH {
            {JsonWebSignatureAlgorithm::RS256, QCryptographicHash::Sha256},
            {JsonWebSignatureAlgorithm::PS256, QCryptographicHash::Sha256},
            {JsonWebSignatureAlgorithm::ES256, QCryptographicHash::Sha256},
            {JsonWebSignatureAlgorithm::ES384, QCryptographicHash::Sha384},
            {JsonWebSignatureAlgorithm::ES512, QCryptographicHash::Sha512},
        };

    if (!SIGNATURE_ALGO_TO_HASH.count(eid.authSignatureAlgorithm())) {
        THROW(ProgrammingError,
              "Hash algorithm mapping missing for signature algorithm "
                  + std::string(eid.authSignatureAlgorithm()));
    }

    const auto hashAlgo = SIGNATURE_ALGO_TO_HASH.at(eid.authSignatureAlgorithm());

    // Take the hash of the origin and nonce to ensure field separation.
    const auto originHash = QCryptographicHash::hash(origin.toUtf8(), hashAlgo);
    const auto challengeNonceHash = QCryptographicHash::hash(challengeNonce.toUtf8(), hashAlgo);

    // The value that is signed is hash(origin)+hash(challenge).
    const auto hashToBeSignedQBytearray =
        QCryptographicHash::hash(originHash + challengeNonceHash, hashAlgo);
    const pcsc_cpp::byte_vector hashToBeSigned {hashToBeSignedQBytearray.cbegin(),
                                                hashToBeSignedQBytearray.cend()};

    const auto signature = eid.signWithAuthKey(std::move(pin), hashToBeSigned);

    return QByteArray::fromRawData(reinterpret_cast<const char*>(signature.data()),
                                   int(signature.size()))
        .toBase64(BASE64_OPTIONS);
}

} // namespace

Authenticate::Authenticate(const CommandWithArguments& cmd) : CertificateReader(cmd)
{
    const auto arguments = cmd.second;
    requireArgumentsAndOptionalLang(
        {"challengeNonce", "origin"}, arguments,
        R"("challengeNonce": "<challenge nonce>", "origin": "<origin URL>")");

    challengeNonce = validateAndGetArgument<decltype(challengeNonce)>(
        QStringLiteral("challengeNonce"), arguments);
    // nonce must contain at least 256 bits of entropy and is usually Base64-encoded, so the
    // required byte length is 44, the length of 32 Base64-encoded bytes.
    if (challengeNonce.length() < 44) {
        THROW(CommandHandlerInputDataError,
              "Challenge nonce argument 'challengeNonce' must be at least 44 characters long");
    }
    if (challengeNonce.length() > 128) {
        THROW(CommandHandlerInputDataError,
              "Challenge nonce argument 'challengeNonce' cannot be longer than 128 characters");
    }
    validateAndStoreOrigin(arguments);
}

QVariantMap Authenticate::onConfirm(WebEidUI* window,
                                    const CardCertificateAndPinInfo& cardCertAndPin)
{
    try {
        const auto signatureAlgorithm =
            QString::fromStdString(cardCertAndPin.cardInfo->eid().authSignatureAlgorithm());
        pcsc_cpp::byte_vector pin;
        // Reserve space for APDU overhead (5 bytes) + PIN padding (16 bytes) to prevent PIN memory
        // reallocation. The 16-byte limit comes from the max PIN length of 12 bytes across all card
        // implementations in lib/libelectronic-id/src/electronic-ids/pcsc/.
        pin.reserve(5 + 16);
        getPin(pin, cardCertAndPin.cardInfo->eid(), window);
        const auto signature = createSignature(origin.url(), challengeNonce,
                                               cardCertAndPin.cardInfo->eid(), std::move(pin));
        return createAuthenticationToken(signatureAlgorithm, cardCertAndPin.certificateBytesInDer,
                                         signature);

    } catch (const VerifyPinFailed& failure) {
        switch (failure.status()) {
        case VerifyPinFailed::Status::PIN_ENTRY_CANCEL:
        case VerifyPinFailed::Status::PIN_ENTRY_TIMEOUT:
            break;
        case VerifyPinFailed::Status::PIN_ENTRY_DISABLED:
            emit retry(RetriableError::PIN_VERIFY_DISABLED);
            break;
        default:
            emit verifyPinFailed(failure.status(), failure.retries());
        }
        if (failure.retries() > 0) {
            throw CommandHandlerVerifyPinFailed(failure.what());
        }
        throw;
    }
}

void Authenticate::connectSignals(const WebEidUI* window)
{
    CertificateReader::connectSignals(window);

    connect(this, &Authenticate::verifyPinFailed, window, &WebEidUI::onVerifyPinFailed);
}
