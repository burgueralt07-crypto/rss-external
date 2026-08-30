#include "nodebounce.h"
#include <algorithm>
#include <cctype>

// --------------------------------------------------------------------------
// ReadRbxString — idêntico ao de rbx.cpp
// --------------------------------------------------------------------------
std::string NoDebounce::ReadRbxString(uintptr_t addr) const
{
    if (!addr) return {};
    int32_t len = ReadT<int32_t>(addr + 0x10);
    if (len <= 0 || len > 255) return {};
    uintptr_t dataPtr = (len >= 16) ? ReadPtr(addr) : addr;
    if (!dataPtr) return {};
    std::string result(len, '\0');
    m_mem.ReadRaw(dataPtr, result.data(), len);
    return result;
}

// --------------------------------------------------------------------------
// GetInstanceName
// --------------------------------------------------------------------------
std::string NoDebounce::GetInstanceName(uintptr_t instance) const
{
    if (!instance) return {};
    uintptr_t nameContainer = ReadPtr(instance + Offsets::Instance::NameContainer);
    if (!nameContainer) return {};
    return ReadRbxString(nameContainer + Offsets::Instance::Name);
}

// --------------------------------------------------------------------------
// GetInstanceClass
// --------------------------------------------------------------------------
std::string NoDebounce::GetInstanceClass(uintptr_t instance) const
{
    if (!instance) return {};
    uintptr_t classDesc = ReadPtr(instance + Offsets::Instance::ClassDescriptor);
    if (!classDesc) return {};
    uintptr_t namePtr   = ReadPtr(classDesc + Offsets::Instance::ClassName);
    if (!namePtr) return {};
    return ReadRbxString(namePtr);
}

// --------------------------------------------------------------------------
// GetChildren
// --------------------------------------------------------------------------
std::vector<uintptr_t> NoDebounce::GetChildren(uintptr_t instance) const
{
    std::vector<uintptr_t> result;
    if (!instance) return result;

    uintptr_t childStart = ReadPtr(instance + Offsets::Instance::ChildrenStart);
    if (!childStart) return result;

    uintptr_t childEnd = ReadPtr(childStart + Offsets::Instance::ChildrenEnd);
    uintptr_t current  = ReadPtr(childStart);

    if (!childEnd || !current || childEnd < current) return result;

    constexpr size_t kMaxChildren = 4096;
    size_t count = 0;
    for (uintptr_t ptr = current; ptr < childEnd && count < kMaxChildren; ptr += 0x10, ++count)
    {
        uintptr_t child = ReadPtr(ptr);
        if (child) result.push_back(child);
    }
    return result;
}

// --------------------------------------------------------------------------
// FindChild
// --------------------------------------------------------------------------
uintptr_t NoDebounce::FindChild(uintptr_t instance, const std::string& name) const
{
    for (uintptr_t child : GetChildren(instance))
    {
        if (GetInstanceName(child) == name)
            return child;
    }
    return 0;
}

// --------------------------------------------------------------------------
// IsTargetBool — verifica se o nome está na lista de alvos
// --------------------------------------------------------------------------
bool NoDebounce::IsTargetBool(const std::string& name)
{
    for (int i = 0; i < kTargetBoolCount; ++i)
    {
        if (name == kTargetBools[i])
            return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// WriteFalse — escreve 0x00 no campo Value do BoolValue via WPM
//
// Estrutura:
//   BoolValue + kBoolValueOffset (0xb8) = 1 byte bool
//
// WriteProcessMemory exige que o handle tenha sido aberto com
// PROCESS_VM_WRITE | PROCESS_VM_OPERATION (ver memory.cpp).
// --------------------------------------------------------------------------
bool NoDebounce::WriteFalse(uintptr_t boolValueInstance)
{
    if (!boolValueInstance) return false;

    uintptr_t addr  = boolValueInstance + kBoolValueOffset;
    HANDLE    hProc = m_mem.GetHandle();

    if (hProc == INVALID_HANDLE_VALUE || !hProc) return false;

    static constexpr uint8_t kFalse = 0x00;
    SIZE_T written = 0;
    return WriteProcessMemory(
        hProc,
        reinterpret_cast<LPVOID>(addr),
        &kFalse,
        sizeof(kFalse),
        &written
    ) && written == sizeof(kFalse);
}

// --------------------------------------------------------------------------
// LockBooleansIn — procura BoolValues alvo dentro de `parent` e trava em false
//
// Itera os filhos de `parent`:
//   - Se a classe for "BoolValue" e o nome estiver na lista → escreve false
//
// Retorna o número de escritas realizadas.
// --------------------------------------------------------------------------
int NoDebounce::LockBooleansIn(uintptr_t parent)
{
    if (!parent) return 0;
    int count = 0;

    for (uintptr_t child : GetChildren(parent))
    {
        if (GetInstanceClass(child) != "BoolValue") continue;

        std::string name = GetInstanceName(child);
        if (!IsTargetBool(name)) continue;

        // Lê o valor atual antes de escrever — evita WPM desnecessário
        uint8_t current = ReadT<uint8_t>(child + kBoolValueOffset, 0xFF);
        if (current != 0x00)
        {
            if (WriteFalse(child))
                ++count;
        }
    }
    return count;
}

// --------------------------------------------------------------------------
// UpdatePointers — chamado pelo render loop (thread principal)
// Atômico: a ScanLoop lê m_workspace/m_localPlayer sem lock.
// --------------------------------------------------------------------------
void NoDebounce::UpdatePointers(uintptr_t workspace, uintptr_t localPlayer)
{
    m_workspace.store(workspace, std::memory_order_relaxed);
    m_localPlayer.store(localPlayer, std::memory_order_relaxed);
}

// --------------------------------------------------------------------------
// GetStats — snapshot para a UI
// --------------------------------------------------------------------------
NoDebounce::Stats NoDebounce::GetStats() const
{
    Stats s;
    s.locksThisFrame = m_locksThisFrame.load(std::memory_order_relaxed);
    s.totalLocks     = m_totalLocks.load(std::memory_order_relaxed);
    s.running        = m_running.load(std::memory_order_relaxed);
    return s;
}

// --------------------------------------------------------------------------
// Start — inicia a thread de scan
// --------------------------------------------------------------------------
void NoDebounce::Start(uintptr_t workspace, uintptr_t localPlayer)
{
    Stop();
    m_workspace.store(workspace,   std::memory_order_relaxed);
    m_localPlayer.store(localPlayer, std::memory_order_relaxed);
    m_running = true;
    m_thread  = std::thread(&NoDebounce::ScanLoop, this);
}

// --------------------------------------------------------------------------
// Stop — para a thread
// --------------------------------------------------------------------------
void NoDebounce::Stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

// --------------------------------------------------------------------------
// ScanLoop — thread dedicada
//
// A cada iteração:
//   1. Se cfg.enabled == false → dorme e continua (não escreve nada)
//   2. Pega workspace e localPlayer dos atômicos
//   3. Lê o model do localPlayer → Character.Bools → varre BoolValues
//   4. Varre filhos diretos do LocalPlayer (PowerShootDebounce, Offsides)
//
// O scan segue exatamente a lógica do Lua:
//   Camada 2 (Trava Ativa de Instâncias) — Heartbeat loop
//     → char.Bools → each child → lock
//     → LocalPlayer → each child → lock
// --------------------------------------------------------------------------
void NoDebounce::ScanLoop()
{
    while (m_running)
    {
        if (!cfg.enabled)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!m_mem.IsValid())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        uintptr_t workspace   = m_workspace.load(std::memory_order_relaxed);
        uintptr_t localPlayer = m_localPlayer.load(std::memory_order_relaxed);

        int locksThisFrame = 0;

        // ── 1. Character.Bools ─────────────────────────────────────────
        // LocalPlayer.Character → char:FindFirstChild("Bools") → filhos
        if (localPlayer)
        {
            // Player.ModelInstance aponta para o Character model
            uintptr_t charModel = ReadPtr(localPlayer + Offsets::Player::ModelInstance);
            if (charModel)
            {
                uintptr_t boolsFolder = FindChild(charModel, "Bools");
                if (boolsFolder)
                    locksThisFrame += LockBooleansIn(boolsFolder);
            }

            // ── 2. Filhos diretos do LocalPlayer ───────────────────────
            // PowerShootDebounce e Offsides ficam no LocalPlayer diretamente
            locksThisFrame += LockBooleansIn(localPlayer);
        }

        // Atualiza stats
        m_locksThisFrame.store(locksThisFrame, std::memory_order_relaxed);
        if (locksThisFrame > 0)
            m_totalLocks.fetch_add(locksThisFrame, std::memory_order_relaxed);

        // Sleep até o próximo tick
        int rateHz = (cfg.scanRateHz > 0 && cfg.scanRateHz <= 500) ? cfg.scanRateHz : 60;
        std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / rateHz));
    }
}
