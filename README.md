# rss-external — Overlay Transparente (ImGui + DX11 + Win32)

Overlay externo para Windows utilizando ImGui com backend DirectX 11.  
A janela é totalmente transparente e click-through: o mouse passa para a aplicação de baixo.

## Estrutura

```
rss-external/
├── CMakeLists.txt
├── offsets.h                  ← offsets do alvo
├── include/
│   ├── overlay.h              ← janela Win32 transparente
│   └── renderer.h             ← DirectX 11 + ImGui
├── src/
│   ├── main.cpp               ← loop principal + UI
│   ├── overlay.cpp
│   └── renderer.cpp
└── imgui-master/imgui-master/ ← ImGui (já presente)
```

## Requisitos

- Windows 10/11
- Visual Studio 2022 (com workload "Desenvolvimento para Desktop com C++")
- CMake >= 3.16

## Build

```bat
:: Clone / abra o repositório
cd rss-external

:: Configure (x64)
cmake -B build -A x64

:: Compile
cmake --build build --config Release

:: Executável gerado em:
build\Release\rss-external.exe
```

## Uso

1. Abra o jogo / aplicação alvo.
2. Execute `rss-external.exe`.
3. O overlay aparecerá sobreposto à janela configurada em `TARGET_WINDOW` (em `main.cpp`).

## Customização

| Arquivo | O que mudar |
|---|---|
| `src/main.cpp` | `TARGET_WINDOW` → nome da janela alvo; `RenderOverlayUI()` → sua UI |
| `offsets.h` | Offsets de memória do alvo |
| `include/renderer.h` | Parâmetros do swap chain / vsync |

## Como o overlay é transparente?

- `WS_EX_LAYERED | WS_EX_TRANSPARENT` → janela sem input e transparente para o sistema.
- `DwmExtendFrameIntoClientArea` com margens `-1` → DWM trata toda a área como "frame".
- Clear do back-buffer com `alpha = 0` → pixels sem desenho ficam invisíveis.
- ImGui desenha com alpha normal → apenas o conteúdo renderizado aparece.
