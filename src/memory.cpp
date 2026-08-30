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

    // PROCESS_VM_READ           → permite ReadProcessMemory
    // PROCESS_VM_WRITE          → permite WriteProcessMemory
    // PROCESS_VM_OPERATION      → necessário para WriteProcessMemory
    // PROCESS_QUERY_INFORMATION → permite consultar informações do processo
    m_handle = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
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
// Detach — fecha o handle do processo
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
// Evita syscall GetExitCodeProcess em todo frame:
//   • m_invalid é setado imediatamente por qualquer ReadRaw que falhe —
//     assim a detecção é instantânea quando o processo fecha durante leitura.
//   • A checagem periódica (a cada 500 ms) captura o caso em que o processo
//     termina sem que haja leituras ativas (ex: overlay oculto).
// --------------------------------------------------------------------------
bool Memory::IsValid() const
{
    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
        return false;

    // Falha imediata sinalizada por ReadRaw
    if (m_invalid)
        return false;

    // Checagem periódica — evita syscall a cada frame
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastCheck >= kCheckInterval)
    {
        m_lastCheck = now;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(m_handle, &exitCode) || exitCode != STILL_ACTIVE)
        {
            m_invalid = true;
            return false;
        }
    }

    return true;
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

    if (!ok || bytesRead != size)
    {
        // Falha de acesso → processo provavelmente terminou ou foi protegido.
        // Sinaliza invalidação imediata para que IsValid() retorne false no
        // próximo frame sem precisar de nova syscall GetExitCodeProcess.
        if (!ok)
            m_invalid = true;
        return false;
    }

    return true;
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
