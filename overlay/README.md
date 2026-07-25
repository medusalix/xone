# xone Overlay (Linux Mint)

Companion em userspace para o driver [xone](../README.md): painel estilo Game Bar no **Linux Mint / Cinnamon (X11)**.

> **Suporte:** desenvolvido e testado no **Linux Mint** (Cinnamon + X11).  
> Em outras distribuições **base Ubuntu/Debian** pode funcionar, **mas não é garantido** — Wayland, outro desktop ou pacotes diferentes podem exigir ajustes.

Pressione o botão **Guide** (X central) do controle Xbox para mostrar ou esconder. Use o **✕** no painel ou `Esc` para fechar.

---

## O que este overlay adiciona

| Recurso | Detalhe |
|---------|---------|
| Relógio | Hora atual no cabeçalho |
| Baterias | Notebook + controle (UPower / sysfs; no controle: Low/Normal/High/Full) |
| Desempenho | CPU, RAM e GPU (NVIDIA ou AMD, se disponível) |
| Áudio | Volume e mute do sink padrão (`pactl`) |
| Captura de tela | Menu no momento da captura: **área**, **janela/app**, **monitor** ou **tela inteira** |
| Pasta das capturas | Configurável (padrão `~/Pictures/Screenshots`) |
| OBS Studio | **Gravar** / **Stream** via WebSocket (não grava dentro do overlay) |
| Desligar controle | Envia `power_off` pelo sysfs do xone; religa pelo botão Xbox |
| Aparência | Segue o tema GTK (ex.: Mint-Y); posição nas 4 bordas; opacidade do fundo |
| Autostart | `systemd --user` + fallback em `~/.config/autostart/` |

---

## Instalação (recomendada)

O script principal do projeto instala o overlay junto com o driver:

```bash
cd /caminho/para/xone
sudo ./install.sh
```

Isso (via [`install/overlay.sh`](../install/overlay.sh), [`install/user-access.sh`](../install/user-access.sh), [`install/apt-deps.sh`](../install/apt-deps.sh)):

1. Instala as dependências apt do overlay (**só as que ainda não estavam**; lista em `/var/lib/xone/apt-packages.list`)
2. Copia o app para `/usr/local/share/xone-overlay`
3. Cria o comando `xone-overlay` e o serviço `systemd --user`
4. Adiciona seu usuário ao grupo `input` (registrado para o uninstall reverter)
5. Instala udev (dongle sem autosuspend + acesso ao controle)
6. Configura autostart em `~/.config/autostart/`

Depois faça **logout/login** (grupo `input`) — o overlay sobe sozinho.

```bash
sudo ./uninstall.sh   # remove overlay, serviço, udev e pacotes apt rastreados pelo xone
```

### Dependências apt (instaladas pelo install)

- `python3-gi`, `python3-gi-cairo`, `gir1.2-gtk-3.0`
- `gir1.2-upowerglib-1.0`, `python3-evdev`, `python3-websocket`
- `gnome-screenshot`
- `pulseaudio-utils` ou `pipewire-pulse` (se faltar `pactl`)
- Opcional no sistema: **OBS Studio** (Flatpak ou apt) — o xone **não** instala o OBS

### Desenvolvimento local (sem `install.sh`)

```bash
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
  gir1.2-upowerglib-1.0 python3-evdev python3-websocket \
  pulseaudio-utils gnome-screenshot
sudo usermod -aG input "$USER"   # logout/login
cd overlay
python3 xone-overlay.py
# opcional:
./install-autostart.sh
```

---

## Uso

### Abrir o painel

1. Com o serviço ativo e o controle conectado (driver xone), pressione **Guide**.
2. Para fechar: **✕**, `Esc` ou Guide de novo.

```bash
# subir agora (sem relogin)
systemctl --user enable --now xone-overlay

# status / logs
systemctl --user status xone-overlay
tail -f ~/.local/state/xone-overlay/overlay.log
```

### Captura de tela

No botão **Captura de tela**, escolha:

- **Área da tela…** — seleção com o mouse  
- **Janela / aplicativo…** — clique na janela  
- **Monitor: …** — um monitor específico (útil com 2+ monitores)  
- **Tela inteira** — todos os monitores  

Pasta padrão: `~/Pictures/Screenshots` (alterável em **Configurações**).

### Desligar o controle

**Desligar controle** → confirmação → o controle apaga (mesmo efeito de segurar o botão Xbox).  
Para ligar de novo: apertar o botão Xbox (pareamento mantido).

### OBS (gravação / stream)

O overlay **não** grava vídeo sozinho; controla o OBS via WebSocket (`localhost:4455`).

1. Instale o OBS: `flatpak install flathub com.obsproject.Studio` ou `sudo apt install obs-studio`
2. Abra o OBS → **Ferramentas → Configurações do servidor WebSocket → Ativar**
3. Se houver senha, salve-a em **Configurações** do overlay
4. No painel: **Gravar** / **Stream**

Se o OBS não estiver instalado ou o WebSocket não responder, o overlay mostra uma mensagem com instruções de instalação/ativação.

### Configurações (ícone de engrenagem)

- **Posição** — Topo / Direita / Baixo / Esquerda (horizontal ou vertical)
- **Opacidade** do fundo do painel
- **Pasta das capturas**
- **OBS** — host, porta e senha do WebSocket

Preferências: `~/.config/xone-overlay/config.json`

---

## Solução de problemas

| Sintoma | Verificação |
|---------|-------------|
| Guide não abre o overlay | `groups` deve listar `input`; logout/login; `python3 -c "import evdev"`; controle com xone |
| Controle sem bateria | `upower -e` / `ls /sys/class/power_supply/` |
| Sem volume | `pactl info` |
| Captura falha | `gnome-screenshot --version` |
| OBS não conecta | OBS aberto + WebSocket ativo; senha nas configs; `python3-websocket` instalado |
| Dongle sem firmware | `ls /lib/firmware/xow_dongle.bin` ou `sudo xone-get-firmware.sh` |
| Painel desalinhado ao trocar posição | Troque de novo após o layout estabilizar; botões ficam bloqueados durante o repositionamento |

---

## Escopo e limitações

- Foco em **Linux Mint / Cinnamon / X11**
- Sem contador de FPS, sem mixer por aplicativo
- Gravação/stream dependem do **OBS** externo
- Outras distros Ubuntu-like: use por sua conta; reporte o ambiente se algo falhar
