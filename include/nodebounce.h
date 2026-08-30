#pragma once
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// --------------------------------------------------------------------------
// NoDebounce
//
// Módulo que mantém uma lista de BoolValues travados em false via
// WriteProcessMemory. Roda em thread dedicada a ~60 Hz e escreve
// false (0x00) em cada instância encontrada a cada scan.
//
// Equivalente C++ externo do script Lua "Debounce Freezer":
//   - Procura pelos BoolValues alvo em Character.Bools e no LocalPlayer
//   - WriteProcessMemory sobrescreve o byte do campo Value diretamente
//
// Uso:
//   NoDebounce nd(mem);
//   nd.Start(workspace, localPlayer);   // chame quando attach OK
//   nd.Stop();                          // chame ao desativar ou fechar
//
// Thread-safety: Start/Stop são chamados do render loop (thread principal).
//                m_enabled pode ser alterado a qualquer momento — é atômico.
// --------------------------------------------------------------------------
class NoDebounce {
public:
    // Lista de nomes de BoolValue a travar em false.
    // Espelha TARGET_BOOLS do script Lua.
    static constexpr const char* kTargetBools[] = {
        "Debounce",
        "TackleDebounce",
        "dribbleDebounce",
        "dribbleDelay",
        "Tackled",
        "PowerShootDebounce",
        "Offsides",
    };
    static constexpr int kTargetBoolCount =
        static_cast<int>(sizeof(kTargetBools) / sizeof(kTargetBools[0]));

    // Offset do campo Value dentro de um BoolValue (mesmo que Misc::Value = 0xb8)
    // Roblox armazena bool como 1 byte nesse offset.
    static constexpr uintptr_t kBoolValueOffset = Offsets::Misc::Value;

    // Config exposta na UI
    struct Config {
        bool enabled = false;

        // Rate do loop de scan em Hz (padrão 60 — evita overhead em alta freq.)
        int scanRateHz = 60;
    };
    Config cfg;

    // Estatísticas para exibição na UI
    struct Stats {
        int  locksThisFrame = 0;   // quantas escritas foram feitas no último scan
        int  totalLocks     = 0;   // total acumulado de escritas
        bool running        = false;
    };

    explicit NoDebounce(Memory& mem) : m_mem(mem) {}
    ~NoDebounce() { Stop(); }

    NoDebounce(const NoDebounce&)            = delete;
    NoDebounce& operator=(const NoDebounce&) = delete;

    // Inicia a thread de scan.
    // workspace e localPlayer são os ponteiros já resolvidos por RobloxReader.
    void Start(uintptr_t workspace, uintptr_t localPlayer);

    // Para a thread. Seguro chamar múltiplas vezes.
    void Stop();

    // Atualiza os ponteiros de workspace e localPlayer (chamado pelo render loop).
    void UpdatePointers(uintptr_t workspace, uintptr_t localPlayer);

    // Retorna cópia das stats para exibição na UI (thread-safe via atômicos).
    Stats GetStats() const;

private:
    // Loop da thread dedicada
    void ScanLoop();

    // Tenta escrever false em todos os BoolValues alvo encontrados em `parent`.
    // Retorna quantas escritas foram feitas.
    int LockBooleansIn(uintptr_t parent);

    // Helpers de leitura (reusam o padrão de rbx.cpp)
    template<typename T>
    T ReadT(uintptr_t addr, T def = {}) const {
        auto v = m_mem.Read<T>(addr);
        return v ? *v : def;
    }
    uintptr_t ReadPtr(uintptr_t addr) const { return ReadT<uintptr_t>(addr); }

    std::string ReadRbxString(uintptr_t addr) const;
    std::string GetInstanceName(uintptr_t instance) const;
    std::string GetInstanceClass(uintptr_t instance) const;
    std::vector<uintptr_t> GetChildren(uintptr_t instance) const;
    uintptr_t FindChild(uintptr_t instance, const std::string& name) const;

    // Verifica se `name` está na lista kTargetBools
    static bool IsTargetBool(const std::string& name);

    // Escreve false (0x00) via WriteProcessMemory no campo Value do BoolValue
    bool WriteFalse(uintptr_t boolValueInstance);

    Memory&           m_mem;

    // Ponteiros sincronizados pelo render loop via UpdatePointers()
    std::atomic<uintptr_t> m_workspace    { 0 };
    std::atomic<uintptr_t> m_localPlayer  { 0 };

    // Stats atômicas (lidas pela UI sem lock)
    std::atomic<int>  m_locksThisFrame { 0 };
    std::atomic<int>  m_totalLocks     { 0 };

    std::thread       m_thread;
    std::atomic<bool> m_running { false };
};
