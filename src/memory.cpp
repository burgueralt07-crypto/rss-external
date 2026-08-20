#include "memory.h"
#include <stdexcept>

// --------------------------------------------------------------------------
// FindPID — varre todos os processos via snapshot e retorna o PID
// --------------------------------------------------------------------------
DWORD Memory::FindPID(const std::wstring& processName)
{
    // TH32CS_SNAPPROCESS: snapshot de todos os processos em execução
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
            // Comparação case-insensitive
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
// Attach — encontra o PID e abre o handle com permissões de leitura
// --------------------------------------------------------------------------
bool Memory::Attach(const std::wstring& processName)
{
    Detach(); // fecha handle anterior se existir

    m_pid = FindPID(processName);
    if (m_pid == 0)
        return false; // processo não encontrado

    // PROCESS_VM_READ         → permite ReadProcessMemory
    // PROCESS_QUERY_INFORMATION → permite consultar informações do processo
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

    return true;
}

// --------------------------------------------------------------------------
// Detach — fecha o handle do processo
// --------------------------------------------------------------------------
void Memory::Detach()
{
    if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    m_pid = 0;
}

// --------------------------------------------------------------------------
// IsValid — verifica se o processo ainda está rodando
// --------------------------------------------------------------------------
bool Memory::IsValid() const
{
    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
        return false;

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_handle, &exitCode))
        return false;

    // STILL_ACTIVE (259) → processo ainda em execução
    return exitCode == STILL_ACTIVE;
}

// --------------------------------------------------------------------------
// GetModuleBase — retorna o endereço base de um módulo no processo alvo
// --------------------------------------------------------------------------
uintptr_t Memory::GetModuleBase(const std::wstring& moduleName) const
{
    if (m_pid == 0)
        return 0;

    // TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32 cobre processos 32 e 64 bits
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

    return ok && bytesRead == size;
}

// --------------------------------------------------------------------------
// ResolvePointerChain — desreferencia uma cadeia de ponteiros
//
// Exemplo (3 níveis):
//   base  → [base + 0x10] → [resultado + 0x30] → [resultado + 0x4] = valor final
//
//   uintptr_t addr = mem.ResolvePointerChain(moduleBase, {0x10, 0x30, 0x4});
// --------------------------------------------------------------------------
uintptr_t Memory::ResolvePointerChain(uintptr_t base,
                                       const std::initializer_list<uintptr_t>& offsets) const
{
    uintptr_t current = base;

    for (uintptr_t offset : offsets)
    {
        // Lê o ponteiro no endereço atual (64 bits)
        auto next = Read<uintptr_t>(current + offset);
        if (!next || *next == 0)
            return 0; // cadeia quebrada

        current = *next;
    }

    return current;
}
