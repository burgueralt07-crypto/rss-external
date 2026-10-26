#include "memory.h"
#include <stdexcept>

// --------------------------------------------------------------------------
// FindPID — varre todos os processos via snapshot e retorna o PID
// --------------------------------------------------------------------------
DWORD Memory::FindPID(const std::wstring& processName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;

    if (Process32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        }
        while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return pid;
}

// --------------------------------------------------------------------------
// Attach
// --------------------------------------------------------------------------
bool Memory::Attach(const std::wstring& processName)
{
    Detach();

    m_pid = FindPID(processName);
    if (m_pid == 0)
        return false;

    m_handle = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        m_pid
    );

    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
    {
        m_handle = INVALID_HANDLE_VALUE;
        m_pid    = 0;
        return false;
    }

    m_invalid   = false;
    m_lastCheck = std::chrono::steady_clock::now();
    return true;
}

// --------------------------------------------------------------------------
// Detach
// --------------------------------------------------------------------------
void Memory::Detach()
{
    if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    m_pid     = 0;
    m_invalid = false;
}

// --------------------------------------------------------------------------
// IsValid — verifica se o processo ainda está rodando
//
// CORREÇÃO: o processo pode ficar "vivo" mesmo após leituras falharem
// (ex: DataModel trocado ao entrar em jogo). Por isso, se o handle ainda
// está aberto e o processo continua rodando, limpamos m_invalid em vez de
// reportar offline para sempre. Só reportamos inválido quando
// GetExitCodeProcess confirma que o processo morreu.
// --------------------------------------------------------------------------
bool Memory::IsValid() const
{
    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
        return false;

    auto now = std::chrono::steady_clock::now();
    bool checkNow = m_invalid || (now - m_lastCheck >= kCheckInterval);

    if (checkNow)
    {
        m_lastCheck = now;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(m_handle, &exitCode) || exitCode != STILL_ACTIVE)
        {
            m_invalid = true;
            return false;
        }

        // Processo continua vivo → limpa a flag de invalidação, permitindo
        // que leituras voltem a funcionar mesmo após falhas pontuais.
        m_invalid = false;
    }

    return true;
}

// --------------------------------------------------------------------------
// GetModuleBase
// --------------------------------------------------------------------------
uintptr_t Memory::GetModuleBase(const std::wstring& moduleName) const
{
    if (m_pid == 0)
        return 0;

    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_pid);

    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    uintptr_t base = 0;

    if (Module32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0)
            {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        }
        while (Module32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return base;
}

// --------------------------------------------------------------------------
// ReadRaw — wrapper em torno de ReadProcessMemory
//
// CORREÇÃO: NÃO marcar m_invalid em falhas isoladas de leitura. Durante uma
// troca de DataModel (entrar/sair de jogo) os endereços cacheados ficam
// inválidos e ReadProcessMemory falha naturalmente — mas o processo continua
// vivo e os ponteiros serão re-resolvidos no próximo UpdateStructure().
// Só marcamos inválido se GetExitCodeProcess confirmar que o processo morreu.
// --------------------------------------------------------------------------
bool Memory::ReadRaw(uintptr_t address, void* buffer, SIZE_T size) const
{
    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
        return false;

    SIZE_T bytesRead = 0;
    BOOL ok = ReadProcessMemory(
        m_handle,
        reinterpret_cast<LPCVOID>(address),
        buffer,
        size,
        &bytesRead
    );

    if (!ok || bytesRead != size)
    {
        if (!ok)
        {
            // Distingue "processo morreu" de "endereço temporariamente inválido".
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(m_handle, &exitCode) || exitCode != STILL_ACTIVE)
                m_invalid = true;
        }
        return false;
    }

    return true;
}

// --------------------------------------------------------------------------
// ResolvePointerChain
// --------------------------------------------------------------------------
uintptr_t Memory::ResolvePointerChain(uintptr_t base,
                                       const std::initializer_list<uintptr_t>& offsets) const
{
    uintptr_t current = base;

    for (uintptr_t offset : offsets)
    {
        auto next = Read<uintptr_t>(current + offset);
        if (!next || *next == 0)
            return 0;

        current = *next;
    }

    return current;
}
