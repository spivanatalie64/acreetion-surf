/*
 * CDMWidevine.h — Chromium Widevine CDM shim for WebKitGTK (EME/com.widevine.alpha).
 * Mirrors the in-tree Thunder/OpenCDM factory *shape* (CDMFactoryThunder etc.)
 * but points the decrypt path at Chromium's libwidevinecdm.so via the public
 * CDMi v10 interface vendored under `widevine/widevine-cdm/`.
 *
 * NOTE deliberately-honest limitation (v1): CDM callbacks dispatch through ONE
 * module-global HostSinks object owned by WebKitWidevine::WidevineCdmModule.
 * Only the most recently opened session receives callbacks; overlapping
 * sessions from multiple MediaKeys objects process serially. This is fine for
 * one-video-per-page playback, which is the use this shim targets.
 */

#pragma once

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)

#include "CDMInstanceSession.h"
#include "CDMPrivate.h"
#include "CDMProxy.h"
#include "GStreamerEMEUtilities.h"
#include "MediaKeyStatus.h"
#include "SharedBuffer.h"
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

#include "widevine/CdmModule.h"

namespace WebCore {

class CDMFactoryWidevine final : public CDMFactory, public CDMProxyFactory {
    WTF_MAKE_TZONE_ALLOCATED(CDMFactoryWidevine);
public:
    static CDMFactoryWidevine& singleton();

    // CDMFactory
    std::unique_ptr<CDMPrivate> createCDM(const String& keySystem, const String& mediaKeysHashSalt, const CDMPrivateClient&) final;
    bool supportsKeySystem(const String&) final;

    // CDMProxyFactory
    RefPtr<CDMProxy> createCDMProxy(const String&) final;

private:
    friend class NeverDestroyed<CDMFactoryWidevine>;
    CDMFactoryWidevine() = default;
};

class CDMPrivateWidevine final : public CDMPrivate {
    WTF_MAKE_TZONE_ALLOCATED(CDMPrivateWidevine);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(CDMPrivateWidevine);
public:
    CDMPrivateWidevine() = default;
    virtual ~CDMPrivateWidevine() = default;

    Vector<String> supportedInitDataTypes() const final;
    bool supportsConfiguration(const CDMKeySystemConfiguration&) const final;
    bool supportsConfigurationWithRestrictions(const CDMKeySystemConfiguration&, const CDMRestrictions&) const final;
    bool supportsSessionTypeWithConfiguration(const CDMSessionType&, const CDMKeySystemConfiguration&) const final;
    Vector<String> supportedRobustnesses() const final;
    CDMRequirement distinctiveIdentifiersRequirement(const CDMKeySystemConfiguration&, const CDMRestrictions&) const final;
    CDMRequirement persistentStateRequirement(const CDMKeySystemConfiguration&, const CDMRestrictions&) const final;
    bool distinctiveIdentifiersAreUniquePerOriginAndClearable(const CDMKeySystemConfiguration&) const final;
    RefPtr<CDMInstance> createInstance() final;
    void loadAndInitialize() final;
    bool supportsServerCertificates() const final;
    bool supportsSessions() const final;
    bool supportsInitData(const String&, const SharedBuffer&) const final;
    RefPtr<SharedBuffer> sanitizeResponse(const SharedBuffer&) const final;
    std::optional<String> sanitizeSessionId(const String&) const final;
};

class CDMInstanceWidevine final : public CDMInstanceProxy {
    WTF_MAKE_TZONE_ALLOCATED(CDMInstanceWidevine);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(CDMInstanceWidevine);
public:
    CDMInstanceWidevine();

    // CDMInstance
    ImplementationType implementationType() const final;
    void initializeWithConfiguration(const CDMKeySystemConfiguration&, AllowDistinctiveIdentifiers,
                                     AllowPersistentState, SuccessCallback&&) final;
    void setServerCertificate(Ref<SharedBuffer>&&, SuccessCallback&&) final;
    void setStorageDirectory(const String&) final;
    const String& keySystem() const LIFETIME_BOUND final;
    RefPtr<CDMInstanceSession> createSession() final;

    virtual ~CDMInstanceWidevine() = default;

private:
    String m_keySystem;
};

class CDMInstanceSessionWidevine final : public CDMInstanceSessionProxy {
public:
    explicit CDMInstanceSessionWidevine(CDMInstanceWidevine&);
    ~CDMInstanceSessionWidevine() override;

    // CDMInstanceSession
    void requestLicense(LicenseType, KeyGroupingStrategy, const String& initDataType,
                        Ref<SharedBuffer>&& initData, LicenseCallback&&) final;
    void updateLicense(const String&, LicenseType, Ref<SharedBuffer>&&, LicenseUpdateCallback&&) final;
    void loadSession(LicenseType, const String&, const String&, LoadSessionCallback&&) final;
    void closeSession(const String&, CloseSessionCallback&&) final;
    void removeSessionData(const String&, LicenseType, RemoveSessionDataCallback&&) final;
    void storeRecordOfKeyUsage(const String&) final;

    bool isValid() const { return !m_sessionID.isEmpty(); }

private:
    static cdm::SessionType toCdmSessionType(LicenseType);
    static cdm::InitDataType toCdmInitDataType(const String&);
    static CDMMessageType fromCdmMessageType(cdm::MessageType);
    static CDMKeyStatus fromCdmKeyStatus(cdm::KeyStatus);

    void setupHostSinks();

    WeakPtr<CDMInstanceWidevine> m_cdmInstance;
    String m_sessionID;
    RefPtr<SharedBuffer> m_initData;
    LicenseCallback m_licenseCallback;
    LicenseUpdateCallback m_licenseUpdateCallback;
};

class CDMProxyWidevine final : public CDMProxy, public CanMakeWeakPtr<CDMProxyWidevine, WeakPtrFactoryInitialization::Eager> {
public:
    CDMProxyWidevine(const String& keySystem);

    struct DecryptionContext {
        GstBuffer* keyIDBuffer;
        GstBuffer* ivBuffer;
        GstBuffer* dataBuffer;
        GstBuffer* subsamplesBuffer;
        size_t numSubsamples;
        WeakPtr<CDMProxyDecryptionClient> cdmProxyDecryptionClient;
    };

    DecryptionResult decrypt(DecryptionContext&, const GRefPtr<GstCaps>& inputCaps);
    const String& keySystem() const LIFETIME_BOUND { return m_keySystem; }

private:
    String m_keySystem;
};

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)
