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

A biblioteca alterna entre uma grade de capas com nome abaixo e uma lista lateral
com painel detalhado. A preferência fica salva. Clique na capa para abrir banner,
ações, estatísticas registradas pelo Epinephrine e configuração de inicialização.
Lista e grade usam as views virtualizadas do GTK: widgets são criados apenas
para uma faixa próxima da área visível. Cada jogo recebe um UUID persistente em
`games.ini`; associações anteriores por comando são migradas para esse UUID.

A Visão geral também mostra capas nos jogos recentes; clicar nelas abre os detalhes.
As cores acompanham o tema e a cor de destaque selecionados no aplicativo.
Busca, filtros por serviço e categoria e seleção para ações em lote continuam
disponíveis; os checkboxes ficam à esquerda da lista, fora da área da barra de
rolagem. A engrenagem abre a edição das opções de inicialização.

Em **Artes e serviços online**, configure sua chave de API do
[SteamGridDB](https://www.steamgriddb.com/profile/preferences/api) para carregar
banners com crédito ao artista. A variável `STEAMGRIDDB_API_KEY` também é aceita.
Sem chave, jogos associados à Steam usam o banner público da Steam quando disponível.
Para jogos de outros launchers, informe o Steam App ID nessa mesma janela para
obter artes e compatibilidade da edição correspondente. A descoberta automática
por nome aceita apenas uma correspondência exata e única,
inclusive na busca pública da Steam. Atalhos do Hydra com `shop=steam` aproveitam
o ID de catálogo que já vem no comando importado. A associação automática de
artes por nome não atribui uma classificação do ProtonDB a uma edição não confirmada.

No botão **Personalizar artes** (ícone de imagem ao lado da engrenagem), escolha
**Capa** ou **Banner**. A galeria oferece as artes disponíveis na Steam e no
SteamGridDB; pesquise outro título e selecione o jogo correto quando a importação
trouxer um nome diferente. Também é possível escolher uma imagem local (até 12 MB)
ou restaurar a escolha automática. Imagens escolhidas são copiadas para
`$XDG_DATA_HOME/epinephrine/artwork` (padrão `~/.local/share/epinephrine/artwork`),
com os créditos da fonte preservados. As escolhas continuam funcionando sem rede
e acompanham alterações no comando do jogo. O título em texto aparece fora do
banner para permanecer legível com qualquer arte.

A classificação e o número de relatos vêm do resumo público do **ProtonDB**;
esse endpoint não é uma API estável e pode ficar indisponível. SteamDB e
HowLongToBeat abrem consultas no navegador, sem coleta automática ou estimativas
inventadas. A compatibilidade de uma edição Steam pode diferir da de outro launcher.

O carregamento precisa de **Python 3**, usa HTTPS em segundo plano, limita a três
processos simultâneos para capas e galeria e não bloqueia a interface. A seleção
envia o nome ou App ID do jogo aos provedores correspondentes.
Os resultados ficam por 24 horas em `$XDG_CACHE_HOME/epinephrine/library`
(padrão `~/.cache/epinephrine/library`); falhas são reconsultadas após 5 minutos,
preservando dados anteriores quando possível. Chave e associações ficam em
`$XDG_CONFIG_HOME/epinephrine/library.ini` (padrão `~/.config/epinephrine/library.ini`),
com permissão de leitura e escrita apenas para o usuário. As configurações de
execução dos jogos continuam em `games.ini`.

- Importação automática de jogos a partir de:
  - **Steam** (parse de `libraryfolders.vdf`)
  - **Heroic Games Launcher**
  - **Lutris**
  - **Bottles** (`programs.yml` / diretório `programs`)
- Card por jogo com ícone, tempo de jogo, FPS médio, espaço para comandos de inicialização e botão de **play**.

### Hardware Control (Controle de Hardware)
- Layout de dois painéis com **divisor gradiente** usando a cor accent do tema.
- **GPU**: informações detalhadas, monitor de uso/temperatura, e gerenciamento por recurso detectado:
  - Performance level, power profile, power limit, Frame Rate Target Control
  - AMD Radeon Chill, Anti-Lag, Variable Rate Shading, Zero RPM fan
  - Controle de ventoinha, e gerenciamento NVIDIA (via `nvidia-smi`)
- **Placa-mãe**: fabricante/modelo, BIOS, chipset, SMBus.
- **RAM**: módulos (DIMMs), total/usado, tecnologia e frequência.
- **CPU**: governor, frequência min/máx, arquitetura, cores.
- **Teclado**: informação do dispositivo, numlock/capslock/scrolllock, repetição de teclas, layout e backlight.
- **Mouse**: velocidade, perfil de aceleração, scroll natural (libinput/gsettings/xinput) e configuração especial para dispositivos gamer como o mouse **Logitech G300s**:
  - Perfis on-board (F3/F4/F5), DPI, polling rate, cor de LED e remapeamento de botões (G1–G9).
  - Suporte a **ratbagd** via D-Bus API para configuração quando disponível.
  - Perfis de fábrica restauráveis (MMO, FPS, Produtividade).
- **Controles (joystick)**: teste em tempo real com visual de botões/analógicos (layouts padrão e arcade), indicadores de input, botões extras (M1–M4) detectados por `EVIOCGBIT`, e nome/URL vindos de banco de dados.

### Utilities (Utilitários)
- **Gamescope**: gerador visual de comando (`--nested`, resolução, refresh, HDR, scaler/filter, sharpness, VRR, backend, ReShade, e muito mais), com copiar/salvar/restaurar.
- **GameMode**: configuração de `gamemode.ini` (governors, renice, frequência CPU, scripts start/end, tweaks de desktop, etc.).
- **MangoHud**: configuração do overlay (FPS, temps, posição, tamanho de fonte, histograma, custom text, etc.).
- Se o utilitário não estiver instalado, o app oferece um botão de **instalação** usando o gerenciador de pacotes detectado (`apt`/`dnf`/`pacman`/`zypper`/`xbps`) via `pkexec`.

### Settings (Configurações)
- **Tema**: sistema, libadwaita ou Breeze.
- **Cor de destaque**: seguir a GPU detectada, ou forçar AMD/Intel/NVIDIA/Tux.
- **Idioma**: português do Brasil (`pt_BR`), inglês (`en_US`) e espanhol (`es_ES`).

### Sistema de Erros Padronizado
- Toast com resumo do erro e botão **"Details"**.
- Ao clicar em "Details", abre um overlay com:
  - Explicação detalhada do erro.
  - Output de terminal (quando disponível).
  - Botões **"Copy Error"** e **"OK"**.

### Banco de dados de hardware (INDEX)
O app carrega bancos de dados em Markdown (compilados como recursos) para exibir nome, URL oficial e specs dos dispositivos detectados:
- `database/GPU_INDEX.md` — specs, VRAM e data de lançamento de GPUs.
- `database/JOY_INDEX.md` — controles (nome, categoria, URL, features).
- `database/KEYBOARD_INDEX.md` — teclados.
- `database/MOUSE_INDEX.md` — mouses (inclui fabricante, ano de lançamento).

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
| `ratbagd` | Configuração de mice gamer via D-Bus |
| `pkexec` | Operações privilegiadas (instalação de utilitários, tuning) |
| `curl` | Refresh do banco usb.ids |

## Build

```sh
meson setup builddir
meson compile -C builddir
```

O binário será gerado em `builddir/src/epinephrine`.

Para executar diretamente do build:
```sh
./builddir/src/epinephrine
```

Consulte [BUILDING.md](BUILDING.md) para instruções detalhadas de build (incluindo builds com Address Sanitizer), instalação e limpeza.

## Estrutura do projeto

```
epinephrine/
├── meson.build                 # Configuração do Meson
├── data/                       # UI XML (GResources), ícones, fonts, desktop/metainfo
│   └── ui/                     # Páginas em formato GTK Builder (.ui)
├── database/                   # Bancos de dados INDEX (GPU, JOY, KEYBOARD, MOUSE)
├── lang/                       # Traduções (en_US.md, pt_BR.md, es_ES.md)
├── src/
│   ├── main.c, application.c   # Bootstrap da aplicação
│   ├── window.c                # Janela principal, abas e sistema de erros
│   ├── command-builder.c       # Gerador de comando do Gamescope
│   ├── epi-privilege.c         # Execução privilegiada via pkexec
│   ├── epi-ratbagd.c/h         # Integração com ratbagd via D-Bus
│   ├── epi-i18n.c/h            # Internacionalização
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
   - **Mouse**: `MOUSE_INDEX.md` (nome, VID:PID, URL, fabricante, ano de lançamento, número de botões).

### Traduções
Os arquivos de tradução estão em `lang/` como Markdown com tabelas key-text.
- Cada arquivo contém uma tabela com coluna `Chave` e `Texto`.
- Novas strings devem ser adicionadas a todos os arquivos de idioma.
- Strings do código usam `tr()` ou `epi_i18n_get()`.
- Strings em arquivos `.ui` são traduzidas via `epi_i18n_apply_widgets()`.

### Código
- Padrão da linguagem: **C17**.
- O código usa **GTK 4** com **Builder (.ui)** e **libadwaita** (widgets `Adw`).
- Mantenha o padrão das páginas existentes (cada página tem `page-<nome>.c`/`.h` e um arquivo `.ui` em `data/ui/`).
- Os INDEX files são parseados como recursos compilados (ver `load_controller_index` em `page-hardware.c` como referência).
- Erros devem usar `dashboard_show_error_toast(short_msg, details)` para padronização.

## Autor

**Renan Mayrinck** — [renanmayrinck.com](https://www.renanmayrinck.com) — [Apoie via LiberaPay](https://liberapay.com/RenanMayrinck/)

## Licença

MIT — veja o arquivo `meson.build` (`license: 'MIT'`). 
Creditos para: Renan Mayrinck, Emerson Lourenço.

### AI notice

This project was built with **Opencode**, an AI coding assistant. The code is fully functional, but may not follow the best practices adopted by professional developers. Contributions and suggestions are welcome via [pull requests or issues](https://github.com/mayrinck/epinephrine/issues).

### Organização do código da biblioteca

- `game-store.c`: leitura, gravação e migração de IDs.
- `game-importers.c`: importadores dos launchers e do sistema.
- `game-icons.c`: normalização dos ícones.
- `game-artwork.c` e `game-details.c`: artes e painel detalhado.
- `page-games.c`: filtros, seleção e views virtualizadas.

As operações administrativas agora dependem do auxiliar instalado `epi-admin`.
Consulte [BUILDING.md](BUILDING.md) para instalação e testes.
