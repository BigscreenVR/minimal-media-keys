#include "stdafx.h"
#include <wrl.h>
#include <thread>
#include <conio.h>
#include <process.h>
#include <propvarutil.h>
#include <strsafe.h>
#include <mfapi.h>
#include <mfmediaengine.h>

HANDLE hStdin;
DWORD fdwSaveOldMode;
DWORD cNumRead, fdwMode;

VOID ErrorExit(LPSTR);
bool KeyEventProc(KEY_EVENT_RECORD);

bool onExit = false;
bool readyToShutdown = false;

const uint32_t LOG_STRING_LEN = 65536;
void Log(const char* format, ...) {
    static char str[LOG_STRING_LEN];
    memset(str, 0, sizeof(LOG_STRING_LEN));
    va_list args;
    va_start(args, format);
    vsnprintf_s(str, sizeof(str), format, args);
    va_end(args);
    printf(str);
    printf("\n");
}

class MediaEngineNotify : public IMFMediaEngineNotify, IMFMediaEngineEMENotify {
    long m_cRef;

public:
    MediaEngineNotify() : m_cRef(0) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (__uuidof(IMFMediaEngineNotify) == riid) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
        } else if (__uuidof(IMFMediaEngineEMENotify) == riid) {
            *ppv = static_cast<IMFMediaEngineEMENotify*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) {
            delete this;
        }
        return cRef;
    }

    STDMETHODIMP EventNotify(DWORD meEvent, DWORD_PTR param1, DWORD param2) {
        Log("Event: %d %d hr=0x%X", meEvent, param1, param2);
        return S_OK;
    }

    // IMFMediaEngineEMENotify
    void STDMETHODCALLTYPE Encrypted(_In_reads_bytes_opt_(cb) const BYTE *pbInitData, _In_ DWORD cb, _In_ BSTR bstrInitDataType) {
        Log("MediaEngineNotify: Encrypted %ls", bstrInitDataType);
    }

    void STDMETHODCALLTYPE WaitingForKey(void) {
        Log("MediaEngineNotify: WaitingForKey");
    }
};

IMFMediaEngineClassFactory *factory = nullptr;
IMFMediaEngine *mediaEngine = nullptr;
IMFMediaKeys2 *mediaKeys = nullptr;
IMFMediaKeySession2 *mediaKeySession2 = nullptr;

class MediaKeySessionNotify : public IMFMediaKeySessionNotify2 {
    long m_cRef;

public:
    // Constructor for reactive license requests.
    MediaKeySessionNotify() : m_cRef(0) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (__uuidof(IMFMediaKeySessionNotify2) == riid)
        {
            *ppv = static_cast<IMFMediaKeySessionNotify2*>(this);
        }
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        AddRef();

        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release()
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
        {
            delete this;
        }
        return cRef;
    }

    void STDMETHODCALLTYPE KeyAdded(void) {
        Log("Key added");
    }

    void STDMETHODCALLTYPE KeyError(USHORT code, DWORD systemCode) {
        Log("KeyError");
    }

    void STDMETHODCALLTYPE KeyMessage(BSTR destinationUrl, const BYTE *message, DWORD cb) {
        Log("KeyMessage");
    }

    void STDMETHODCALLTYPE KeyMessage2(MF_MEDIAKEYSESSION_MESSAGETYPE eMessageType,
        _In_opt_  BSTR destinationURL,
        _In_reads_bytes_(cbMessage)  const BYTE *pbMessage,
        _In_  DWORD cbMessage) {
        Log("KeyMessage2 %d\n", eMessageType);

    }

    void STDMETHODCALLTYPE KeyStatusChange(void) {
        Log("KeyStatusChange");
    }
};

MediaKeySessionNotify *mediaKeySessionNotify = new MediaKeySessionNotify();

class MediaEngineNeedKeyNotify : public IMFMediaEngineNeedKeyNotify {
    long m_cRef;

public:
    MediaEngineNeedKeyNotify() : m_cRef(0) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (__uuidof(IMFMediaEngineNeedKeyNotify) == riid) {
            *ppv = static_cast<IMFMediaEngineNeedKeyNotify*>(this);
        }
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) {
            delete this;
        }
        return cRef;
    }

    void STDMETHODCALLTYPE NeedKey(_In_reads_bytes_opt_(cb) const BYTE *initData, _In_ DWORD cb) {
        Log("Needkey, len: %d", cb);
        Log("If we got here, then the key system appears to be working on this version of windows - press ESC to quit");

        HRESULT hr = mediaKeys->CreateSession2(MF_MEDIAKEYSESSION_TYPE_TEMPORARY, mediaKeySessionNotify, &mediaKeySession2);

        if (FAILED(hr = mediaKeySession2->GenerateRequest(L"cenc", initData, cb))) {
            Log("GenerateRequest failed: 0x%x\n", hr);
        }
    }
};

MediaEngineNotify *mediaEngineNotify = nullptr;
MediaEngineNeedKeyNotify *mediaEngineNeedKeyNotify = nullptr;

HRESULT CreateProtectedPlaybackSession() {
    HRESULT hr = S_OK;
    IMFAttributes *attributes = nullptr;

    mediaEngineNotify = new MediaEngineNotify();
    mediaEngineNeedKeyNotify = new MediaEngineNeedKeyNotify();

    if (FAILED(hr = MFStartup(MF_VERSION))) {
        Log("StartupMediaFoundation: 0x%x\n", hr);
        return hr;
    }

    if (FAILED(hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&factory)))) {
        Log("Failed to create CLSID_MFMediaEngineClassFactory 0x%x", hr);
        return hr;
    }

    if (FAILED(hr = MFCreateAttributes(&attributes, 1))) {
        Log("MFCreateAttributes failed 0x%x", hr);
        return hr;
    }

    if (FAILED(hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, (IMFMediaEngineEMENotify *)mediaEngineNotify))) {
        Log("Failed to set MF_MEDIA_ENGINE_CALLBACK 0x%x", hr);
        return hr;
    }

    // Key notification callback
    if (FAILED(hr = attributes->SetUnknown(MF_MEDIA_ENGINE_NEEDKEY_CALLBACK, mediaEngineNeedKeyNotify))) {
        Log("Failed to set MF_MEDIA_ENGINE_NEEDKEY_CALLBACK 0x%x", hr);
        return hr;
    }

    DWORD CONTENT_PROTECTION_FLAGS = MF_MEDIA_ENGINE_ENABLE_PROTECTED_CONTENT | MF_MEDIA_ENGINE_USE_PMP_FOR_ALL_CONTENT;
    if (FAILED(hr = attributes->SetUINT32(MF_MEDIA_ENGINE_CONTENT_PROTECTION_FLAGS, MF_MEDIA_ENGINE_ENABLE_PROTECTED_CONTENT | MF_MEDIA_ENGINE_USE_PMP_FOR_ALL_CONTENT))) {
        Log("Failed to set MF_MEDIA_ENGINE_CONTENT_PROTECTION_FLAGS 0x%x", hr);
        return hr;
    }

    
    if (FAILED(hr = attributes->SetUINT64(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_R8G8B8A8_UNORM))) {
        Log("Failed to set MF_MEDIA_ENGINE_PLAYBACK_HWND 0x%x", hr);
        return hr;
    }

    if (FAILED(hr = attributes->SetGUID(MF_MEDIA_ENGINE_COMPATIBILITY_MODE, MF_MEDIA_ENGINE_COMPATIBILITY_MODE_WIN10))) {
        Log("Failed to set MF_MEDIA_ENGINE_COMPATIBILITY_MODE 0x%x", hr);
        return hr;
    }

    DWORD MEDIA_ENGINE_FLAGS = 0; // MF_MEDIA_ENGINE_REAL_TIME_MODE | MF_MEDIA_ENGINE_DISABLE_LOCAL_PLUGINS;
    if (FAILED(hr = factory->CreateInstance(0, attributes, &mediaEngine))) {
        Log("Failed to Create MediaEngine Instance 0x%x", hr);
        return hr;
    }

    attributes->Release();

    // Tears of steel DASH from Microsoft PlayReady test site.
    const std::wstring streamManifestUrl = L"http://profficialsite.origin.mediaservices.windows.net/c51358ea-9a5e-4322-8951-897d640fdfd7/tearsofsteel_4k.ism/manifest(format=mpd-time-csf)";
    const std::wstring laUrl = L"http://test.playready.microsoft.com/service/rightsmanager.asmx?cfg=(persist:false,sl:150,playenablers:(786627D8-C2A6-44BE-8F88-08AE255B01A7,AE092501-A9E3-46F6-AFBE-628577DCDF55))";

    // Query for the class factory that allows us to create media keys.
    //
    // https://docs.microsoft.com/en-us/windows/win32/api/mfmediaengine/nn-mfmediaengine-imfmediaengineclassfactory2
    
    IMFMediaEngineClassFactory2 *keyFactory2 = nullptr;
    if (FAILED(hr = factory->QueryInterface(__uuidof(IMFMediaEngineClassFactory2), (void**)&keyFactory2))) {
        Log("Failed to query interface for IMFMediaEngineClassFactory2: %.2x", hr);
        return hr;
    }

    // Create CDM store directories in current working directory (or "cwd").
    const uint32_t CWD_BUFFER_LEN = 512;
    wchar_t cwd[CWD_BUFFER_LEN];
    ZeroMemory((void *)cwd, CWD_BUFFER_LEN * sizeof(wchar_t));
    DWORD cwdLen = GetCurrentDirectoryW(CWD_BUFFER_LEN, (LPWSTR)cwd);
    std::wstring cwdCdmStore(cwd, std::wstring::traits_type::length(cwd));
    cwdCdmStore.append(L"\\cdm");
    std::wstring cwdPrivateCdmStores(cwd, cwdLen);
    cwdPrivateCdmStores.append(L"\\cdm_private");

    BSTR defaultCdmStorePath = SysAllocStringLen(cwdCdmStore.c_str(), cwdCdmStore.size());
    BSTR inprivateCdmStorePath = SysAllocStringLen(cwdPrivateCdmStores.c_str(), cwdPrivateCdmStores.size());
    
    BOOL bResult;

    // Create the cdm store directory.
    // TODO: ignore error if it already exists.
    if (false == (bResult = CreateDirectory(cwdCdmStore.c_str(), NULL))) {
        Log("Could not create cdm store directory: 0x%X.", GetLastError());
    }

    // Create the cdm_private store directory.
    // TODO: ignore error if it already exists.
    if (false == (bResult = CreateDirectory(cwdPrivateCdmStores.c_str(), NULL))) {
        Log("Could not create cdm_private store directory: 0x%X.", GetLastError());
    }

    BSTR keySystem = L"com.microsoft.playready";

    IMFMediaEngineClassFactory3 *keyFactory3 = nullptr;
    if (FAILED(hr = factory->QueryInterface(__uuidof(IMFMediaEngineClassFactory3), (void**)&keyFactory3))) {
        Log("Failed to query interface for IMFMediaEngineClassFactory3: %.2x", hr);
        return hr;
    }

    /*
     * Create initialization props array
     *
     * See: https://w3c.github.io/encrypted-media/#dom-mediakeysystemconfiguration
     [{
        initDataTypes: ['keyids', 'cenc'],
        audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a"' }],
        videoCapabilities: [{ contentType: 'video/mp4; codecs="avc1"' }]
     }]
    */

    IPropertyStore *props[4];
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props[0]));
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props[1]));
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props[2]));
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props[3]));

    PROPVARIANT initDataTypes;
    hr = InitPropVariantFromStringAsVector(L"keyids; cenc", &initDataTypes);
    props[0]->SetValue(MF_EME_INITDATATYPES, initDataTypes);
    PropVariantClear(&initDataTypes);

    // Video
    IPropertyStore *videoCaps;
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&videoCaps));
    PROPVARIANT contentType;
    hr = InitPropVariantFromString(L"video/mp4; codecs=\"avc1\"", &contentType);
    videoCaps->SetValue(MF_EME_CONTENTTYPE, contentType);
    PropVariantClear(&contentType);

    PROPVARIANT videoCapabilitiesPropVar;
    PropVariantInit(&videoCapabilitiesPropVar);
    videoCapabilitiesPropVar.punkVal = videoCaps;
    videoCapabilitiesPropVar.vt = VT_ARRAY;

    PROPVARIANT videoCapabilities;
    hr = InitPropVariantFromPropVariantVectorElem(videoCapabilitiesPropVar, 1, &videoCapabilities);
    props[1]->SetValue(MF_EME_VIDEOCAPABILITIES, videoCapabilities);
    PropVariantClear(&videoCapabilities);

    // Audio
    IPropertyStore *audioCapsStore;
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&audioCapsStore));
    PROPVARIANT audioContentType;
    hr = InitPropVariantFromString(L"audio/mp4", &audioContentType);
    audioCapsStore->SetValue(MF_EME_CONTENTTYPE, audioContentType);
    PropVariantClear(&contentType);

    PROPVARIANT audioCapabilitiesPropVar;
    PropVariantInit(&audioCapabilitiesPropVar);
    audioCapabilitiesPropVar.punkVal = audioCapsStore;
    audioCapabilitiesPropVar.vt = VT_ARRAY;

    PROPVARIANT audioCapabilities;
    hr = InitPropVariantFromPropVariantVectorElem(audioCapabilitiesPropVar, 1, &audioCapabilities);
    props[2]->SetValue(MF_EME_AUDIOCAPABILITIES, audioCapabilities);
    PropVariantClear(&audioCapabilities);

    // Persistent state - do we need this?
    PROPVARIANT persistentState;
    hr = InitPropVariantFromString(L"required", &persistentState);
    props[3]->SetValue(MF_EME_PERSISTEDSTATE, persistentState);
    PropVariantClear(&persistentState);

    IMFMediaKeySystemAccess *keyAccess = nullptr;
    if (FAILED(hr = keyFactory3->CreateMediaKeySystemAccess(keySystem, props, 3, &keyAccess))) {
        Log("CreateMediaKeySystemAccess failed: %.2x", hr);
        return hr;
    }

    props[0]->Release();
    props[1]->Release();
    props[2]->Release();
    props[3]->Release();

    /*
     * Media keys stuff.
     */
    IPropertyStore *cdmValues;
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&cdmValues));

    // MF_EME_CDM_INPRIVATESTOREPATH
    PROPVARIANT inPrivateStorePath;
    hr = InitPropVariantFromString(inprivateCdmStorePath, &inPrivateStorePath);
    cdmValues->SetValue(MF_EME_CDM_INPRIVATESTOREPATH, inPrivateStorePath);
    PropVariantClear(&inPrivateStorePath);

    // MF_EME_CDM_STOREPATH
    PROPVARIANT storePath;
    hr = InitPropVariantFromString(defaultCdmStorePath, &storePath);
    cdmValues->SetValue(MF_EME_CDM_STOREPATH, storePath);
    PropVariantClear(&storePath);

    if (FAILED(hr = keyAccess->CreateMediaKeys(cdmValues, &mediaKeys))) {
        Log("CreateMediaKeys failed: %.2x", hr);
        return hr;
    }

    IMFMediaEngineEME *mediaEngineEME;
    if (FAILED(hr = mediaEngine->QueryInterface(__uuidof(IMFMediaEngineEME), (void**)&mediaEngineEME))) {
        Log("Failed to query interface for IMFMediaEngineEME 0x%x", hr);
        return hr;
    }
    // TODO: Is this the best place to do this?
    if (FAILED(hr = mediaEngineEME->SetMediaKeys(mediaKeys))) {
        Log("Failed to set media keys 0x%X", hr);
    }

    mediaEngineEME->Release();

    BSTR url = SysAllocStringLen(streamManifestUrl.data(), streamManifestUrl.size());
    hr = mediaEngine->SetSource(url);
    SysReleaseString(url);

    if (FAILED(hr)) {
        Log("Failed to set media engine source %.2x", hr);
        return hr;
    }

    return S_OK;
}

void CleanUp() {
    if (factory) {
        factory->Release();
    }

    if (mediaEngine) {
        mediaEngine->Release();
    }
}

void MediaThread(void *nothing) {
    HRESULT hr = CreateProtectedPlaybackSession();
    if (FAILED(hr)) {
        Log("Creating the protected playback session failed.");
        readyToShutdown = true;
        return;
    }

    // Mimic the game loop.
    while (true) {
        if (onExit) {
            CleanUp();
            break;
        }

        Sleep(11); // 90 fps (ish)
    }

    readyToShutdown = true;
}

// Console input handling from: https://docs.microsoft.com/en-us/windows/console/reading-input-buffer-events
int main() {
    HRESULT hr = S_OK;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) {
        ErrorExit("GetStdHandle");
    }

    if (!GetConsoleMode(hStdin, &fdwSaveOldMode)) {
        ErrorExit("GetConsoleMode");
    }

    fdwMode = ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    if (!SetConsoleMode(hStdin, fdwMode)) {
        ErrorExit("SetConsoleMode");
    }

    _beginthread(MediaThread, 0, nullptr);

    Log("Getting media keys -- press ESC to quit");
    
    INPUT_RECORD irInBuf[128];
    bool result = false;

    while (true) {
        if (readyToShutdown) {
            break;
        }

        if (!ReadConsoleInput(hStdin, irInBuf, 128, &cNumRead)) {
            ErrorExit("ReadConsoleInput");
        }

        for (DWORD i = 0; i < cNumRead; i++) {
            switch (irInBuf[i].EventType) {
                case KEY_EVENT: // keyboard input 
                    result = KeyEventProc(irInBuf[i].Event.KeyEvent);
                    break;
                case MOUSE_EVENT: // disregard focus events
                case WINDOW_BUFFER_SIZE_EVENT: // disregard focus events
                case FOCUS_EVENT:  // disregard focus events 
                case MENU_EVENT:   // disregard menu events 
                    break;
                default:
                    ErrorExit("Unknown event type");
                    break;
            }
        }
    }

    CoUninitialize();
    return hr;
}

VOID ErrorExit(LPSTR lpszMessage) {
    Log("%s\n", lpszMessage);
    SetConsoleMode(hStdin, fdwSaveOldMode);
    ExitProcess(0);
}

bool KeyEventProc(KEY_EVENT_RECORD ker) {
    if (!ker.bKeyDown && ker.wVirtualKeyCode == VK_ESCAPE) { // "esc"
        onExit = true;
        return true;
    }
    return false;
}
