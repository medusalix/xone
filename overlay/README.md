# xone Overlay (Linux Mint)

Companion em userspace para o driver [xone](../README.md): overlay estilo Game Bar no **Linux Mint / Cinnamon (X11)**.

Pressione o botão **Guide** (X central) do controle Xbox para mostrar ou esconder. Use o **✕** no painel ou `Esc` para fechar.

## Funcionalidades (v1)

- Relógio
- Bateria do notebook e do controle (UPower / sysfs — níveis Low/Normal/High/Full no controle)
- CPU, RAM e GPU (NVIDIA ou AMD, se disponível)
- Volume e mute do sink padrão (`pactl`)
- Captura de tela com menu: área, janela/app, monitor específico ou tela inteira (`gnome-screenshot`; pasta configurável)
- Controle do **OBS Studio** (gravar / stream) via WebSocket — com ajuda se não estiver instalado ou conectado
- Desligar o controle (sysfs `power_off` do xone — religa pelo botão Xbox)

## Instalação (recomendada)

O script principal do projeto já instala o overlay junto com o driver:

```bash
cd /caminho/para/xone
sudo ./install.sh
```

Isso (via [`install/overlay.sh`](../install/overlay.sh) + [`install/user-access.sh`](../install/user-access.sh)):

1. Instala as dependências apt do overlay
2. Copia o app para `/usr/local/share/xone-overlay`
3. Cria o comando `xone-overlay` e o serviço `systemd --user`
4. Adiciona seu usuário ao grupo `input` (e registra isso para o uninstall reverter)
5. Instala udev (dongle sem autosuspend + acesso `uaccess` ao controle)
6. Configura autostart em `~/.config/autostart/`

Depois faça **logout/login** (grupo `input`) — o overlay sobe sozinho.

Remoção: `sudo ./uninstall.sh` também remove o overlay.

### Dependências instaladas automaticamente

- `python3-gi`, `python3-gi-cairo`, `gir1.2-gtk-3.0`
- `gir1.2-upowerglib-1.0`, `python3-evdev`, `python3-websocket`
- `gnome-screenshot`
- `pulseaudio-utils` ou `pipewire-pulse` (para `pactl`)
- Opcional: **OBS Studio** (Flatpak ou apt) para gravação/stream

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

## Uso

O overlay precisa ficar em background (daemon). Com o install completo ele sobe no login via:

1. **systemd --user** (`xone-overlay.service`) — preferencial  
2. Autostart do Cinnamon (`~/.config/autostart/`) — fallback  

Não é preciso deixar um terminal aberto.

```bash
# subir agora (sem relogin)
systemctl --user enable --now xone-overlay

# status / logs
systemctl --user status xone-overlay
tail -f ~/.local/state/xone-overlay/overlay.log
```

**Importante:** depois do install, faça **logout/login** para o grupo `input` entrar na sessão. Sem isso o Guide pode falhar em alguns contextos (no terminal da sessão gráfica costuma funcionar via ACL do logind).

O processo fica escondido até o Guide ser pressionado.

## Solução de problemas

| Sintoma | Verificação |
|---------|-------------|
| Guide não abre o overlay | `groups` deve listar `input`; `python3 -c "import evdev"`; controle com xone |
| Controle sem bateria | `upower -e` / `ls /sys/class/power_supply/` |
| Sem volume | `pactl info` |
| Captura falha | `gnome-screenshot --version` |
| OBS não conecta | OBS aberto + WebSocket ativo; senha em Configurações do overlay; `python3-websocket` instalado |

## OBS (gravação / stream)

O overlay não grava vídeo sozinho: controla o **OBS Studio** via WebSocket (porta `4455`).

1. Instale o OBS (`flatpak install flathub com.obsproject.Studio` ou `sudo apt install obs-studio`)
2. Abra o OBS → **Ferramentas → Configurações do servidor WebSocket → Ativar**
3. Se houver senha, salve-a em **Configurações** do overlay
4. No painel Guide: **Gravar** / **Stream**

## Escopo

Foco em Linux Mint / Cinnamon. Sem FPS, sem mixer por aplicativo, sem mudanças no módulo kernel.
