#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <optional>

// --------------------------------------------------------------------------
// Memory — leitura externa de processo (ReadProcessMemory)
//
// Uso:
//   Memory mem;
//   if (!mem.Attach(L"RobloxPlayerBeta.exe")) { /* processo não encontrado */ }
//
//   uintptr_t base = mem.GetModuleBase(L"RobloxPlayerBeta.exe");
//   float hp = mem.Read<float>(base + 0x1234);
// --------------------------------------------------------------------------
class Memory {
public:
    Memory()  = default;
    ~Memory() { Detach(); }

    // Não copiável — handle único de processo
    Memory(const Memory&)            = delete;
    Memory& operator=(const Memory&) = delete;

    // ---------- Ciclo de vida ----------

    // Abre handle para o processo pelo nome do executável (ex: L"RobloxPlayerBeta.exe")
    // Retorna true se encontrou e abriu com sucesso.
    bool Attach(const std::wstring& processName);

    // Fecha o handle do processo
    void Detach();

    // Retorna true se o handle está aberto e o processo ainda existe
    bool IsValid() const;

    // ---------- Informações ----------

    DWORD      GetPID()  const { return m_pid; }
    HANDLE     GetHandle() const { return m_handle; }

    // Retorna o endereço base de um módulo dentro do processo alvo.
    // Passa o nome do exe principal para obter a base do executável.
    uintptr_t  GetModuleBase(const std::wstring& moduleName) const;

    // ---------- Leitura de memória ----------

    // Lê `size` bytes a partir de `address` no processo alvo.
    // Retorna true se a leitura foi completa.
    bool ReadRaw(uintptr_t address, void* buffer, SIZE_T size) const;

    // Lê um tipo T diretamente (ex: Read<float>(addr))
    // Retorna std::nullopt em caso de falha.
    template<typename T>
    std::optional<T> Read(uintptr_t address) const
    {
        T value{};
        if (!ReadRaw(address, &value, sizeof(T)))
            return std::nullopt;
        return value;
    }

    // Resolve uma cadeia de ponteiros (pointer chain / multi-level pointer).
    // offsets: lista de offsets a aplicar em sequência partindo de `base`.
    // Retorna 0 em caso de falha em qualquer etapa.
    uintptr_t ResolvePointerChain(uintptr_t base,
                                   const std::initializer_list<uintptr_t>& offsets) const;

private:
    // Encontra o PID de um processo pelo nome do executável
    static DWORD FindPID(const std::wstring& processName);

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    DWORD  m_pid    = 0;
};
