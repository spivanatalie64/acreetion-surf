/*
 * CdmModule.cpp — AcreetionOS Widevine shim core implementation.
 * Exact Host_10 bridge against the Kodi-vendored CDMi v10 header.
 */

#include "CdmModule.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace WebKitWidevine {

// ───────────────────────────────────────────────────────────────────────────
// Library discovery
// ───────────────────────────────────────────────────────────────────────────
static bool regularFile(const std::string& path) {
    struct stat st = {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::vector<std::string> listVersionDirs(const std::string& root) {
    std::vector<std::string> out;
    DIR* d = ::opendir(root.c_str());
    if (!d) return out;
    struct dirent* de = nullptr;
    while ((de = ::readdir(d)) != nullptr) {
        if (de->d_type == DT_DIR && de->d_name[0] != '.')
            out.emplace_back(de->d_name);
    }
    ::closedir(d);
    std::sort(out.rbegin(), out.rend()); // newest version first
    return out;
}

std::string findWidevineCdmLibrary() {
    if (const char* env = ::getenv("ACREETION_WIDEVINE_CDM_PATH"))
        if (regularFile(env)) return env;

    const char* home = ::getenv("HOME");
    std::vector<std::string> roots {
        std::string(home ? home : "/root") + "/.config/vivaldi/WidevineCdm/",
        "/opt/vivaldi/WidevineCdm/",
        "/opt/google/chrome/WidevineCdm/",
        "/opt/chromium/WidevineCdm/",
        "/usr/lib/chromium/WidevineCdm/",
        "/usr/lib/brave-browser/WidevineCdm/",
        "/usr/lib/opera/WidevineCdm/",
    };
    for (const auto& root : roots) {
        for (const auto& version : listVersionDirs(root)) {
            const std::string withPlatform = root + version
                + "/_platform_specific/linux_x64/libwidevinecdm.so";
            if (regularFile(withPlatform))
                return withPlatform;
            const std::string plain = root + version + "/libwidevinecdm.so";
            if (regularFile(plain))
                return plain;
        }
    }
    return {};
}

// ───────────────────────────────────────────────────────────────────────────
// CDM loading
// ───────────────────────────────────────────────────────────────────────────
WidevineCdmModule& WidevineCdmModule::shared() {
    static WidevineCdmModule module;
    return module;
}

bool WidevineCdmModule::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_loaded)
        return true;

    const std::string path = findWidevineCdmLibrary();
    if (path.empty())
        return false;

    m_handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (!m_handle)
        return false;

    auto createCdmInstance = reinterpret_cast<void* (*)(int, const char*, uint32_t, GetCdmHostFunc, void*)>(
        ::dlsym(m_handle, "CreateCdmInstance"));
    m_initialize = reinterpret_cast<void(*)()>(::dlsym(m_handle, "InitializeCdmModule_4"));
    auto verifyHost = reinterpret_cast<VerifyCdmHostFunc>(::dlsym(m_handle, "VerifyCdmHost_0"));
    auto getVersion = reinterpret_cast<GetCdmVersionFunc>(::dlsym(m_handle, "GetCdmVersion"));

    if (!createCdmInstance || !m_initialize) {
        ::dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_initialize();

    // Empty host file list — CDM 4.10's VerifyCdmHost_0 returns success when
    // no files need verification (this is how Firefox loads L3 CDMs too).
    if (verifyHost && verifyHost(nullptr, 0) != 1) {
        ::dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_hostSinks = HostSinks {};
    m_host = std::make_unique<WidevineHost>(std::move(m_hostSinks));

    m_cdm = createCdmInstance(
        kCdmInterfaceVersion, kWidevineKeySystem,
        static_cast<uint32_t>(std::strlen(kWidevineKeySystem)),
        [](int host_interface_version, void* user_data) -> void* {
            // Chromium host-selection callback: the shim owns a single Host_10
            // object and hands that back for v10; rejects otherwise.
            if (host_interface_version == kCdmInterfaceVersion)
                return static_cast<WebKitWidevine::WidevineHost*>(user_data);
            return nullptr;
        },
        m_host.get());

    if (!m_cdm) {
        m_host.reset();
        ::dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }

    if (getVersion)
        m_version = getVersion();

    m_loaded = true;
    return true;
}

// ───────────────────────────────────────────────────────────────────────────
// WidevineFileIO
// ───────────────────────────────────────────────────────────────────────────
static std::string widevineFileIOPath(const char* name, uint32_t size) {
    std::string base = ::getenv("HOME") ? ::getenv("HOME") : "/tmp";
    base += "/.acreetion/widevine-cdm/";
    ::mkdir(base.c_str(), 0755);
    return base + std::string(name, size);
}

void WidevineFileIO::Open(const char* file_name, uint32_t file_name_size) {
    m_fileName = widevineFileIOPath(file_name, file_name_size);
    m_file = ::fopen(m_fileName.c_str(), "rb");
    if (!m_file) {
        m_client->OnOpenComplete(cdm::FileIOClient::Status::kError);
        return;
    }
    ::fseek(m_file, 0, SEEK_END);
    const auto size = ::ftell(m_file);
    ::fseek(m_file, 0, SEEK_SET);
    m_bufferRead.resize(size);
    ::fread(m_bufferRead.data(), 1, size, m_file);
    m_client->OnOpenComplete(cdm::FileIOClient::Status::kSuccess);
}

void WidevineFileIO::Read() {
    if (m_bufferRead.empty()) {
        m_client->OnReadComplete(cdm::FileIOClient::Status::kSuccess, nullptr, 0);
        return;
    }
    m_client->OnReadComplete(cdm::FileIOClient::Status::kSuccess, m_bufferRead.data(), m_bufferRead.size());
    m_bufferRead.clear();
}

void WidevineFileIO::Write(const uint8_t* data, uint32_t data_size) {
    if (m_file) {
        ::fclose(m_file);
        m_file = nullptr;
    }
    m_file = ::fopen(m_fileName.c_str(), "wb");
    const bool ok = m_file && ::fwrite(data, 1, data_size, m_file) == data_size;
    if (m_file) {
        ::fclose(m_file);
        m_file = nullptr;
    }
    m_client->OnWriteComplete(ok ? cdm::FileIOClient::Status::kSuccess : cdm::FileIOClient::Status::kError);
}

// NOTE: cdm::FileIO has no Close() virtual in the v10 interface; the CDM
// releases the FileIO object with delete. Kept pointer lifecycle rather than
// a Close hook.

// ───────────────────────────────────────────────────────────────────────────
// WidevineHost — Host_10 interface bridge (exact vtable)
// ───────────────────────────────────────────────────────────────────────────
cdm::Buffer* WidevineHost::Allocate(uint32_t capacity) {
    return new WidevineBuffer(capacity);
}

void WidevineHost::SetTimer(int64_t delay_ms, void*) {
    // v1 limitation: license-renewal timers (CDM → Host::OnSessionMessage with
    // kLicenseRenewal arriving automatically) would normally require the host
    // to fire cdm->TimerExpired(context) after delay_ms. Without it, licenses
    // with strict renewals may stop decoding after their initial window.
    // Streaming EME flows generally renew through UpdateSession — v2 TODO.
    (void)delay_ms;
}

cdm::Time WidevineHost::GetCurrentWallTime() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

void WidevineHost::OnInitialized(bool success) {
    if (m_sinks.onInitialized) m_sinks.onInitialized(success);
}

void WidevineHost::OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus key_status) {
    if (m_sinks.onResolveKeyStatus) m_sinks.onResolveKeyStatus(promise_id, key_status);
}

void WidevineHost::OnResolveNewSessionPromise(uint32_t promise_id, const char* session_id, uint32_t session_id_size) {
    if (m_sinks.onResolveNewSession)
        m_sinks.onResolveNewSession(promise_id, std::string(session_id, session_id_size));
}

void WidevineHost::OnResolvePromise(uint32_t promise_id) {
    if (m_sinks.onResolve) m_sinks.onResolve(promise_id);
}

void WidevineHost::OnRejectPromise(uint32_t promise_id, cdm::Exception exception, uint32_t system_code,
                                   const char* error_message, uint32_t) {
    if (m_sinks.onReject) m_sinks.onReject(promise_id, exception, system_code, error_message);
}

void WidevineHost::OnSessionMessage(const char* session_id, uint32_t session_id_size,
                                    cdm::MessageType message_type, const char* message, uint32_t message_size) {
    if (m_sinks.onSessionMessage)
        m_sinks.onSessionMessage(std::string(session_id, session_id_size), message_type,
                                 reinterpret_cast<const uint8_t*>(message), message_size);
}

void WidevineHost::OnSessionKeysChange(const char* session_id, uint32_t session_id_size,
                                       bool has_additional_usable_key, const cdm::KeyInformation* keys_info,
                                       uint32_t keys_info_count) {
    if (m_sinks.onSessionKeysChange)
        m_sinks.onSessionKeysChange(std::string(session_id, session_id_size), has_additional_usable_key,
                                    keys_info ? std::vector<cdm::KeyInformation>(keys_info, keys_info + keys_info_count)
                                              : std::vector<cdm::KeyInformation>());
}

void WidevineHost::OnExpirationChange(const char* session_id, uint32_t session_id_size,
                                      cdm::Time new_expiry_time) {
    if (m_sinks.onExpirationChange)
        m_sinks.onExpirationChange(std::string(session_id, session_id_size), new_expiry_time);
}

void WidevineHost::OnSessionClosed(const char* session_id, uint32_t session_id_size) {
    if (m_sinks.onSessionClosed)
        m_sinks.onSessionClosed(std::string(session_id, session_id_size));
}

void WidevineHost::SendPlatformChallenge(const char*, uint32_t, const char*, uint32_t) {
    // Software L3 CDM on a personal laptop — no HDCP challenge for local players.
}

void WidevineHost::EnableOutputProtection(uint32_t) {
    // Software rendering.
}

void WidevineHost::QueryOutputProtectionStatus() {
    // Software path: report as protected/no-op.
}

void WidevineHost::OnDeferredInitializationDone(cdm::StreamType, cdm::Status) {
    // We don't implement a CDM-integrated decoder; software decrypt only.
}

cdm::FileIO* WidevineHost::CreateFileIO(cdm::FileIOClient* client) {
    return new WidevineFileIO(client);
}

void WidevineHost::RequestStorageId(uint32_t) {
    // L3-bound feature; skip.
}

} // namespace WebKitWidevine
