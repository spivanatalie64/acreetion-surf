/*
 * CDMWidevine.cpp — Chromium Widevine CDM shim, WebCore glue.
 * Mirrors CDMThunder.cpp (Igalia/Metrological, TriState) structure.
 * Decrypt path: Chromium libwidevinecdm.so via CDMi v10 (public headers).
 *
 * v1 scope-limitations kept deliberately honest:
 *   • module-global HostSinks → last-opened session receives callbacks.
 *   • Temporary sessions only; persistent-store flows exist logically but
 *     LoadSession/RemoveSessionData return unsupported.
 *   • FileIO state persists under ~/.acreetion/widevine-cdm/ (L3 secret pad).
 */

#include "CDMWidevine.h"

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)

#include "CDMUtilities.h"
#include "GStreamerCommon.h"
#include "GStreamerCommonHelpers.h"
#include "SharedBuffer.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/text/WTFString.h>

#include <gst/base/gstbytereader.h>
#include <gst/gst.h>

GST_DEBUG_CATEGORY_STATIC(webkitMediaWidevineDecryptDebugCategory);
#define GST_CAT_DEFAULT webkitMediaWidevineDecryptDebugCategory

namespace WebCore {

// Common shortcuts
static const String& widevineKeySystemAtom()
{
    return GStreamerEMEUtilities::s_WidevineKeySystem;
}

static WebKitWidevine::WidevineCdmModule& module()
{
    return WebKitWidevine::WidevineCdmModule::shared();
}

// ────────────────────────────────────────────────────────────────────────────
// CDMFactoryWidevine
// ────────────────────────────────────────────────────────────────────────────
CDMFactoryWidevine& CDMFactoryWidevine::singleton()
{
    static NeverDestroyed<CDMFactoryWidevine> factory;
    return factory;
}

bool CDMFactoryWidevine::supportsKeySystem(const String& keySystem)
{
    if (keySystem != widevineKeySystemAtom())
        return false;
    module().load();
    return module().available();
}

std::unique_ptr<CDMPrivate> CDMFactoryWidevine::createCDM(const String& keySystem,
                                                          const String&,
                                                          const CDMPrivateClient&)
{
    if (!supportsKeySystem(keySystem))
        return nullptr;
    return std::make_unique<CDMPrivateWidevine>();
}

RefPtr<CDMProxy> CDMFactoryWidevine::createCDMProxy(const String& keySystem)
{
    if (keySystem != widevineKeySystemAtom() || !module().load())
        return nullptr;
    return adoptRef(*new CDMProxyWidevine(keySystem));
}

// ────────────────────────────────────────────────────────────────────────────
// CDMPrivateWidevine
// ────────────────────────────────────────────────────────────────────────────
Vector<String> CDMPrivateWidevine::supportedInitDataTypes() const
{
    // Chromium's v10 CDMi interface handles cenc / webm / keyids only.
    return { "cenc"_s, "webm"_s, "keyids"_s };
}

bool CDMPrivateWidevine::supportsConfiguration(const CDMKeySystemConfiguration& configuration) const
{
    for (auto& audioType : configuration.audioTypes) {
        if (!GStreamerEMEUtilities::isSupportedAudioContentType(audioType.contentType()))
            return false;
    }
    for (auto& videoType : configuration.videoTypes) {
        if (!GStreamerEMEUtilities::isSupportedVideoContentType(videoType.contentType()))
            return false;
    }
    return true;
}

bool CDMPrivateWidevine::supportsConfigurationWithRestrictions(const CDMKeySystemConfiguration& configuration,
                                                               const CDMRestrictions&) const
{
    return supportsConfiguration(configuration);
}

bool CDMPrivateWidevine::supportsSessionTypeWithConfiguration(const CDMSessionType& sessionType,
                                                              const CDMKeySystemConfiguration& configuration) const
{
    if (sessionType != CDMSessionType::Temporary)
        return false;
    return supportsConfiguration(configuration);
}

Vector<String> CDMPrivateWidevine::supportedRobustnesses() const
{
    // L3 only — this is a software CDM; claiming any hardware-security class
    // would be dishonest.
    return { ""_s, "L3"_s };
}

CDMRequirement CDMPrivateWidevine::distinctiveIdentifiersRequirement(const CDMKeySystemConfiguration&,
                                                                     const CDMRestrictions&) const
{
    return CDMRequirement::NotAllowed;
}

CDMRequirement CDMPrivateWidevine::persistentStateRequirement(const CDMKeySystemConfiguration&,
                                                              const CDMRestrictions&) const
{
    return CDMRequirement::Optional;
}

bool CDMPrivateWidevine::distinctiveIdentifiersAreUniquePerOriginAndClearable(const CDMKeySystemConfiguration&) const
{
    return true;
}

RefPtr<CDMInstance> CDMPrivateWidevine::createInstance()
{
    return adoptRef(*new CDMInstanceWidevine());
}

void CDMPrivateWidevine::loadAndInitialize()
{
    module().load();
}

bool CDMPrivateWidevine::supportsServerCertificates() const
{
    return true;
}

bool CDMPrivateWidevine::supportsSessions() const
{
    return true;
}

bool CDMPrivateWidevine::supportsInitData(const String& initDataType, const SharedBuffer& data) const
{
    UNUSED_PARAM(data);
    return supportedInitDataTypes().contains(initDataType);
}

RefPtr<SharedBuffer> CDMPrivateWidevine::sanitizeResponse(const SharedBuffer& response) const
{
    // Chromium CDMs accept raw license responses.
    return const_cast<SharedBuffer&>(response).copyRef();
}

std::optional<String> CDMPrivateWidevine::sanitizeSessionId(const String& sessionId) const
{
    if (sessionId.isEmpty())
        return std::nullopt;
    return sessionId;
}

// ────────────────────────────────────────────────────────────────────────────
// CDMInstanceWidevine
// ────────────────────────────────────────────────────────────────────────────
CDMInstanceWidevine::CDMInstanceWidevine()
    : CDMInstanceProxy(widevineKeySystemAtom())
    , m_keySystem(widevineKeySystemAtom())
{
}

CDMInstance::ImplementationType CDMInstanceWidevine::implementationType() const
{
    // CDMInstance.h's ImplementationType enum has no Widevine member upstream —
    // the shim reuses ClearKey as the "other CDM" bucket at the type level so
    // no interface break is needed. The type is not used to *select* CDM
    // behavior anywhere (the keySystem string is the discriminator).
    return ImplementationType::ClearKey;
}

void CDMInstanceWidevine::initializeWithConfiguration(const CDMKeySystemConfiguration&,
                                                      AllowDistinctiveIdentifiers allowDistinctiveIdentifiers,
                                                      AllowPersistentState allowPersistentState,
                                                      SuccessCallback&& callback)
{
    if (!module().load() || !module().impl()) {
        callback(CDMInstanceSuccessValue::Failed);
        return;
    }
    module().impl()->Initialize(
        allowDistinctiveIdentifiers == AllowDistinctiveIdentifiers::Yes,
        allowPersistentState == AllowPersistentState::Yes,
        /*use_hw_secure_codecs=*/false);
    callback(CDMInstanceSuccessValue::Succeeded);
}

void CDMInstanceWidevine::setServerCertificate(Ref<SharedBuffer>&& certificate, SuccessCallback&& callback)
{
    if (!module().impl()) {
        callback(CDMInstanceSuccessValue::Failed);
        return;
    }
    auto certSpan = certificate->span();
    module().impl()->SetServerCertificate(certSpan.data(), static_cast<uint32_t>(certSpan.size()));
    callback(CDMInstanceSuccessValue::Succeeded);
}

void CDMInstanceWidevine::setStorageDirectory(const String&)
{
    // FileIO lives under ~/.acreetion/widevine-cdm — see CdmModule.cpp.
}

const String& CDMInstanceWidevine::keySystem() const
{
    return m_keySystem;
}

RefPtr<CDMInstanceSession> CDMInstanceWidevine::createSession()
{
    return adoptRef(*new CDMInstanceSessionWidevine(*this));
}

// ────────────────────────────────────────────────────────────────────────────
// CDMInstanceSessionWidevine
// ────────────────────────────────────────────────────────────────────────────
CDMInstanceSessionWidevine::CDMInstanceSessionWidevine(CDMInstanceWidevine& instance)
    : CDMInstanceSessionProxy(instance)
{
    setupHostSinks();
}

CDMInstanceSessionWidevine::~CDMInstanceSessionWidevine() = default;

void CDMInstanceSessionWidevine::setupHostSinks()
{
    auto& sinks = module().sinks();

    sinks.onInitialized = [](bool success) {
        GST_DEBUG("Widevine CDM initialized: %s", success ? "yes" : "no");
    };

    sinks.onResolveNewSession = [this](uint32_t, const std::string& sessionId) {
        m_sessionID = String::fromUTF8(sessionId.data());
        if (m_licenseCallback && m_initData)
            m_licenseCallback(m_initData->copyRef(), m_sessionID, false,
                              CDMInstanceSession::SuccessValue::Succeeded);
    };

    sinks.onResolve = [this](uint32_t promiseId) {
        if (promiseId == 2 && m_licenseUpdateCallback) {
            std::optional<CDMInstanceSession::KeyStatusVector> changed;
            m_licenseUpdateCallback(false, WTF::move(changed), std::nullopt,
                                    std::nullopt, CDMInstanceSession::SuccessValue::Succeeded);
            m_licenseUpdateCallback = nullptr;
        }
    };

    sinks.onReject = [this](uint32_t promiseId, cdm::Exception exception, uint32_t systemCode, const char* msg) {
        GST_ERROR("CDM promise rejected: exception=%d code=%u: %s",
                  static_cast<int>(exception), systemCode, msg ? msg : "(none)");
        if (promiseId == 1 && m_licenseCallback) {
            if (m_initData)
                m_licenseCallback(m_initData->copyRef(), { }, false,
                                  CDMInstanceSession::SuccessValue::Failed);
            m_licenseCallback = nullptr;
        }
        if (promiseId == 2 && m_licenseUpdateCallback) {
            m_licenseUpdateCallback(false, std::nullopt, std::nullopt,
                                    std::nullopt, CDMInstanceSession::SuccessValue::Failed);
            m_licenseUpdateCallback = nullptr;
        }
    };

    sinks.onSessionMessage = [this](const std::string& sid, cdm::MessageType msgType,
                                    const uint8_t* message, size_t messageSize) {
        m_sessionID = String::fromUTF8(sid.data());
        if (m_licenseCallback && m_initData) {
            m_licenseCallback(SharedBuffer::create(message, messageSize),
                              m_sessionID, false,
                              CDMInstanceSession::SuccessValue::Succeeded);
            m_licenseCallback = nullptr;
        }
        GST_DEBUG("CDM session message (%u bytes) sid=%s", (unsigned)messageSize, sid.c_str());
    };

    sinks.onSessionKeysChange = [this](const std::string& sid, bool additionalUsableKey,
                                       const std::vector<cdm::KeyInformation>& keysInfo) {
        UNUSED_PARAM(additionalUsableKey);
        m_sessionID = String::fromUTF8(sid.data());
        CDMInstanceSession::KeyStatusVector keyStatuses;
        for (const auto& keyInfo : keysInfo) {
            keyStatuses.append({
                // CDMKeyID = Ref<SharedBuffer> — raw key bytes per the EME/PSSH
                // spec go in straight, key_id_size bytes deep.
                SharedBuffer::create(reinterpret_cast<const unsigned char*>(keyInfo.key_id), keyInfo.key_id_size),
                fromCdmKeyStatus(keyInfo.status)
            });
        }
        if (m_licenseUpdateCallback) {
            m_licenseUpdateCallback(false, WTF::move(keyStatuses), std::nullopt,
                                    std::nullopt, CDMInstanceSession::SuccessValue::Succeeded);
            m_licenseUpdateCallback = nullptr;
        }
    };

    sinks.onExpirationChange = [this](const std::string& sid, cdm::Time newExpiry) {
        UNUSED_PARAM(sid);
        UNUSED_PARAM(newExpiry); // WebCore EME expiry isn't surfaced via this shim v1
    };

    sinks.onSessionClosed = [this](const std::string& sid) {
        UNUSED_PARAM(sid);
        m_sessionID = { };
    };
}

cdm::SessionType CDMInstanceSessionWidevine::toCdmSessionType(LicenseType)
{
    return cdm::SessionType::kTemporary;
}

cdm::InitDataType CDMInstanceSessionWidevine::toCdmInitDataType(const String& initDataType)
{
    if (initDataType == "cenc"_s)
        return cdm::InitDataType::kCenc;
    if (initDataType == "webm"_s)
        return cdm::InitDataType::kWebM;
    if (initDataType == "keyids"_s)
        return cdm::InitDataType::kKeyIds;
    // Note: v4.10-v10 Chromium CDMi only supports cenc/webm/keyids — sinf/skd
    // live in interface v11+ and would need a shim author to bump the code.
    return cdm::InitDataType::kCenc;
}

CDMMessageType CDMInstanceSessionWidevine::fromCdmMessageType(cdm::MessageType type)
{
    switch (type) {
    case cdm::MessageType::kLicenseRequest:
        return CDMMessageType::LicenseRequest;
    case cdm::MessageType::kLicenseRenewal:
        return CDMMessageType::LicenseRenewal;
    case cdm::MessageType::kIndividualizationRequest:
        return CDMMessageType::LicenseRequest; // EME treats individualization as license req
    default:
        return CDMMessageType::LicenseRequest;
    }
}

CDMKeyStatus CDMInstanceSessionWidevine::fromCdmKeyStatus(cdm::KeyStatus status)
{
    switch (status) {
    case cdm::KeyStatus::kUsable:
        return CDMKeyStatus::Usable;
    case cdm::KeyStatus::kExpired:
        return CDMKeyStatus::Expired;
    case cdm::KeyStatus::kReleased:
        return CDMKeyStatus::Released;
    case cdm::KeyStatus::kOutputRestricted:
        return CDMKeyStatus::OutputRestricted;
    case cdm::KeyStatus::kOutputDownscaled:
        return CDMKeyStatus::OutputDownscaled;
    case cdm::KeyStatus::kStatusPending:
        return CDMKeyStatus::StatusPending;
    case cdm::KeyStatus::kInternalError:
        return CDMKeyStatus::InternalError;
    default:
        return CDMKeyStatus::InternalError;
    }
}

void CDMInstanceSessionWidevine::requestLicense(LicenseType licenseType, KeyGroupingStrategy,
                                                const String& initDataType,
                                                Ref<SharedBuffer>&& initData,
                                                LicenseCallback&& callback)
{
    if (!module().load() || !module().impl()) {
        callback(WTF::move(initData), { }, false, CDMInstanceSession::SuccessValue::Failed);
        return;
    }

    m_licenseCallback = WTF::move(callback);
    m_initData = initData.copyRef();
    m_sessionID = { };

    const auto initDataSpan = initData->span();
    module().impl()->CreateSessionAndGenerateRequest(
        /*promise_id=*/1,
        toCdmSessionType(licenseType),
        toCdmInitDataType(initDataType),
        initDataSpan.data(),
        static_cast<uint32_t>(initDataSpan.size()));
}

void CDMInstanceSessionWidevine::updateLicense(const String& sessionId, LicenseType,
                                               Ref<SharedBuffer>&& response,
                                               LicenseUpdateCallback&& callback)
{
    if (!module().load() || !module().impl()) {
        callback(false, std::nullopt, std::nullopt, std::nullopt,
                 CDMInstanceSession::SuccessValue::Failed);
        return;
    }

    m_licenseUpdateCallback = WTF::move(callback);
    m_sessionID = sessionId;

    const auto responseSpan = response->span();
    module().impl()->UpdateSession(/*promise_id=*/2,
                                   m_sessionID.ascii().data(),
                                   static_cast<uint32_t>(m_sessionID.ascii().length()),
                                   responseSpan.data(),
                                   static_cast<uint32_t>(responseSpan.size()));
    // Asymmetric completion via OnSessionKeysChange / OnResolvePromise(2) /
    // OnRejectPromise(2).
}

void CDMInstanceSessionWidevine::loadSession(LicenseType, const String&, const String&,
                                             LoadSessionCallback&& callback)
{
    // v1: no persistent sessions exposed by this shim (L3 CDMs typically don't
    // honor persistent license storage across sessions).
    callback(std::nullopt, std::nullopt, std::nullopt,
             CDMInstanceSession::SuccessValue::Failed, CDMInstanceSession::SessionLoadFailure::Other);
}

void CDMInstanceSessionWidevine::closeSession(const String& sessionId,
                                              CloseSessionCallback&& callback)
{
    if (module().impl() && !sessionId.isEmpty())
        module().impl()->CloseSession(3,
                                      sessionId.ascii().data(),
                                      static_cast<uint32_t>(sessionId.ascii().length()));
    m_sessionID = { };
    callback();
}

void CDMInstanceSessionWidevine::removeSessionData(const String&, LicenseType,
                                                   RemoveSessionDataCallback&& callback)
{
    callback({ }, { }, CDMInstanceSession::SuccessValue::Succeeded);
}

void CDMInstanceSessionWidevine::storeRecordOfKeyUsage(const String&)
{
    // No-op — L3 CDMs on this shim aren't doing usage recording.
}

// ────────────────────────────────────────────────────────────────────────────
// CDMProxyWidevine
// ────────────────────────────────────────────────────────────────────────────
CDMProxyWidevine::CDMProxyWidevine(const String& keySystem)
{
    ASSERT(keySystem == widevineKeySystemAtom());
    m_keySystem = keySystem;
    GST_DEBUG_CATEGORY_INIT(webkitMediaWidevineDecryptDebugCategory,
                            "acreetion-widevine-proxy", 0, "AbruptionOS Widevine shim proxy");
}

DecryptionResult CDMProxyWidevine::decrypt(DecryptionContext& input, const GRefPtr<GstCaps>& inputCaps)
{
    UNUSED_PARAM(inputCaps);
    if (!module().impl()) {
        GST_ERROR("widevine module not loaded; cannot decrypt");
        return DecryptionResult::Failure;
    }

    // Map every GstBuffer into memory synchronously.
    GstMapInfo dataInfo, ivInfo, keyInfo, subInfo;
    if (!gst_buffer_map(input.dataBuffer, &dataInfo, GST_MAP_READ)) {
        GST_ERROR("cannot map data buffer");
        return DecryptionResult::Failure;
    }
    if (!gst_buffer_map(input.ivBuffer, &ivInfo, GST_MAP_READ)) {
        gst_buffer_unmap(input.dataBuffer, &dataInfo);
        GST_ERROR("cannot map iv buffer");
        return DecryptionResult::Failure;
    }
    if (!gst_buffer_map(input.keyIDBuffer, &keyInfo, GST_MAP_READ)) {
        gst_buffer_unmap(input.ivBuffer, &ivInfo);
        gst_buffer_unmap(input.dataBuffer, &dataInfo);
        GST_ERROR("cannot map key id buffer");
        return DecryptionResult::Failure;
    }

    // Subsample boundaries, if any
    std::vector<cdm::SubsampleEntry> subsamples;
    if (input.numSubsamples && input.subsamplesBuffer && gst_buffer_map(input.subsamplesBuffer, &subInfo, GST_MAP_READ)) {
        // The gst protection element feeds §subsamples as a mesh of GstSubsampleEntry structs of the
        // form {guint32 clear_bytes; guint32 cipher_bytes;} followed in order.
        subsamples.reserve(input.numSubsamples);
        const auto* raw = reinterpret_cast<const guint32*>(subInfo.data);
        for (size_t i = 0; i < input.numSubsamples && (i * 2 * sizeof(guint32)) < subInfo.size; ++i) {
            cdm::SubsampleEntry entry;
            entry.clear_bytes = raw[i * 2];
            entry.cipher_bytes = raw[i * 2 + 1];
            subsamples.emplace_back(entry);
        }
        gst_buffer_unmap(input.subsamplesBuffer, &subInfo);
    }

    // Build the InputBuffer_2 the CDM understands
    cdm::InputBuffer_2 cdmInput;
    std::memset(&cdmInput, 0, sizeof(cdmInput));
    cdmInput.data = dataInfo.data;
    cdmInput.data_size = static_cast<uint32_t>(dataInfo.size);
    cdmInput.encryption_scheme = cdm::EncryptionScheme::kCenc;
    cdmInput.key_id = keyInfo.data;
    cdmInput.key_id_size = static_cast<uint32_t>(keyInfo.size);
    cdmInput.iv = ivInfo.data;
    cdmInput.iv_size = static_cast<uint32_t>(ivInfo.size);
    cdmInput.subsamples = subsamples.data();
    cdmInput.num_subsamples = static_cast<uint32_t>(subsamples.size());
    cdmInput.timestamp = 0;

    // The CDM decrypts in place (for L3 CENC) using an allocated output buffer
    // handed back through the DecryptedBlock, but Chromium expects an Output_
    // Buffer it allocates via the host. Under v10 Decrypt(), the CDM writes
    // into the input data itself when no output is provided.
    WidevineBuffer* output = new WidevineBuffer(dataInfo.size);
    cdm::DecryptedBlock block;
    block.SetDecryptedBuffer(output);

    const auto status = module().impl()->Decrypt(cdmInput, &block);

    gst_buffer_unmap(input.keyIDBuffer, &keyInfo);
    gst_buffer_unmap(input.ivBuffer, &ivInfo);

    DecryptionResult result = DecryptionResult::Failure;
    if (status == cdm::Status::kSuccess) {
        // Copy decrypted output back into the GstBuffer (the same buffer we pass further down the pipeline)
        const auto* outBytes = output->Data();
        const auto outSize = output->Size();
        gst_buffer_unmap(input.dataBuffer, &dataInfo);
        gst_buffer_map(input.dataBuffer, &dataInfo, GST_MAP_WRITE);
        if (dataInfo.size >= outSize)
            std::memcpy(dataInfo.data, outBytes, outSize);
        gst_buffer_unmap(input.dataBuffer, &dataInfo);
        result = DecryptionResult::Success;
        GST_DEBUG("decrypt success (%u bytes out)", outSize);
    } else {
        gst_buffer_unmap(input.dataBuffer, &dataInfo);
        if (status == cdm::Status::kNoKey) {
            GST_DEBUG("decrypt: no key for this sample");
            result = DecryptionResult::NoKey;
        } else {
            GST_ERROR("CDM Decrypt error %d", static_cast<int>(status));
        }
    }

    delete output;
    return result;
}

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)
