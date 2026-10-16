/*
 * CdmModule.h — AcreetionOS Widevine shim core
 * Apache-2.0; built around Chromium's public CDMi v10 interface.
 *
 * Loads Chromium's libwidevinecdm.so (ABI-verified against 4.10.3112.0:
 * InitializeCdmModule_4 · DeinitializeCdmModule · CreateCdmInstance ·
 * VerifyCdmHost_0 · GetCdmVersion) and implements the *host* side of the
 * CDMi v10 interface, mapping CDM callbacks into function sinks.
 *
 * Vendored header: widevine-cdm/content_decryption_module.{h,_export.h}
 * taken verbatim from Kodi's inputstream.adaptive (clean C++, no MFBT deps).
 *
 * WebCore-free on purpose so interface bridging stays testable standalone.
 */

#pragma once

#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Chromium CDMi v10 interface header (Apache-2.0, clean C++ vendoring)
#include "widevine-cdm/content_decryption_module.h"

extern "C" {
using VerifyCdmHostFunc = int (*)(const void* files, size_t num_files);
using GetCdmVersionFunc = const char* (*)();
} // namespace

namespace WebKitWidevine {

inline constexpr const char* kWidevineKeySystem = "com.widevine.alpha";
inline constexpr int kCdmInterfaceVersion = 10;

// ── Host callback sinks (CDM→shim bridges) ─────────────────────────────────
struct HostSinks {
    std::function<void(bool)>                                                onInitialized;
    std::function<void(uint32_t, cdm::KeyStatus)>                            onResolveKeyStatus;
    std::function<void(uint32_t, const std::string&)>                        onResolveNewSession;
    std::function<void(uint32_t)>                                            onResolve;
    std::function<void(uint32_t, cdm::Exception, uint32_t, const char*)>     onReject;
    std::function<void(const std::string&, cdm::MessageType, const uint8_t*, size_t)> onSessionMessage;
    std::function<void(const std::string&, bool, const std::vector<cdm::KeyInformation>&)> onSessionKeysChange;
    std::function<void(const std::string&, cdm::Time)>                       onExpirationChange;
    std::function<void(const std::string&)>                                  onSessionClosed;
};

// ── Buffer / FileIO impls (exact CDMi v10 vtables) ─────────────────────────
//
// cdm::Buffer:
//   virtual void Destroy() = 0;
//   virtual uint32_t Capacity() const = 0;
//   virtual uint8_t* Data() = 0;
//   virtual void SetSize(uint32_t size) = 0;
//   virtual uint32_t Size() const = 0;
class WidevineBuffer final : public cdm::Buffer {
public:
    explicit WidevineBuffer(uint32_t capacity)
        : m_data(new uint8_t[capacity])
        , m_capacity(capacity)
        , m_size(capacity)
    {
        std::memset(m_data, 0, capacity);
    }
    ~WidevineBuffer() override { delete[] m_data; }

    void Destroy() override { delete this; }
    uint32_t Capacity() const override { return m_capacity; }
    uint8_t* Data() override { return m_data; }
    void SetSize(uint32_t size) override { m_size = size; }
    uint32_t Size() const override { return m_size; }

private:
    uint8_t* m_data;
    uint32_t m_capacity;
    uint32_t m_size;
};

// FileIOClient callbacks carry cdm::Status (NOT bool — v10 contract).
class WidevineFileIO final : public cdm::FileIO {
public:
    explicit WidevineFileIO(cdm::FileIOClient* client) : m_client(client) {}
    ~WidevineFileIO() override {}

    // Mirror of cdm::FileIO (Open/Read/Write/Close, same vtable shape).
    void Open(const char* file_name, uint32_t file_name_size) override;
    void Read() override;
    void Write(const uint8_t* data, uint32_t data_size) override;
    void Close() override;

private:
    cdm::FileIOClient* m_client;
    FILE* m_file = nullptr;
    std::vector<uint8_t> m_bufferRead;
    std::string m_fileName;
};

// ── WidevineHost — implements Host_10 verbatim ─────────────────────────────
class WidevineHost final : public cdm::Host_10 {
public:
    explicit WidevineHost(HostSinks&& sinks) : m_sinks(std::move(sinks)) {}

    // Host_10 vtable (exact)
    cdm::Buffer* Allocate(uint32_t capacity) override;
    void SetTimer(int64_t delay_ms, void* context) override;
    cdm::Time GetCurrentWallTime() override;
    void OnInitialized(bool success) override;
    void OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus key_status) override;
    void OnResolveNewSessionPromise(uint32_t promise_id, const char* session_id, uint32_t session_id_size) override;
    void OnResolvePromise(uint32_t promise_id) override;
    void OnRejectPromise(uint32_t promise_id, cdm::Exception exception, uint32_t system_code,
                         const char* error_message, uint32_t error_message_size) override;
    void OnSessionMessage(const char* session_id, uint32_t session_id_size, cdm::MessageType message_type,
                          const char* message, uint32_t message_size) override;
    void OnSessionKeysChange(const char* session_id, uint32_t session_id_size, bool has_additional_usable_key,
                             const cdm::KeyInformation* keys_info, uint32_t keys_info_count) override;
    void OnExpirationChange(const char* session_id, uint32_t session_id_size, cdm::Time new_expiry_time) override;
    void OnSessionClosed(const char* session_id, uint32_t session_id_size) override;
    void SendPlatformChallenge(const char* service_id, uint32_t service_id_size,
                               const char* challenge, uint32_t challenge_size) override;
    void EnableOutputProtection(uint32_t desired_protection_mask) override;
    void QueryOutputProtectionStatus() override;
    void OnDeferredInitializationDone(cdm::StreamType stream_type, cdm::Status decoder_status) override;
    cdm::FileIO* CreateFileIO(cdm::FileIOClient* client) override;
    void RequestStorageId(uint32_t version) override;

    HostSinks& sinks() { return m_sinks; }

private:
    HostSinks m_sinks;
};

// ── Owns the dlopen'd CDM + the live host (one per key-system) ─────────────
class WidevineCdmModule {
public:
    static WidevineCdmModule& shared();

    // Loads the CDM library. Returns true on success. Idempotent.
    bool load();
    bool available() const { return m_loaded; }
    cdm::ContentDecryptionModule_10* impl() const {
        return reinterpret_cast<cdm::ContentDecryptionModule_10*>(m_cdm);
    }
    WidevineHost* host() { return m_host.get(); }
    HostSinks& sinks() { return m_hostSinks; }

    // optional version string when available
    const char* version() const { return m_version ? m_version : "unknown"; }

private:
    WidevineCdmModule() = default;

    bool m_loaded = false;
    void* m_handle = nullptr;
    void* m_cdm = nullptr;
    void (*m_initialize)() = nullptr;
    std::unique_ptr<WidevineHost> m_host;
    HostSinks m_hostSinks;
    const char* m_version = nullptr;
    std::mutex m_mutex;
};

std::string findWidevineCdmLibrary();

} // namespace WebKitWidevine
