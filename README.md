# Epinephrine

Ferramenta **all-in-one** de otimização e configuração de sistema para gamers no Linux, construída com **GTK 4**, **libadwaita** e **C17**.

O Epinephrine reúne monitoramento de hardware, tuning de performance e configuração de recursos como **Gamescope** e **Gamemode** em uma única interface moderna (GNOME/Adwaita), com suporte a temas e traduções.

## Funcionalidades

### Overview (Dashboard)
- **Gaming Score** (0.0 – 11.0): classifica o sistema comparando CPU, GPU, RAM e monitor contra uma máquina de referência.
- Monitoramento em tempo real de **CPU**, **RAM**, **GPU/VRAM**, **Vulkan** e **video driver/kernel module**.
- Informações de **armazenamento** (discos e uso).
- Atalho para **lançamento rápido de jogos recentes**.

### Game Library (Biblioteca de Jogos)
- Importação automática de jogos a partir de:
  - **Steam** (parse de `libraryfolders.vdf`)
  - **Heroic Games Launcher**
  - **Lutris**
  - **Bottles** (`programs.yml` / diretório `programs`)
- Card por jogo com ícone, tempo de jogo, FPS médio, espaço para comandos de inicialização e botão de **play**.

### Hardware Control (Controle de Hardware)
- **GPU**: informações detalhadas, monitor de uso/temperatura, e gerenciamento por recurso detectado:
  - Performance level, power profile, power limit, Frame Rate Target Control
  - AMD Radeon Chill, Anti-Lag, Variable Rate Shading, Zero RPM fan
  - Controle de ventoinha, e gerenciamento NVIDIA (via `nvidia-smi`)
- **Placa-mãe**: fabricante/modelo, BIOS, chipset, SMBus.
- **RAM**: módulos (DIMMs), total/usado, tecnologia e frequência.
- **CPU**: governor, frequência min/máx, arquitetura, cores.
- **Teclado**: informação do dispositivo, numlock/capslock/scrolllock, repetição de teclas, layout e backlight.
- **Mouse**: velocidade, perfil de aceleração, scroll natural (libinput/gsettings/xinput) e configuração especial para dispositivos gamer como o mouse **Logitech G300s**:
  - Perfis on-board (F3/F4/F5), DPI, polling rate, cor de LED e remapeamento de botões (G1–G9) via HID raw.
- **Controles (joystick)**: teste em tempo real com visual de botões/analógicos (layouts padrão e arcade), indicadores de input, botões extras (M1–M4) detectados por `EVIOCGBIT`, e nome/URL vindos de banco de dados.

### Utilities (Utilitários)
- **Gamescope**: gerador visual de comando (`--nested`, resolução, refresh, HDR, scaler/filter, sharpness, VRR, backend, ReShade, e muito mais), com copiar/salvar/restaurar.
- **GameMode**: configuração de `gamemode.ini` (governors, renice, frequência CPU, scripts start/end, tweaks de desktop, etc.).
- **MangoHud**: configuração do overlay (FPS, temps, posição, tamanho de fonte, histograma, custom text, etc.).
- Se o utilitário não estiver instalado, o app oferece um botão de **instalação** usando o gerenciador de pacotes detectado (`apt`/`dnf`/`pacman`/`zypper`/`xbps`) via `pkexec`.

### Settings (Configurações)
- **Tema**: sistema, libadwaita ou Breeze.
- **Cor de destaque**: seguir a GPU detectada, ou forçar AMD/Intel/NVIDIA/Tux.
- **Idioma**: português do Brasil (`pt_BR`) e inglês.

### Banco de dados de hardware (INDEX)
O app carrega bancos de dados em Markdown (compilados como recursos) para exibir nome, URL oficial e specs dos dispositivos detectados:
- `database/GPU_INDEX.md` — specs, VRAM e data de lançamento de GPUs.
- `database/JOY_INDEX.md` — controles (nome, categoria, URL, features).
- `database/KEYBOARD_INDEX.md` — teclados.
- `database/MOUSE_INDEX.md` — mouses.

Também consulta o banco **usb.ids** do sistema (`/usr/share/hwdata/usb.ids`) com refresh assíncrono via `curl`, usado como fallback de nomeação de dispositivos USB.

## Dependências

### Compilação (build)
| Dependência | Versão mínima |
|---|---|
| Meson | >= 1.0.0 |
| GTK 4 (`gtk4`) | >= 4.10 |
| libadwaita (`libadwaita-1`) | >= 1.4 |
| fontconfig | — |
| Compilador C com suporte a C17 (GCC, Clang) | — |

### Fedora
```sh
sudo dnf install meson gtk4-devel libadwaita-devel fontconfig-devel
```

### Ubuntu/Debian
```sh
sudo apt install meson libgtk-4-dev libadwaita-1-dev libfontconfig1-dev
```

### Arch
```sh
sudo pacman -S meson gtk4 libadwaita fontconfig
```

### Runtime (opcional, recursos são habilitados quando detectados)
| Programa | Função |
|---|---|
| `gamescope` | Aba Gamescope do Utilities |
| `gamemoderun` / `gamemoded` | Aba GameMode do Utilities |
| `mangoapp` / `mangohud` | Aba MangoHud do Utilities |
| `gsettings` (GNOME) ou `xinput` (X11) | Aplicar configurações de mouse |
| `openrgb` | Editar iluminação no OpenRGB |
| `libinput` | Detecção de DPI do mouse |
| `nvidia-smi` | Gerenciamento de GPU NVIDIA |
| `pkexec` | Operações privilegiadas (instalação de utilitários, tuning) |
| `curl` | Refresh do banco usb.ids |

## Build

```sh
meson setup _build
meson compile -C _build
```

O binário será gerado em `_build/src/epinephrine`.

Consulte [BUILDING.md](BUILDING.md) para instruções detalhadas de build (incluindo builds com Address Sanitizer), instalação e limpeza.

## Estrutura do projeto

```
epinephrine/
├── meson.build                 # Configuração do Meson
├── data/                       # UI XML (GResources), ícones, fonts, desktop/metainfo
│   └── ui/                     # Páginas em formato GTK Builder (.ui)
├── database/                   # Bancos de dados INDEX (GPU, JOY, KEYBOARD, MICE)
├── po/                         # Traduções (pt_BR, templates .pot)
├── src/
│   ├── main.c, application.c   # Bootstrap da aplicação
│   ├── window.c                # Janela principal e abas
│   ├── command-builder.c       # Gerador de comando do Gamescope
│   ├── epi-privilege.c         # Execução privilegiada via pkexec
│   ├── usb-ids.c               # Banco usb.ids (sistema + refresh online)
│   └── pages/                  # Páginas do app (dashboard, games, hardware, ...)
└── BUILDING.md                 # Guia de build detalhado
```

## Como contribuir

### Adicionar um dispositivo ao banco de dados
1. Descubra o `VID:PID` com `lsusb` (ou `dmesg | grep -i "input:"` para teclados).
2. Encontre a página oficial do produto na internet (página do fabricante).
3. Abra um pull request adicionando a linha à tabela do INDEX correspondente em `database/`:
   - **GPU**: `GPU_INDEX.md` (nome exibido por `vulkaninfo`, VRAM, URL, specs, data de lançamento).
   - **Controle**: `JOY_INDEX.md` (nome, VID:PID, categoria, URL, features, conexão, botões).
   - **Teclado**: `KEYBOARD_INDEX.md` (nome, VID:PID, keyset, URL, features, conexão).
   - **Mouse**: `MICE_INDEX.md` (nome, VID:PID, URL, número de botões).

### Traduções
Os arquivos de tradução estão em `po/` (gettext). Execute `meson compile -C _build epinephrine-pot` para regenerar o template `.pot` e atualize/adicione `.po` dos idiomas desejados.

### Código
- Padrão da linguagem: **C17**.
- O código usa **GTK 4** com **Builder (.ui)** e **libadwaita** (widgets `Adw`).
- Mantenha o padrão das páginas existentes (cada página tem `page-<nome>.c`/`.h` e um arquivo `.ui` em `data/ui/`).
- Os INDEX files são parseados como recursos compilados (ver `load_controller_index` em `page-hardware.c` como referência).

## Licença

MIT — veja o arquivo `meson.build` (`license: 'MIT'`).
