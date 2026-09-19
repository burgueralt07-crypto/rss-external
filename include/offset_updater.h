#pragma once
// --------------------------------------------------------------------------
// OffsetUpdater — aplica offsets colados de offsets.hpp do Theo em runtime
//
// Uso:
//   Cole o conteúdo de https://offsets.imtheo.lol/<version>/offsets.hpp
//   na textarea do menu (aba Misc > Offset Updater > Colar .hpp).
//   Clique "Aplicar" — os campos de Offsets:: são atualizados sem reiniciar.
//
// Também exporta FetchAndApplyFromVersion() para busca automática via
// WinHTTP (requer rede).
// --------------------------------------------------------------------------

#include "offsets.h"
#include <string>
#include <string_view>
#include <charconv>
#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace OffsetUpdater {

// --------------------------------------------------------------------------
// ParseHpp — parseia conteúdo de um offsets.hpp linha a linha
//
// Formato de cada linha relevante:
//   namespace X {
//   inline constexpr uintptr_t FieldName = 0xABC;
//
// Estratégia: rastreia o namespace atual, e para cada linha com
// "uintptr_t FieldName = 0x..." tenta casar com os campos conhecidos.
//
// Retorna número de campos alterados. outVersion recebe o ClientVersion.
// --------------------------------------------------------------------------
inline int ParseHpp(const std::string& content, std::string& outVersion)
{
    using namespace Offsets;

    // Tabela namespace -> campo -> ponteiro para a variável
    struct Entry { const char* ns; const char* field; uintptr_t* ptr; };
    Entry table[] = {
        { "BasePart",      "Primitive",                &BasePart::Primitive                },
        { "Camera",        "Position",                 &Camera::Position                   },
        { "Camera",        "Rotation",                 &Camera::Rotation                   },
        { "Camera",        "ViewportSize",             &Camera::ViewportSize               },
        { "FakeDataModel", "Pointer",                  &FakeDataModel::Pointer             },
        { "FakeDataModel", "RealDataModel",            &FakeDataModel::RealDataModel       },
        { "Humanoid",      "Health",                   &Humanoid::Health                   },
        { "Humanoid",      "HumanoidRootPart",         &Humanoid::HumanoidRootPart         },
        { "Humanoid",      "MaxHealth",                &Humanoid::MaxHealth                },
        { "Humanoid",      "Walkspeed",                &Humanoid::Walkspeed                },
        { "Instance",      "ChildrenEnd",              &Instance::ChildrenEnd              },
        { "Instance",      "ChildrenStart",            &Instance::ChildrenStart            },
        { "Instance",      "ClassDescriptor",          &Instance::ClassDescriptor          },
        { "Instance",      "ClassName",                &Instance::ClassName                },
        { "Instance",      "Name",                     &Instance::Name                     },
        { "Instance",      "NameContainer",            &Instance::NameContainer            },
        { "Instance",      "Parent",                   &Instance::Parent                   },
        { "Misc",          "StringLength",             &Misc::StringLength                 },
        { "Misc",          "Value",                    &Misc::Value                        },
        { "Model",         "PrimaryPart",              &Model::PrimaryPart                 },
        { "Player",        "LocalPlayer",              &Player::LocalPlayer                },
        { "Player",        "ModelInstance",            &Player::ModelInstance              },
        { "Player",        "UserId",                   &Player::UserId                     },
        { "Primitive",     "AssemblyAngularVelocity",  &Primitive::AssemblyAngularVelocity },
        { "Primitive",     "AssemblyLinearVelocity",   &Primitive::AssemblyLinearVelocity  },
        { "Primitive",     "Flags",                    &Primitive::Flags                   },
        { "Primitive",     "Position",                 &Primitive::Position                },
        { "Primitive",     "Rotation",                 &Primitive::Rotation                },
        { "Primitive",     "Size",                     &Primitive::Size                    },
        { "TaskScheduler", "Pointer",                  &TaskScheduler::Pointer             },
        { "VisualEngine",  "Pointer",                  &VisualEngine::Pointer              },
        { "VisualEngine",  "ViewMatrix",               &VisualEngine::ViewMatrix           },
        { "Weld",          "Part0",                    &Weld::Part0                        },
        { "Weld",          "Part1",                    &Weld::Part1                        },
    };
    constexpr int TABLE_SIZE = (int)(sizeof(table) / sizeof(table[0]));

    int changed = 0;
    std::string curNs;

    // Itera linha a linha
    size_t pos = 0;
    while (pos < content.size())
    {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        std::string_view line(content.data() + pos, end - pos);
        pos = end + 1;

        // Remove \r se presente
        if (!line.empty() && line.back() == '\r')
            line = line.substr(0, line.size() - 1);

        // Detecta: namespace X {
        {
            auto p = line.find("namespace ");
            if (p != std::string_view::npos)
            {
                auto after = p + 10; // len("namespace ")
                auto brace = line.find('{', after);
                if (brace != std::string_view::npos)
                {
                    std::string_view nsName = line.substr(after, brace - after);
                    // Trim espaços
                    while (!nsName.empty() && nsName.front() == ' ') nsName.remove_prefix(1);
                    while (!nsName.empty() && nsName.back()  == ' ') nsName.remove_suffix(1);
                    curNs = std::string(nsName);
                }
                continue;
            }
        }

        // Detecta ClientVersion = "version-xxx"
        if (outVersion.empty())
        {
            auto p = line.find("ClientVersion");
            if (p != std::string_view::npos)
            {
                auto q1 = line.find('"', p);
                if (q1 != std::string_view::npos)
                {
                    auto q2 = line.find('"', q1 + 1);
                    if (q2 != std::string_view::npos)
                        outVersion = std::string(line.substr(q1 + 1, q2 - q1 - 1));
                }
                continue;
            }
        }

        // Detecta: ... uintptr_t FieldName = 0xABC;
        {
            auto p = line.find("uintptr_t ");
            if (p == std::string_view::npos) continue;

            auto nameStart = p + 10; // len("uintptr_t ")
            auto eq = line.find('=', nameStart);
            if (eq == std::string_view::npos) continue;

            std::string_view fieldName = line.substr(nameStart, eq - nameStart);
            while (!fieldName.empty() && fieldName.back() == ' ') fieldName.remove_suffix(1);

            // Avança após '='
            auto valStart = eq + 1;
            while (valStart < line.size() && line[valStart] == ' ') ++valStart;
            if (valStart >= line.size()) continue;

            // Parseia valor hex ou decimal
            uintptr_t val = 0;
            bool ok = false;
            if (valStart + 1 < line.size() &&
                line[valStart] == '0' && (line[valStart+1] == 'x' || line[valStart+1] == 'X'))
            {
                const char* s = line.data() + valStart + 2;
                const char* e = line.data() + line.size();
                auto [ptr, ec] = std::from_chars(s, e, val, 16);
                ok = (ec == std::errc{});
            }
            else if (line[valStart] >= '0' && line[valStart] <= '9')
            {
                const char* s = line.data() + valStart;
                const char* e = line.data() + line.size();
                auto [ptr, ec] = std::from_chars(s, e, val, 10);
                ok = (ec == std::errc{});
            }

            if (!ok) continue;

            // Procura na tabela
            for (int i = 0; i < TABLE_SIZE; ++i)
            {
                if (curNs == table[i].ns && fieldName == table[i].field)
                {
                    if (*table[i].ptr != val)
                    {
                        *table[i].ptr = val;
                        ++changed;
                    }
                    break;
                }
            }
        }
    }

    return changed;
}

// --------------------------------------------------------------------------
// FetchHpp — GET HTTPS para /<version>/offsets.hpp via WinHTTP
// --------------------------------------------------------------------------
inline bool FetchHpp(const std::string& version, std::string& outBody, std::string& outErr)
{
    if (version.empty()) { outErr = "versao vazia"; return false; }

    std::wstring wver(version.begin(), version.end());
    std::wstring wpath = L"/" + wver + L"/offsets.hpp";

    HINTERNET hSess = WinHttpOpen(L"rss-external/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { outErr = "WinHttpOpen failed"; return false; }

    HINTERNET hConn = WinHttpConnect(hSess, L"offsets.imtheo.lol",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); outErr = "WinHttpConnect failed"; return false; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq)
    {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        outErr = "WinHttpOpenRequest failed"; return false;
    }

    DWORD timeout = 8000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hReq, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    bool ok = false;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr))
    {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

        if (status == 200)
        {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (WinHttpReadData(hReq, chunk.data(), avail, &read))
                    outBody.append(chunk.data(), read);
                else break;
            }
            ok = !outBody.empty();
            if (!ok) outErr = "Empty response";
        }
        else
        {
            outErr = "HTTP " + std::to_string(status);
        }
    }
    else
    {
        outErr = "Request failed (" + std::to_string(GetLastError()) + ")";
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return ok;
}

// --------------------------------------------------------------------------
// FetchAndApplyFromVersion — fetch + parse em uma chamada
// --------------------------------------------------------------------------
inline int FetchAndApplyFromVersion(const std::string& version,
                                    std::string& outVersion,
                                    std::string& outErr)
{
    std::string body;
    if (!FetchHpp(version, body, outErr)) return -1;

    if (body.find("namespace Offsets") == std::string::npos)
    {
        outErr = "resposta invalida (versao nao encontrada?)";
        return -1;
    }

    return ParseHpp(body, outVersion);
}

} // namespace OffsetUpdater
