#pragma once

// IMPORTANTE: inclui o offsets.h da raiz (namespace Offsets::... hardcoded)
// como fallback — o OffsetManager sobrescreve os inline uintptr_t em runtime
// quando consegue baixar o JSON do dumper.
#include "../offsets.h"

#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>

// --------------------------------------------------------------------------
// OffsetManager — offsets dinâmicos do Roblox
//
// Fonte primária: https://offsets.imtheo.lol/Offsets.json  (RbxDumperV2)
// Fonte fallback: arquivo "offsets.json" ao lado do .exe
// Último recurso: valores hardcoded em offsets.h (raiz)
//
// CORREÇÃO IMPORTANTE: alguns offsets válidos são 0 (ex: Instance::Name = 0,
// RenderView::* = 0). Por isso o ApplyToNamespace NÃO pode usar "!= 0" para
// decidir se sobrescreve — precisa aplicar sempre que o campo foi encontrado
// no JSON, mesmo que o valor seja 0. Caso contrário, o fallback hardcoded
// permanece e quebra a leitura de nomes (FindChild não acha Ball/Bools/etc).
// --------------------------------------------------------------------------

struct RobloxOffsets {
    uintptr_t FakeDataModel_Pointer       = 0;
    uintptr_t FakeDataModel_RealDataModel = 0;

    uintptr_t DataModel_Workspace = 0;
    uintptr_t DataModel_PlaceId   = 0;
    uintptr_t DataModel_GameId    = 0;

    uintptr_t Workspace_CurrentCamera       = 0;
    uintptr_t Workspace_DistributedGameTime = 0;

    uintptr_t Camera_Position     = 0;
    uintptr_t Camera_Rotation     = 0;
    uintptr_t Camera_FieldOfView  = 0;
    uintptr_t Camera_ViewportSize = 0;

    uintptr_t Player_LocalPlayer   = 0;
    uintptr_t Player_ModelInstance = 0;
    uintptr_t Player_UserId        = 0;
    uintptr_t Player_DisplayName   = 0;

    uintptr_t Humanoid_Health           = 0;
    uintptr_t Humanoid_MaxHealth        = 0;
    uintptr_t Humanoid_HumanoidRootPart = 0;
    uintptr_t Humanoid_Walkspeed        = 0;
    uintptr_t Humanoid_JumpPower        = 0;

    uintptr_t BasePart_Primitive                = 0;
    uintptr_t Primitive_Position                = 0;
    uintptr_t Primitive_Size                    = 0;
    uintptr_t Primitive_Rotation                = 0;
    uintptr_t Primitive_AssemblyLinearVelocity  = 0;
    uintptr_t Primitive_AssemblyAngularVelocity = 0;

    uintptr_t Instance_NameContainer   = 0;
    uintptr_t Instance_Name            = 0;
    uintptr_t Instance_ChildrenStart   = 0;
    uintptr_t Instance_ChildrenEnd     = 0;
    uintptr_t Instance_ClassDescriptor = 0;
    uintptr_t Instance_ClassName       = 0;
    uintptr_t Instance_Parent          = 0;

    uintptr_t Misc_Value        = 0;
    uintptr_t Model_PrimaryPart = 0;

    uintptr_t VisualEngine_Pointer    = 0;
    uintptr_t VisualEngine_ViewMatrix = 0;

    bool IsValid() const {
        return FakeDataModel_Pointer != 0 && DataModel_Workspace != 0;
    }
};

class OffsetManager {
public:
    static OffsetManager& Get() {
        static OffsetManager instance;
        return instance;
    }

    bool Fetch();
    bool FetchFromHttp();
    bool LoadFromFile();

    const RobloxOffsets& GetOffsets()  const { return m_offsets;  }
    const std::string&   GetVersion()  const { return m_version;  }
    const std::string&   GetStatus()   const { return m_status;   }
    const std::string&   GetDumpedAt() const { return m_dumpedAt; }

private:
    OffsetManager() = default;
    OffsetManager(const OffsetManager&)            = delete;
    OffsetManager& operator=(const OffsetManager&) = delete;

    bool ParseJson(const std::string& json);
    void ApplyToNamespace();

    static bool  ExtractCategory(const std::string& json,
                                 const std::string& cat,
                                 std::string& outBody);
    static bool  ExtractNumber(const std::string& body,
                               const std::string& key,
                               uintptr_t& out);
    static bool  ExtractString(const std::string& json,
                               const std::string& key,
                               std::string& out);
    static std::string HttpGet(const std::wstring& host,
                               const std::wstring& path);
    static std::string GetExeDir();

    RobloxOffsets m_offsets;
    std::string   m_version  = "Unknown";
    std::string   m_status   = "NONE";
    std::string   m_dumpedAt = "-";
};

// ==========================================================================
// IMPLEMENTAÇÃO
// ==========================================================================

inline std::string OffsetManager::GetExeDir()
{
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* last = strrchr(buf, '\\');
    if (last) *(last + 1) = '\0';
    return buf;
}

inline std::string OffsetManager::HttpGet(const std::wstring& host,
                                          const std::wstring& path)
{
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest)
        {
            LPCWSTR headers = L"Accept-Encoding: identity\r\n";
            if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD statusCode = 0;
                DWORD sz         = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                                    &sz, WINHTTP_NO_HEADER_INDEX);

                if (statusCode == 200)
                {
                    DWORD avail = 0;
                    do {
                        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0)
                            break;
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0)
                            break;
                        chunk.resize(read);
                        result += chunk;
                    } while (avail > 0);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

inline bool OffsetManager::ExtractCategory(const std::string& json,
                                           const std::string& cat,
                                           std::string& outBody)
{
    std::string needle = "\"" + cat + "\":{";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;

    size_t start = pos + needle.size();
    int depth = 1;
    size_t i = start;
    while (i < json.size() && depth > 0)
    {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return false;
    outBody = json.substr(start, i - start - 1);
    return true;
}

inline bool OffsetManager::ExtractNumber(const std::string& body,
                                         const std::string& key,
                                         uintptr_t& out)
{
    std::string needle = "\"" + key + "\":";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;

    size_t i = pos + needle.size();
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;

    if (i + 1 < body.size() && body[i] == '0' && (body[i+1] == 'x' || body[i+1] == 'X'))
    {
        i += 2;
        uintptr_t v = 0;
        while (i < body.size() && std::isxdigit((unsigned char)body[i]))
        {
            char c = body[i++];
            v = v * 16 + (uintptr_t)(std::isdigit((unsigned char)c) ? (c - '0')
                                                                  : (std::tolower((unsigned char)c) - 'a' + 10));
        }
        out = v;
        return true;
    }

    uintptr_t v = 0;
    bool any = false;
    while (i < body.size() && std::isdigit((unsigned char)body[i]))
    {
        v = v * 10 + (uintptr_t)(body[i++] - '0');
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

inline bool OffsetManager::ExtractString(const std::string& json,
                                         const std::string& key,
                                         std::string& out)
{
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    size_t start = pos + needle.size();
    size_t end   = json.find('"', start);
    if (end == std::string::npos) return false;
    out = json.substr(start, end - start);
    return true;
}

inline bool OffsetManager::ParseJson(const std::string& json)
{
    if (json.empty()) return false;

    ExtractString(json, "Roblox Version", m_version);
    ExtractString(json, "Dumped At",      m_dumpedAt);

    RobloxOffsets o;
    std::string body;

#define PULL(CAT, KEY, FIELD) \
    if (ExtractCategory(json, CAT, body)) ExtractNumber(body, KEY, o.FIELD);

    PULL("FakeDataModel", "Pointer",       FakeDataModel_Pointer)
    PULL("FakeDataModel", "RealDataModel", FakeDataModel_RealDataModel)

    PULL("DataModel", "Workspace", DataModel_Workspace)
    PULL("DataModel", "PlaceId",   DataModel_PlaceId)
    PULL("DataModel", "GameId",    DataModel_GameId)

    PULL("Workspace", "CurrentCamera",       Workspace_CurrentCamera)
    PULL("Workspace", "DistributedGameTime", Workspace_DistributedGameTime)

    PULL("Camera", "Position",     Camera_Position)
    PULL("Camera", "Rotation",     Camera_Rotation)
    PULL("Camera", "FieldOfView",  Camera_FieldOfView)
    PULL("Camera", "ViewportSize", Camera_ViewportSize)

    PULL("Player", "LocalPlayer",   Player_LocalPlayer)
    PULL("Player", "ModelInstance", Player_ModelInstance)
    PULL("Player", "UserId",        Player_UserId)
    PULL("Player", "DisplayName",   Player_DisplayName)

    PULL("Humanoid", "Health",           Humanoid_Health)
    PULL("Humanoid", "MaxHealth",        Humanoid_MaxHealth)
    PULL("Humanoid", "HumanoidRootPart", Humanoid_HumanoidRootPart)
    PULL("Humanoid", "Walkspeed",        Humanoid_Walkspeed)
    PULL("Humanoid", "JumpPower",        Humanoid_JumpPower)

    PULL("BasePart",  "Primitive", BasePart_Primitive)
    PULL("Primitive", "Position",  Primitive_Position)
    PULL("Primitive", "Size",      Primitive_Size)
    PULL("Primitive", "Rotation",  Primitive_Rotation)
    PULL("Primitive", "AssemblyLinearVelocity",  Primitive_AssemblyLinearVelocity)
    PULL("Primitive", "AssemblyAngularVelocity", Primitive_AssemblyAngularVelocity)

    PULL("Instance", "NameContainer",   Instance_NameContainer)
    PULL("Instance", "Name",            Instance_Name)
    PULL("Instance", "ChildrenStart",   Instance_ChildrenStart)
    PULL("Instance", "ChildrenEnd",     Instance_ChildrenEnd)
    PULL("Instance", "ClassDescriptor", Instance_ClassDescriptor)
    PULL("Instance", "ClassName",       Instance_ClassName)
    PULL("Instance", "Parent",          Instance_Parent)

    PULL("Misc",  "Value",       Misc_Value)
    PULL("Model", "PrimaryPart", Model_PrimaryPart)

    PULL("VisualEngine", "Pointer",    VisualEngine_Pointer)
    PULL("VisualEngine", "ViewMatrix", VisualEngine_ViewMatrix)

#undef PULL

    if (!o.IsValid()) return false;
    m_offsets = o;
    return true;
}

inline void OffsetManager::ApplyToNamespace()
{
    const RobloxOffsets& o = m_offsets;
    if (!o.IsValid()) return;

    // Aplica SEMPRE (mesmo 0), porque 0 é offset válido em vários campos
    // (ex: Instance::Name = 0 no JSON atual). Usar "!= 0" quebraria a
    // leitura de nomes e por tabela toda a lógica de FindChild do AutoDive.
#define SET(NS, KEY, VAL) Offsets::NS::KEY = (VAL);

    SET(FakeDataModel, Pointer,       o.FakeDataModel_Pointer)
    SET(FakeDataModel, RealDataModel, o.FakeDataModel_RealDataModel)

    SET(DataModel, Workspace, o.DataModel_Workspace)
    SET(DataModel, PlaceId,   o.DataModel_PlaceId)
    SET(DataModel, GameId,    o.DataModel_GameId)

    SET(Workspace, CurrentCamera,       o.Workspace_CurrentCamera)
    SET(Workspace, DistributedGameTime, o.Workspace_DistributedGameTime)

    SET(Camera, Position,     o.Camera_Position)
    SET(Camera, Rotation,     o.Camera_Rotation)
    SET(Camera, FieldOfView,  o.Camera_FieldOfView)
    SET(Camera, ViewportSize, o.Camera_ViewportSize)

    SET(Player, LocalPlayer,   o.Player_LocalPlayer)
    SET(Player, ModelInstance, o.Player_ModelInstance)
    SET(Player, UserId,        o.Player_UserId)
    SET(Player, DisplayName,   o.Player_DisplayName)

    SET(Humanoid, Health,           o.Humanoid_Health)
    SET(Humanoid, MaxHealth,        o.Humanoid_MaxHealth)
    SET(Humanoid, HumanoidRootPart, o.Humanoid_HumanoidRootPart)
    SET(Humanoid, Walkspeed,        o.Humanoid_Walkspeed)
    SET(Humanoid, JumpPower,        o.Humanoid_JumpPower)

    SET(BasePart,  Primitive, o.BasePart_Primitive)
    SET(Primitive, Position, o.Primitive_Position)
    SET(Primitive, Size,     o.Primitive_Size)
    SET(Primitive, Rotation, o.Primitive_Rotation)
    SET(Primitive, AssemblyLinearVelocity,  o.Primitive_AssemblyLinearVelocity)
    SET(Primitive, AssemblyAngularVelocity, o.Primitive_AssemblyAngularVelocity)

    SET(Instance, NameContainer,   o.Instance_NameContainer)
    SET(Instance, Name,            o.Instance_Name)
    SET(Instance, ChildrenStart,   o.Instance_ChildrenStart)
    SET(Instance, ChildrenEnd,     o.Instance_ChildrenEnd)
    SET(Instance, ClassDescriptor, o.Instance_ClassDescriptor)
    SET(Instance, ClassName,       o.Instance_ClassName)
    SET(Instance, Parent,          o.Instance_Parent)

    SET(Misc,  Value,       o.Misc_Value)
    SET(Model, PrimaryPart, o.Model_PrimaryPart)

    SET(VisualEngine, Pointer,    o.VisualEngine_Pointer)
    SET(VisualEngine, ViewMatrix, o.VisualEngine_ViewMatrix)

#undef SET
}

inline bool OffsetManager::FetchFromHttp()
{
    std::string body = HttpGet(L"offsets.imtheo.lol", L"/Offsets.json");
    if (body.empty()) return false;
    if (!ParseJson(body)) return false;

    m_status = "HTTP";
    ApplyToNamespace();
    return true;
}

inline bool OffsetManager::LoadFromFile()
{
    std::string path = GetExeDir() + "offsets.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    std::stringstream ss;
    ss << f.rdbuf();
    if (!ParseJson(ss.str())) return false;

    m_status = "FILE";
    ApplyToNamespace();
    return true;
}

inline bool OffsetManager::Fetch()
{
    if (m_offsets.IsValid())
        return true;

    if (FetchFromHttp()) return true;
    if (LoadFromFile())  return true;

    m_status = "FALLBACK (offsets hardcoded)";
    return false;
}
