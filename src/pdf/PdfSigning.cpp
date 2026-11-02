// Digital signing, certificate creation and signature verification. This is
// the only file that needs the OpenSSL headers.
#include "PdfEditor.h"

#include <QByteArray>
#include <QFile>
#include <QSaveFile>
#include <QUuid>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <openssl/x509.h>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

// The text of the most recent OpenSSL error, or empty when there is none.
QString openSslErrorDetail()
{
    const unsigned long code = ERR_get_error();
    if (!code)
        return {};
    char detail[160] = {};
    ERR_error_string_n(code, detail, sizeof(detail));
    return QString::fromLocal8Bit(detail);
}

struct OpenSslSigner {
    pdf_pkcs7_signer signer = {};
    std::atomic_int references{1};
    EVP_PKEY *privateKey = nullptr;
    X509 *certificate = nullptr;
    STACK_OF(X509) *chain = nullptr;
    char error[256] = {};
};

OpenSslSigner *openSslSigner(pdf_pkcs7_signer *signer)
{
    return reinterpret_cast<OpenSslSigner *>(signer);
}

void setOpenSslError(OpenSslSigner *signer, const char *operation)
{
    const QByteArray detail = openSslErrorDetail().toLocal8Bit();
    std::snprintf(signer->error, sizeof(signer->error), "%s failed%s%s",
                  operation, detail.isEmpty() ? "." : ": ", detail.constData());
}

pdf_pkcs7_signer *keepOpenSslSigner(fz_context *, pdf_pkcs7_signer *base)
{
    openSslSigner(base)->references.fetch_add(1, std::memory_order_relaxed);
    return base;
}

void dropOpenSslSigner(fz_context *, pdf_pkcs7_signer *base)
{
    OpenSslSigner *signer = openSslSigner(base);
    if (signer->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    EVP_PKEY_free(signer->privateKey);
    X509_free(signer->certificate);
    sk_X509_pop_free(signer->chain, X509_free);
    delete signer;
}

pdf_pkcs7_distinguished_name *openSslSignerName(fz_context *ctx,
                                                 pdf_pkcs7_signer *base)
{
    OpenSslSigner *signer = openSslSigner(base);
    X509_NAME *subject = X509_get_subject_name(signer->certificate);
    if (!subject)
        return nullptr;
    const int index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
    if (index < 0)
        return nullptr;

    X509_NAME_ENTRY *entry = X509_NAME_get_entry(subject, index);
    if (!entry)
        return nullptr;
    unsigned char *utf8 = nullptr;
    const int nameLength = ASN1_STRING_to_UTF8(
        &utf8, X509_NAME_ENTRY_get_data(entry));
    if (nameLength < 0)
        return nullptr;

    pdf_pkcs7_distinguished_name *name = nullptr;
    fz_var(name);
    fz_try(ctx)
    {
        name = fz_malloc_struct(ctx, pdf_pkcs7_distinguished_name);
        name->cn = nullptr;
        name->cn = static_cast<char *>(fz_malloc(ctx, size_t(nameLength) + 1));
        std::memcpy(name->cn, utf8, size_t(nameLength));
        name->cn[nameLength] = '\0';
        name->o = nullptr;
        name->ou = nullptr;
        name->email = nullptr;
        name->c = nullptr;
    }
    fz_always(ctx)
    {
        OPENSSL_free(utf8);
    }
    fz_catch(ctx)
    {
        if (name) {
            fz_free(ctx, name->cn);
            fz_free(ctx, name);
        }
        return nullptr;
    }
    return name;
}

size_t openSslSignerMaxDigestSize(fz_context *, pdf_pkcs7_signer *)
{
    return 65536;
}

int openSslSignerCreateDigest(fz_context *ctx, pdf_pkcs7_signer *base,
                              fz_stream *input, unsigned char *output,
                              size_t outputSize)
{
    OpenSslSigner *signer = openSslSigner(base);
    PKCS7 *pkcs7 = nullptr;
    BIO *dataBio = nullptr;
    unsigned char *encoded = nullptr;
    int succeeded = 0;
    unsigned char buffer[8192];
    fz_var(pkcs7);
    fz_var(dataBio);
    fz_var(encoded);
    fz_var(succeeded);

    ERR_clear_error();
    fz_try(ctx)
    {
        pkcs7 = PKCS7_sign(nullptr, nullptr, nullptr, nullptr,
                           PKCS7_PARTIAL | PKCS7_DETACHED | PKCS7_BINARY);
        if (!pkcs7) {
            setOpenSslError(signer, "Preparing the PKCS#7 signature");
        } else if (!PKCS7_sign_add_signer(pkcs7, signer->certificate,
                                          signer->privateKey, EVP_sha256(), 0)) {
            setOpenSslError(signer, "Adding the signing certificate");
        } else if (PKCS7_set_detached(pkcs7, 1) != 1) {
            setOpenSslError(signer, "Preparing the detached signature");
        } else {
            const int chainCount = signer->chain ? sk_X509_num(signer->chain) : 0;
            bool certificatesAdded = true;
            for (int i = 0; i < chainCount; ++i) {
                if (PKCS7_add_certificate(
                        pkcs7, sk_X509_value(signer->chain, i)) != 1) {
                    certificatesAdded = false;
                    break;
                }
            }

            if (!certificatesAdded) {
                setOpenSslError(signer, "Adding the certificate chain");
            } else {
                dataBio = PKCS7_dataInit(pkcs7, nullptr);
                if (!dataBio) {
                    setOpenSslError(signer, "Preparing PDF signature data");
                } else {
                    for (;;) {
                        const size_t read = fz_read(ctx, input, buffer, sizeof(buffer));
                        if (read == 0)
                            break;
                        if (BIO_write(dataBio, buffer, int(read)) != int(read)) {
                            setOpenSslError(signer, "Hashing the PDF");
                            break;
                        }
                    }

                    if (!signer->error[0]) {
                        if (PKCS7_dataFinal(pkcs7, dataBio) != 1) {
                            setOpenSslError(signer, "Creating the PKCS#7 signature");
                        } else {
                            const int encodedSize = i2d_PKCS7(pkcs7, nullptr);
                            if (encodedSize <= 0) {
                                setOpenSslError(signer, "Encoding the PKCS#7 signature");
                            } else if (size_t(encodedSize) > outputSize) {
                                std::snprintf(
                                    signer->error, sizeof(signer->error),
                                    "The certificate chain is too large for the reserved signature area.");
                            } else {
                                encoded = static_cast<unsigned char *>(
                                    OPENSSL_malloc(size_t(encodedSize)));
                                if (!encoded) {
                                    setOpenSslError(signer, "Allocating the signature data");
                                } else {
                                    unsigned char *cursor = encoded;
                                    if (i2d_PKCS7(pkcs7, &cursor) != encodedSize) {
                                        setOpenSslError(
                                            signer, "Encoding the PKCS#7 signature");
                                    } else {
                                        std::memcpy(output, encoded,
                                                    size_t(encodedSize));
                                        succeeded = encodedSize;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    fz_always(ctx)
    {
        OPENSSL_free(encoded);
        BIO_free_all(dataBio);
        PKCS7_free(pkcs7);
    }
    fz_catch(ctx)
    {
        std::snprintf(signer->error, sizeof(signer->error),
                      "Could not read the PDF while calculating its signature.");
        return 0;
    }
    return succeeded;
}

bool createOpenSslSigner(const QString &pfxPath, const QString &password,
                         pdf_pkcs7_signer **result, QString *error)
{
    QFile file(pfxPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = "Could not read the certificate file: " + file.errorString();
        return false;
    }

    QByteArray pfxData = file.readAll();
    if (file.error() != QFileDevice::NoError || pfxData.isEmpty()) {
        *error = pfxData.isEmpty() ? QStringLiteral("The certificate file is empty.")
                                   : "Could not read the certificate file: "
                                         + file.errorString();
        return false;
    }

    QByteArray passwordUtf8 = password.toUtf8();
    const unsigned char *cursor =
        reinterpret_cast<const unsigned char *>(pfxData.constData());
    PKCS12 *pfx = d2i_PKCS12(nullptr, &cursor, long(pfxData.size()));
    EVP_PKEY *privateKey = nullptr;
    X509 *certificate = nullptr;
    STACK_OF(X509) *chain = nullptr;
    ERR_clear_error();
    const bool parsed = pfx
                        && PKCS12_parse(pfx, passwordUtf8.constData(), &privateKey,
                                        &certificate, &chain) == 1
                        && privateKey && certificate;
    OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
    OPENSSL_cleanse(pfxData.data(), size_t(pfxData.size()));
    PKCS12_free(pfx);
    if (!parsed) {
        EVP_PKEY_free(privateKey);
        X509_free(certificate);
        sk_X509_pop_free(chain, X509_free);
        *error = "Could not open the PFX/P12 certificate. Check the file and password.";
        return false;
    }
    if (EVP_PKEY_base_id(privateKey) != EVP_PKEY_RSA
        && EVP_PKEY_base_id(privateKey) != EVP_PKEY_RSA_PSS) {
        EVP_PKEY_free(privateKey);
        X509_free(certificate);
        sk_X509_pop_free(chain, X509_free);
        *error = "This version supports RSA signing certificates only.";
        return false;
    }

    auto *signer = new (std::nothrow) OpenSslSigner;
    if (!signer) {
        EVP_PKEY_free(privateKey);
        X509_free(certificate);
        sk_X509_pop_free(chain, X509_free);
        *error = "Could not allocate a signing context.";
        return false;
    }
    signer->privateKey = privateKey;
    signer->certificate = certificate;
    signer->chain = chain;
    signer->signer.keep = keepOpenSslSigner;
    signer->signer.drop = dropOpenSslSigner;
    signer->signer.get_signing_name = openSslSignerName;
    signer->signer.max_digest_size = openSslSignerMaxDigestSize;
    signer->signer.create_digest = openSslSignerCreateDigest;
    *result = &signer->signer;
    return true;
}

bool createSelfSignedCertificateFile(const QString &path,
                                     const QString &commonName,
                                     const QString &password,
                                     QString *error)
{
    const QString normalizedName = commonName.trimmed();
    const QByteArray nameUtf8 = normalizedName.toUtf8();
    QByteArray passwordUtf8 = password.toUtf8();
    if (normalizedName.isEmpty() || nameUtf8.size() > 255
        || normalizedName.contains(QChar(0))) {
        *error = "Enter a name of at most 255 UTF-8 bytes.";
        OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
        return false;
    }
    if (password.size() < 12 || password.contains(QChar(0))) {
        *error = "Use a certificate password with at least 12 characters.";
        OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
        return false;
    }

    using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
    using Serial = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
    using Pkcs12 = std::unique_ptr<PKCS12, decltype(&PKCS12_free)>;
    using Extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;

    ERR_clear_error();
    KeyContext keyContext(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                          EVP_PKEY_CTX_free);
    EVP_PKEY *rawKey = nullptr;
    if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) <= 0
        || EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 3072) <= 0
        || EVP_PKEY_keygen(keyContext.get(), &rawKey) <= 0) {
        OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
        const QString detail = openSslErrorDetail();
        *error = QString("Could not generate the signing key%1")
                     .arg(detail.isEmpty() ? QStringLiteral(".") : ": " + detail);
        return false;
    }
    Key key(rawKey, EVP_PKEY_free);
    Certificate certificate(X509_new(), X509_free);
    if (!certificate) {
        OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
        *error = "Could not create the signing certificate.";
        return false;
    }

    unsigned char serialBytes[16];
    serialBytes[0] = 0;
    Serial serial(nullptr, BN_free);
    bool certificateReady = RAND_bytes(serialBytes, sizeof(serialBytes)) == 1;
    serialBytes[0] &= 0x7f;
    serialBytes[0] |= 0x01;
    serial.reset(BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr));
    OPENSSL_cleanse(serialBytes, sizeof(serialBytes));
    certificateReady =
        certificateReady && serial
        && X509_set_version(certificate.get(), 2) == 1
        && BN_to_ASN1_INTEGER(serial.get(),
                              X509_get_serialNumber(certificate.get())) != nullptr
        && X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) != nullptr
        && X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                           5L * 365L * 24L * 60L * 60L) != nullptr
        && X509_set_pubkey(certificate.get(), key.get()) == 1;

    X509_NAME *subject = X509_get_subject_name(certificate.get());
    certificateReady =
        certificateReady && subject
        && X509_NAME_add_entry_by_NID(
               subject, NID_commonName, MBSTRING_UTF8,
               reinterpret_cast<const unsigned char *>(nameUtf8.constData()),
               nameUtf8.size(), -1, 0) == 1
        && X509_set_issuer_name(certificate.get(), subject) == 1;

    const auto addExtension = [&certificate](int nid, const char *value) {
        Extension extension(
            X509V3_EXT_conf_nid(nullptr, nullptr, nid,
                                const_cast<char *>(value)),
            X509_EXTENSION_free);
        return extension
               && X509_add_ext(certificate.get(), extension.get(), -1) == 1;
    };
    certificateReady =
        certificateReady
        && addExtension(NID_basic_constraints, "critical,CA:FALSE")
        && addExtension(NID_key_usage, "critical,digitalSignature,nonRepudiation")
        && X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0;
    if (!certificateReady) {
        OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
        const QString detail = openSslErrorDetail();
        *error = QString("Could not create the signing certificate%1")
                     .arg(detail.isEmpty() ? QStringLiteral(".") : ": " + detail);
        return false;
    }

    const QByteArray friendlyName = normalizedName.toUtf8();
    Pkcs12 pfx(PKCS12_create(
                   passwordUtf8.constData(), friendlyName.constData(), key.get(),
                   certificate.get(), nullptr, NID_aes_256_cbc,
                   NID_aes_256_cbc, 2048, 2048, 0),
               PKCS12_free);
    OPENSSL_cleanse(passwordUtf8.data(), size_t(passwordUtf8.size()));
    if (!pfx) {
        *error = "Could not package the certificate as a password-protected P12 file.";
        return false;
    }

    const int encodedSize = i2d_PKCS12(pfx.get(), nullptr);
    if (encodedSize <= 0) {
        *error = "Could not encode the P12 certificate.";
        return false;
    }
    QByteArray encoded(encodedSize, Qt::Uninitialized);
    unsigned char *cursor =
        reinterpret_cast<unsigned char *>(encoded.data());
    if (i2d_PKCS12(pfx.get(), &cursor) != encodedSize) {
        OPENSSL_cleanse(encoded.data(), size_t(encoded.size()));
        *error = "Could not encode the P12 certificate.";
        return false;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        OPENSSL_cleanse(encoded.data(), size_t(encoded.size()));
        *error = "Could not save the certificate file: " + output.errorString();
        return false;
    }
    const qint64 written = output.write(encoded);
    OPENSSL_cleanse(encoded.data(), size_t(encoded.size()));
    if (written != encodedSize || !output.commit()) {
        *error = "Could not save the certificate file: " + output.errorString();
        return false;
    }
    return true;
}

struct MuPdfStreamBio {
    fz_context *ctx = nullptr;
    fz_stream *stream = nullptr;
    bool failed = false;
};

int readMuPdfStreamBio(BIO *bio, char *buffer, int bufferSize)
{
    auto *source = static_cast<MuPdfStreamBio *>(BIO_get_data(bio));
    if (!source || source->failed)
        return -1;

    size_t read = 0;
    fz_var(read);
    fz_try(source->ctx)
    {
        read = fz_read(source->ctx, source->stream,
                       reinterpret_cast<unsigned char *>(buffer),
                       size_t(bufferSize));
    }
    fz_catch(source->ctx)
    {
        source->failed = true;
        return -1;
    }
    return int(read);
}

long controlMuPdfStreamBio(BIO *, int command, long, void *)
{
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}

BIO_METHOD *muPdfStreamBioMethod()
{
    static BIO_METHOD *method = [] {
        BIO_METHOD *created = BIO_meth_new(
            BIO_TYPE_NONE, "MuPDF signature stream");
        if (!created)
            return static_cast<BIO_METHOD *>(nullptr);
        if (BIO_meth_set_read(created, readMuPdfStreamBio) != 1
            || BIO_meth_set_ctrl(created, controlMuPdfStreamBio) != 1) {
            BIO_meth_free(created);
            return static_cast<BIO_METHOD *>(nullptr);
        }
        return created;
    }();
    return method;
}

bool verifyPkcs7Signature(fz_context *ctx, fz_stream *signedBytes,
                          const char *signatureBytes, size_t signatureLength,
                          PdfEditor::DigitalSignatureInfo *info)
{
    if (!signatureBytes || signatureLength == 0
        || signatureLength > static_cast<size_t>(LONG_MAX))
        return false;

    const unsigned char *cursor =
        reinterpret_cast<const unsigned char *>(signatureBytes);
    PKCS7 *pkcs7 = d2i_PKCS7(nullptr, &cursor, long(signatureLength));
    if (!pkcs7)
        return false;

    BIO_METHOD *method = muPdfStreamBioMethod();
    BIO *data = method ? BIO_new(method) : nullptr;
    BIO *pkcs7Data = nullptr;
    MuPdfStreamBio source{ctx, signedBytes, false};
    if (data) {
        BIO_set_data(data, &source);
        BIO_set_init(data, 1);
    }

    ERR_clear_error();
    if (data)
        pkcs7Data = PKCS7_dataInit(pkcs7, data);
    char buffer[4096];
    if (pkcs7Data) {
        while (BIO_read(pkcs7Data, buffer, sizeof(buffer)) > 0) {
        }
    }

    STACK_OF(PKCS7_SIGNER_INFO) *signerInfos =
        PKCS7_get_signer_info(pkcs7);
    if (signerInfos && sk_PKCS7_SIGNER_INFO_num(signerInfos) > 0) {
        PKCS7_SIGNER_INFO *signerInfo =
            sk_PKCS7_SIGNER_INFO_value(signerInfos, 0);
        X509 *certificate = PKCS7_cert_from_signer_info(pkcs7, signerInfo);
        info->cryptographicallyValid =
            pkcs7Data && certificate
            && PKCS7_signatureVerify(pkcs7Data, pkcs7, signerInfo, certificate) > 0
            && !source.failed;
        if (certificate) {
            X509_NAME *subject = X509_get_subject_name(certificate);
            const int commonNameIndex =
                subject ? X509_NAME_get_index_by_NID(subject, NID_commonName, -1)
                        : -1;
            if (commonNameIndex >= 0) {
                X509_NAME_ENTRY *entry =
                    X509_NAME_get_entry(subject, commonNameIndex);
                unsigned char *commonName = nullptr;
                const int commonNameLength =
                    entry ? ASN1_STRING_to_UTF8(
                                &commonName, X509_NAME_ENTRY_get_data(entry))
                          : -1;
                if (commonNameLength >= 0) {
                    info->signerName = QString::fromUtf8(
                        reinterpret_cast<const char *>(commonName),
                        commonNameLength);
                    OPENSSL_free(commonName);
                }
            }

            EVP_PKEY *publicKey = X509_get_pubkey(certificate);
            info->selfSignedCertificate =
                publicKey
                && X509_check_issued(certificate, certificate) == X509_V_OK
                && X509_verify(certificate, publicKey) == 1;
            EVP_PKEY_free(publicKey);
        }
    }
    if (pkcs7Data)
        BIO_free_all(pkcs7Data);
    else
        BIO_free(data);
    PKCS7_free(pkcs7);
    return info->cryptographicallyValid;
}

}

bool PdfEditor::createSelfSignedCertificate(const QString &path,
                                            const QString &commonName,
                                            const QString &password,
                                            QString *error)
{
    if (!error)
        return false;
    error->clear();
    return createSelfSignedCertificateFile(path, commonName, password, error);
}

QVector<PdfEditor::DigitalSignatureInfo> PdfEditor::digitalSignatures()
{
    QVector<DigitalSignatureInfo> signatures;
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return signatures;
    }

    pdf_page *page = nullptr;
    fz_stream *signedBytes = nullptr;
    char *signatureBytes = nullptr;
    int succeeded = 1;
    fz_var(page);
    fz_var(signedBytes);
    fz_var(signatureBytes);
    fz_var(succeeded);
    fz_var(signatures);

    fz_try(m_ctx)
    {
        const int pageCount = pdf_count_pages(m_ctx, m_doc);
        for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
            page = pdf_load_page(m_ctx, m_doc, pageIndex);
            for (pdf_annot *widget = pdf_first_widget(m_ctx, page); widget;
                 widget = pdf_next_widget(m_ctx, widget)) {
                if (pdf_widget_type(m_ctx, widget) != PDF_WIDGET_TYPE_SIGNATURE)
                    continue;

                pdf_obj *field = pdf_annot_obj(m_ctx, widget);
                if (!pdf_signature_is_signed(m_ctx, m_doc, field))
                    continue;

                DigitalSignatureInfo info;
                signedBytes = pdf_signature_hash_bytes(m_ctx, m_doc, field);
                const size_t signatureLength =
                    pdf_signature_contents(m_ctx, m_doc, field,
                                           &signatureBytes);
                if (signedBytes && signatureBytes && signatureLength > 0) {
                    verifyPkcs7Signature(m_ctx, signedBytes, signatureBytes,
                                         signatureLength, &info);
                }
                info.documentChangedSinceSigning =
                    pdf_signature_incremental_change_since_signing(
                        m_ctx, m_doc, field) != 0;
                signatures.append(info);

                fz_drop_stream(m_ctx, signedBytes);
                signedBytes = nullptr;
                fz_free(m_ctx, signatureBytes);
                signatureBytes = nullptr;
            }
            pdf_drop_page(m_ctx, page);
            page = nullptr;
        }
    }
    fz_always(m_ctx)
    {
        fz_drop_stream(m_ctx, signedBytes);
        fz_free(m_ctx, signatureBytes);
        if (page)
            pdf_drop_page(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        signatures.clear();
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        succeeded = 0;
    }
    if (!succeeded)
        return {};
    return signatures;
}

bool PdfEditor::signAs(const QString &path, int pageIndex, const QRectF &bounds,
                       const QString &pfxPath, const QString &pfxPassword)
{
    m_error.clear();
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (pageIndex < 0 || !bounds.isValid() || bounds.width() < 24.0
        || bounds.height() < 18.0) {
        m_error = "Choose a larger signature area on the page.";
        return false;
    }

    pdf_pkcs7_signer *signer = nullptr;
    QString signerError;
    if (!createOpenSslSigner(pfxPath, pfxPassword, &signer, &signerError)) {
        m_error = signerError;
        return false;
    }

    // MuPDF errors longjmp out of the fz_try block, skipping the destructors
    // of anything built inside it, so the Qt objects are created up front.
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray fieldName =
        ("PDFHomeSignature_" + QUuid::createUuid().toString(QUuid::WithoutBraces))
            .toUtf8();
    pdf_page *page = nullptr;
    pdf_annot *widget = nullptr;
    int succeeded = 0;
    const char *stage = "creating the signature field";
    fz_var(page);
    fz_var(widget);
    fz_var(succeeded);
    fz_var(stage);

    fz_try(m_ctx)
    {
        page = pdf_load_page(m_ctx, m_doc, pageIndex);
        widget = pdf_create_signature_widget(
            m_ctx, page, const_cast<char *>(fieldName.constData()));
        if (!widget)
            fz_throw(m_ctx, FZ_ERROR_GENERIC,
                     "Could not create the PDF signature field");

        const fz_rect rect = {
            float(bounds.left()), float(bounds.top()),
            float(bounds.right()), float(bounds.bottom())
        };
        pdf_set_annot_rect(m_ctx, widget, rect);
        pdf_update_annot(m_ctx, widget);
        pdf_sign_signature(m_ctx, widget, signer,
                           PDF_SIGNATURE_SHOW_LABELS
                               | PDF_SIGNATURE_SHOW_DN
                               | PDF_SIGNATURE_SHOW_DATE
                               | PDF_SIGNATURE_SHOW_TEXT_NAME
                               | PDF_SIGNATURE_SHOW_LOGO,
                           nullptr, nullptr, nullptr);

        stage = "writing the signed PDF";
        pdf_write_options options = pdf_default_write_options;
        options.do_incremental = 1;
        pdf_save_document(m_ctx, m_doc, pathUtf8.constData(), &options);
        if (openSslSigner(signer)->error[0])
            m_error = QString::fromLocal8Bit(openSslSigner(signer)->error);
        else
            succeeded = 1;
    }
    fz_always(m_ctx)
    {
        if (openSslSigner(signer)->error[0] && m_error.isEmpty())
            m_error = QString::fromLocal8Bit(openSslSigner(signer)->error);
        if (widget)
            pdf_drop_widget(m_ctx, widget);
        if (page)
            pdf_drop_page(m_ctx, page);
        pdf_drop_signer(m_ctx, signer);
    }
    fz_catch(m_ctx)
    {
        if (m_error.isEmpty())
            m_error = QString("%1 failed: %2")
                          .arg(QString::fromLatin1(stage),
                               QString::fromUtf8(fz_caught_message(m_ctx)));
        return false;
    }
    if (!succeeded && m_error.isEmpty())
        m_error = QString("%1 did not complete.").arg(QString::fromLatin1(stage));
    return succeeded != 0;
}
