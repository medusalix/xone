"""GTK overlay window and application lifecycle."""

from __future__ import annotations

import logging
import os
from datetime import datetime
from pathlib import Path
from typing import Optional, Tuple

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GdkPixbuf, Gio, GLib, Gtk, Pango

from . import audio, battery, config, controller, obs, performance, screenshot
from .guide_listener import GuideListener

log = logging.getLogger("xone-overlay.app")

APP_ID = "io.github.medusalix.xone.overlay"
REFRESH_MS = 1000

_LEVEL_FRACTION = {
    "Critical": 0.12,
    "Low": 0.28,
    "Normal": 0.55,
    "High": 0.78,
    "Full": 1.0,
}


def _data_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "data"


def _css_path() -> Path:
    return _data_dir() / "style.css"


def _state_dir() -> Path:
    base = os.environ.get("XDG_STATE_HOME")
    if base:
        path = Path(base) / "xone-overlay"
    else:
        path = Path.home() / ".local" / "state" / "xone-overlay"
    path.mkdir(parents=True, exist_ok=True)
    return path


def setup_logging() -> None:
    log_file = _state_dir() / "overlay.log"
    root = logging.getLogger()
    if root.handlers:
        return
    root.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(name)s %(levelname)s: %(message)s")
    fh = logging.FileHandler(log_file, encoding="utf-8")
    fh.setFormatter(fmt)
    sh = logging.StreamHandler()
    sh.setFormatter(fmt)
    root.addHandler(fh)
    root.addHandler(sh)
    log.info("logging to %s", log_file)


def _icon_image(name: str, size=Gtk.IconSize.BUTTON) -> Gtk.Image:
    return Gtk.Image.new_from_icon_name(name, size)


def _xbox_image(pixel_size: int = 22) -> Gtk.Image:
    """Load bundled Xbox mark, with icon-theme / gaming fallbacks."""
    svg = _data_dir() / "xbox.svg"
    if svg.is_file():
        try:
            pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_size(
                str(svg), pixel_size, pixel_size
            )
            return Gtk.Image.new_from_pixbuf(pixbuf)
        except GLib.Error as exc:
            log.warning("xbox.svg load failed: %s", exc)

    theme = Gtk.IconTheme.get_default()
    for name in ("xbox", "input-gaming", "applications-games"):
        if theme is not None and theme.has_icon(name):
            return Gtk.Image.new_from_icon_name(name, Gtk.IconSize.BUTTON)
    return Gtk.Image.new_from_icon_name("input-gaming", Gtk.IconSize.BUTTON)


class OverlayWindow(Gtk.Window):
    def __init__(self):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self.set_title("xone Overlay")
        self.set_decorated(False)
        self.set_keep_above(True)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_accept_focus(True)
        self.set_resizable(False)
        self.set_border_width(0)
        self.set_type_hint(Gdk.WindowTypeHint.DOCK)
        self.stick()
        self.set_app_paintable(True)
        self._enable_rgba()

        self._suppress_volume = False
        self._status_timeout = None
        self._settings = config.load()
        self._opacity_provider: Gtk.CssProvider | None = None
        self._opacity_suppress = False
        self._place_timeout_id = 0
        self._alloc_handler_id = 0
        self._placing = False
        self._position_busy = False
        self._position_buttons: list = []

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        root.get_style_context().add_class("overlay-root")
        self.add(root)

        accent = Gtk.Box()
        accent.get_style_context().add_class("overlay-accent")
        accent.set_size_request(-1, 3)
        root.pack_start(accent, False, False, 0)

        # Header — Xbox · brand · clock · settings · close
        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        header.get_style_context().add_class("overlay-header")
        root.pack_start(header, False, False, 0)

        brand_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        xbox = _xbox_image(22)
        xbox.set_valign(Gtk.Align.CENTER)
        brand_box.pack_start(xbox, False, False, 0)
        brand = Gtk.Label(label="xone")
        brand.get_style_context().add_class("brand")
        brand.set_xalign(0)
        brand_box.pack_start(brand, False, False, 0)
        brand_sub = Gtk.Label(label="Guide")
        brand_sub.get_style_context().add_class("brand-sub")
        brand_box.pack_start(brand_sub, False, False, 0)
        header.pack_start(brand_box, False, False, 0)

        self.clock_label = Gtk.Label(label="--:--")
        self.clock_label.get_style_context().add_class("clock")
        header.pack_start(self.clock_label, True, True, 0)

        close_btn = Gtk.Button()
        close_btn.set_image(_icon_image("window-close-symbolic"))
        close_btn.set_tooltip_text("Fechar (Esc)")
        close_btn.get_style_context().add_class("icon-btn")
        close_btn.set_relief(Gtk.ReliefStyle.NONE)
        close_btn.connect("clicked", lambda *_: self.hide_overlay())
        header.pack_end(close_btn, False, False, 0)

        settings_btn = Gtk.Button()
        settings_btn.set_image(_icon_image("preferences-system-symbolic"))
        settings_btn.set_tooltip_text("Configurações")
        settings_btn.get_style_context().add_class("icon-btn")
        settings_btn.set_relief(Gtk.ReliefStyle.NONE)
        settings_btn.connect("clicked", self._open_settings_dialog)
        header.pack_end(settings_btn, False, False, 0)

        body = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        body.get_style_context().add_class("overlay-body")
        self._body = body
        root.pack_start(body, False, False, 0)

        # Baterias
        batt = self._section("Bateria")
        self.laptop_bar, self.laptop_val = self._metric(batt, "PC")
        self.ctrl_bar, self.ctrl_val = self._metric(batt, "Controle")
        body.pack_start(batt, True, True, 0)

        # Desempenho
        perf = self._section("Desempenho")
        self.cpu_bar, self.cpu_val = self._metric(perf, "CPU")
        self.ram_bar, self.ram_val = self._metric(perf, "RAM")
        self.gpu_bar, self.gpu_val = self._metric(perf, "GPU")
        body.pack_start(perf, True, True, 0)

        # Áudio
        audio_sec = self._section("Áudio")
        audio_sec.get_style_context().add_class("audio-row")
        vol_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.mute_btn = Gtk.ToggleButton()
        self.mute_btn.set_image(_icon_image("audio-volume-high-symbolic"))
        self.mute_btn.set_tooltip_text("Mudo")
        self.mute_btn.get_style_context().add_class("icon-btn")
        self.mute_btn.set_relief(Gtk.ReliefStyle.NONE)
        self.mute_btn.connect("toggled", self._on_mute_toggled)
        vol_row.pack_start(self.mute_btn, False, False, 0)

        self.volume_scale = Gtk.Scale.new_with_range(
            Gtk.Orientation.HORIZONTAL, 0, 100, 1
        )
        self.volume_scale.set_draw_value(True)
        self.volume_scale.set_value_pos(Gtk.PositionType.RIGHT)
        self.volume_scale.set_hexpand(True)
        self.volume_scale.set_size_request(120, -1)
        self.volume_scale.connect("value-changed", self._on_volume_changed)
        vol_row.pack_start(self.volume_scale, True, True, 0)
        audio_sec.pack_start(vol_row, False, False, 0)
        hint = Gtk.Label(label="Sink padrão do sistema")
        hint.get_style_context().add_class("metric-hint")
        hint.set_xalign(0)
        audio_sec.pack_start(hint, False, False, 0)
        body.pack_start(audio_sec, True, True, 0)

        # Ação
        act = self._section("Ação")
        act.get_style_context().add_class("action-col")
        shot = Gtk.Button()
        shot_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        shot_box.set_halign(Gtk.Align.CENTER)
        shot_box.pack_start(_icon_image("camera-photo-symbolic"), False, False, 0)
        shot_box.pack_start(Gtk.Label(label="Captura de tela"), False, False, 0)
        shot.add(shot_box)
        shot.get_style_context().add_class("btn-primary")
        shot.set_tooltip_text(self._screenshot_tooltip())
        shot.connect("clicked", self._on_screenshot_menu)
        self._shot_btn = shot
        act.pack_start(shot, False, False, 0)

        power = Gtk.Button()
        power_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        power_box.set_halign(Gtk.Align.CENTER)
        power_box.pack_start(
            _icon_image("system-shutdown-symbolic"), False, False, 0
        )
        power_box.pack_start(Gtk.Label(label="Desligar controle"), False, False, 0)
        power.add(power_box)
        power.get_style_context().add_class("btn-secondary")
        power.set_tooltip_text(
            "Desliga o controle sem segurar o botão Xbox. "
            "Ligue de novo pelo botão Xbox."
        )
        power.connect("clicked", self._on_power_off_controller)
        act.pack_start(power, False, False, 0)

        self.status_label = Gtk.Label(label="")
        self.status_label.set_xalign(0)
        self.status_label.get_style_context().add_class("status")
        self.status_label.set_line_wrap(True)
        self.status_label.set_max_width_chars(28)
        act.pack_start(self.status_label, False, False, 0)
        body.pack_start(act, False, False, 0)

        # OBS — record / stream (controls existing OBS Studio via WebSocket)
        obs_sec = self._section("OBS")
        obs_sec.get_style_context().add_class("action-col")
        obs_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)

        self._obs_suppress = False
        self.obs_record_btn = Gtk.ToggleButton()
        rec_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        rec_box.set_halign(Gtk.Align.CENTER)
        rec_box.pack_start(_icon_image("media-record-symbolic"), False, False, 0)
        self._obs_record_label = Gtk.Label(label="Gravar")
        rec_box.pack_start(self._obs_record_label, False, False, 0)
        self.obs_record_btn.add(rec_box)
        self.obs_record_btn.get_style_context().add_class("btn-secondary")
        self.obs_record_btn.set_tooltip_text("Iniciar/parar gravação no OBS Studio")
        self.obs_record_btn.connect("toggled", self._on_obs_record_toggled)
        obs_row.pack_start(self.obs_record_btn, True, True, 0)

        self.obs_stream_btn = Gtk.ToggleButton()
        st_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        st_box.set_halign(Gtk.Align.CENTER)
        st_box.pack_start(_icon_image("network-wireless-symbolic"), False, False, 0)
        self._obs_stream_label = Gtk.Label(label="Stream")
        st_box.pack_start(self._obs_stream_label, False, False, 0)
        self.obs_stream_btn.add(st_box)
        self.obs_stream_btn.get_style_context().add_class("btn-secondary")
        self.obs_stream_btn.set_tooltip_text("Iniciar/parar transmissão no OBS Studio")
        self.obs_stream_btn.connect("toggled", self._on_obs_stream_toggled)
        obs_row.pack_start(self.obs_stream_btn, True, True, 0)

        obs_sec.pack_start(obs_row, False, False, 0)
        self.obs_hint = Gtk.Label(label="")
        self.obs_hint.set_xalign(0)
        self.obs_hint.set_line_wrap(True)
        self.obs_hint.set_max_width_chars(28)
        self.obs_hint.get_style_context().add_class("metric-hint")
        obs_sec.pack_start(self.obs_hint, False, False, 0)
        help_btn = Gtk.Button(label="Como configurar o OBS")
        help_btn.set_relief(Gtk.ReliefStyle.NONE)
        help_btn.get_style_context().add_class("icon-btn")
        help_btn.connect("clicked", self._on_obs_help)
        obs_sec.pack_start(help_btn, False, False, 0)
        body.pack_start(obs_sec, False, False, 0)

        self.connect("delete-event", self._on_delete)
        self.connect("key-press-event", self._on_key)
        self._apply_surface_opacity()
        self._apply_layout_orientation()
        self._prepare_window()

    def _enable_rgba(self) -> None:
        screen = self.get_screen()
        if screen is None:
            return
        visual = screen.get_rgba_visual()
        if visual is not None and screen.is_composited():
            self.set_visual(visual)

    def _apply_surface_opacity(self) -> None:
        """Paint panel background with configured alpha (text stays crisp)."""
        opacity = float(self._settings.get("opacity", config.DEFAULTS["opacity"]))
        opacity = max(config.OPACITY_MIN, min(config.OPACITY_MAX, opacity))
        # Header slightly denser so controls stay readable
        header_opacity = min(1.0, opacity + 0.06)
        css = f"""
        .overlay-root {{
          background-color: alpha(@theme_bg_color, {opacity:.3f});
        }}
        .overlay-header {{
          background-color: alpha(@theme_bg_color, {header_opacity:.3f});
        }}
        """
        provider = Gtk.CssProvider()
        try:
            provider.load_from_data(css.encode("utf-8"))
        except GLib.Error as exc:
            log.warning("opacity CSS failed: %s", exc)
            return

        screen = Gdk.Screen.get_default()
        if screen is None:
            return
        if self._opacity_provider is not None:
            Gtk.StyleContext.remove_provider_for_screen(
                screen, self._opacity_provider
            )
        Gtk.StyleContext.add_provider_for_screen(
            screen,
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1,
        )
        self._opacity_provider = provider
        self.queue_draw()

    def _open_settings_dialog(self, *_args) -> None:
        self._settings = config.load()

        dialog = Gtk.Dialog(
            title="Configurações do xone Overlay",
            transient_for=self,
            modal=True,
            destroy_with_parent=True,
        )
        dialog.set_resizable(False)
        dialog.set_default_size(420, -1)
        dialog.add_button("Fechar", Gtk.ResponseType.CLOSE)
        dialog.set_default_response(Gtk.ResponseType.CLOSE)
        dialog.get_style_context().add_class("settings-dialog")

        content = dialog.get_content_area()
        content.set_spacing(0)
        content.set_border_width(0)

        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        body.set_border_width(18)
        body.get_style_context().add_class("settings-dialog-body")
        content.pack_start(body, True, True, 0)

        # —— Posição ——
        pos_block = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        pos_title = Gtk.Label(label="Posição na tela")
        pos_title.get_style_context().add_class("settings-heading")
        pos_title.set_xalign(0)
        pos_block.pack_start(pos_title, False, False, 0)

        pos_help = Gtk.Label(
            label="Topo/Baixo: painel horizontal.  Esquerda/Direita: painel vertical."
        )
        pos_help.get_style_context().add_class("metric-hint")
        pos_help.set_xalign(0)
        pos_help.set_line_wrap(True)
        pos_block.pack_start(pos_help, False, False, 0)

        # Cross layout: top / left-right / bottom
        grid = Gtk.Grid()
        grid.set_row_spacing(8)
        grid.set_column_spacing(8)
        grid.set_column_homogeneous(True)
        grid.set_row_homogeneous(True)
        grid.get_style_context().add_class("position-grid")

        layout = {
            "top": (1, 0),
            "left": (0, 1),
            "right": (2, 1),
            "bottom": (1, 2),
        }
        short_labels = {
            "top": "⬆ Topo",
            "right": "➡ Direita",
            "bottom": "⬇ Baixo",
            "left": "⬅ Esquerda",
        }

        current = self._settings.get("position", "top")
        group = None
        self._position_buttons = []
        for key, (col, row) in layout.items():
            radio = Gtk.RadioButton.new_with_label_from_widget(
                group, short_labels[key]
            )
            if group is None:
                group = radio
            radio.set_mode(False)
            radio.get_style_context().add_class("position-choice")
            radio.set_tooltip_text(config.POSITION_LABELS[key])
            radio.set_active(key == current)
            radio.set_size_request(110, 44)
            radio.set_sensitive(not self._position_busy)
            radio.connect("toggled", self._on_position_toggled, key)
            grid.attach(radio, col, row, 1, 1)
            self._position_buttons.append(radio)

        pos_block.pack_start(grid, False, False, 0)
        body.pack_start(pos_block, False, False, 0)

        sep = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        body.pack_start(sep, False, False, 0)

        # —— Opacidade ——
        opac_block = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        opac_title = Gtk.Label(label="Opacidade do fundo")
        opac_title.get_style_context().add_class("settings-heading")
        opac_title.set_xalign(0)
        opac_block.pack_start(opac_title, False, False, 0)

        opac_help = Gtk.Label(
            label="Valores menores deixam o jogo mais visível atrás do painel."
        )
        opac_help.get_style_context().add_class("metric-hint")
        opac_help.set_xalign(0)
        opac_help.set_line_wrap(True)
        opac_block.pack_start(opac_help, False, False, 0)

        opacity_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        low = Gtk.Label(label="Transparente")
        low.get_style_context().add_class("metric-hint")
        high = Gtk.Label(label="Sólido")
        high.get_style_context().add_class("metric-hint")

        self._opacity_scale = Gtk.Scale.new_with_range(
            Gtk.Orientation.HORIZONTAL,
            int(config.OPACITY_MIN * 100),
            int(config.OPACITY_MAX * 100),
            1,
        )
        self._opacity_scale.set_draw_value(True)
        self._opacity_scale.set_value_pos(Gtk.PositionType.TOP)
        self._opacity_scale.set_hexpand(True)
        self._opacity_scale.set_value(
            int(round(float(self._settings.get("opacity", 0.90)) * 100))
        )
        self._opacity_scale.connect("value-changed", self._on_opacity_changed)

        opacity_row.pack_start(low, False, False, 0)
        opacity_row.pack_start(self._opacity_scale, True, True, 0)
        opacity_row.pack_start(high, False, False, 0)
        opac_block.pack_start(opacity_row, False, False, 0)
        body.pack_start(opac_block, False, False, 0)

        sep2 = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        body.pack_start(sep2, False, False, 0)

        # —— Capturas ——
        shot_block = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        shot_title = Gtk.Label(label="Pasta das capturas")
        shot_title.get_style_context().add_class("settings-heading")
        shot_title.set_xalign(0)
        shot_block.pack_start(shot_title, False, False, 0)

        shot_help = Gtk.Label(
            label="Onde salvar as imagens do botão Captura de tela."
        )
        shot_help.get_style_context().add_class("metric-hint")
        shot_help.set_xalign(0)
        shot_help.set_line_wrap(True)
        shot_block.pack_start(shot_help, False, False, 0)

        shot_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self._screenshot_dir_label = Gtk.Label(
            label=self._screenshot_dir_display()
        )
        self._screenshot_dir_label.set_xalign(0)
        self._screenshot_dir_label.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        self._screenshot_dir_label.set_hexpand(True)
        self._screenshot_dir_label.set_tooltip_text(
            str(config.resolve_screenshot_dir(self._settings.get("screenshot_dir")))
        )
        shot_row.pack_start(self._screenshot_dir_label, True, True, 0)

        browse = Gtk.Button(label="Escolher…")
        browse.set_tooltip_text("Selecionar pasta")
        browse.connect("clicked", self._on_pick_screenshot_dir, dialog)
        shot_row.pack_start(browse, False, False, 0)

        reset = Gtk.Button(label="Padrão")
        reset.set_tooltip_text("Voltar para ~/Pictures/Screenshots")
        reset.connect("clicked", self._on_reset_screenshot_dir)
        shot_row.pack_start(reset, False, False, 0)

        shot_block.pack_start(shot_row, False, False, 0)
        body.pack_start(shot_block, False, False, 0)

        sep3 = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        body.pack_start(sep3, False, False, 0)

        # —— OBS ——
        obs_block = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        obs_title = Gtk.Label(label="OBS Studio (gravação / stream)")
        obs_title.get_style_context().add_class("settings-heading")
        obs_title.set_xalign(0)
        obs_block.pack_start(obs_title, False, False, 0)

        obs_help = Gtk.Label(
            label="Controle remoto via WebSocket. Ative em: "
            "Ferramentas → Configurações do servidor WebSocket."
        )
        obs_help.get_style_context().add_class("metric-hint")
        obs_help.set_xalign(0)
        obs_help.set_line_wrap(True)
        obs_block.pack_start(obs_help, False, False, 0)

        host_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        host_row.pack_start(Gtk.Label(label="Host", xalign=0), False, False, 0)
        self._obs_host_entry = Gtk.Entry()
        self._obs_host_entry.set_text(str(self._settings.get("obs_host", "127.0.0.1")))
        self._obs_host_entry.set_hexpand(True)
        host_row.pack_start(self._obs_host_entry, True, True, 0)
        obs_block.pack_start(host_row, False, False, 0)

        port_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        port_row.pack_start(Gtk.Label(label="Porta", xalign=0), False, False, 0)
        self._obs_port_entry = Gtk.Entry()
        self._obs_port_entry.set_width_chars(6)
        self._obs_port_entry.set_text(str(self._settings.get("obs_port", 4455)))
        port_row.pack_start(self._obs_port_entry, False, False, 0)
        obs_block.pack_start(port_row, False, False, 0)

        pass_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        pass_row.pack_start(Gtk.Label(label="Senha", xalign=0), False, False, 0)
        self._obs_pass_entry = Gtk.Entry()
        self._obs_pass_entry.set_visibility(False)
        self._obs_pass_entry.set_input_purpose(Gtk.InputPurpose.PASSWORD)
        self._obs_pass_entry.set_text(str(self._settings.get("obs_password", "")))
        self._obs_pass_entry.set_hexpand(True)
        self._obs_pass_entry.set_placeholder_text("(se configurada no OBS)")
        pass_row.pack_start(self._obs_pass_entry, True, True, 0)
        obs_block.pack_start(pass_row, False, False, 0)

        save_obs = Gtk.Button(label="Salvar conexão OBS")
        save_obs.get_style_context().add_class("btn-secondary")
        save_obs.connect("clicked", self._on_save_obs_settings)
        obs_block.pack_start(save_obs, False, False, 0)
        body.pack_start(obs_block, False, False, 0)

        foot = Gtk.Label(label="Preferências salvas em ~/.config/xone-overlay/")
        foot.get_style_context().add_class("metric-hint")
        foot.set_xalign(0)
        foot.set_margin_top(4)
        body.pack_start(foot, False, False, 0)

        dialog.show_all()
        dialog.run()
        dialog.destroy()
        self._position_buttons = []
        self._screenshot_dir_label = None

    def _screenshot_dir_display(self) -> str:
        path = config.resolve_screenshot_dir(self._settings.get("screenshot_dir"))
        home = str(Path.home())
        text = str(path)
        if text.startswith(home + "/"):
            return "~/" + text[len(home) + 1 :]
        if text == home:
            return "~"
        return text

    def _screenshot_tooltip(self) -> str:
        return f"Salva em {self._screenshot_dir_display()}"

    def _refresh_screenshot_dir_ui(self) -> None:
        display = self._screenshot_dir_display()
        full = str(config.resolve_screenshot_dir(self._settings.get("screenshot_dir")))
        if getattr(self, "_screenshot_dir_label", None) is not None:
            self._screenshot_dir_label.set_text(display)
            self._screenshot_dir_label.set_tooltip_text(full)
        if getattr(self, "_shot_btn", None) is not None:
            self._shot_btn.set_tooltip_text(self._screenshot_tooltip())

    def _on_pick_screenshot_dir(self, _button, parent: Gtk.Window) -> None:
        chooser = Gtk.FileChooserDialog(
            title="Pasta das capturas",
            parent=parent,
            action=Gtk.FileChooserAction.SELECT_FOLDER,
        )
        chooser.add_buttons(
            "Cancelar",
            Gtk.ResponseType.CANCEL,
            "Selecionar",
            Gtk.ResponseType.ACCEPT,
        )
        current = config.resolve_screenshot_dir(self._settings.get("screenshot_dir"))
        if current.is_dir():
            chooser.set_current_folder(str(current))
        else:
            chooser.set_current_folder(str(Path.home()))

        response = chooser.run()
        folder = chooser.get_filename() if response == Gtk.ResponseType.ACCEPT else None
        chooser.destroy()
        if not folder:
            return
        self._settings = config.set_screenshot_dir(folder)
        log.info("screenshot dir → %s", self._settings.get("screenshot_dir"))
        self._refresh_screenshot_dir_ui()

    def _on_reset_screenshot_dir(self, *_args) -> None:
        self._settings = config.set_screenshot_dir("")
        log.info("screenshot dir → default")
        self._refresh_screenshot_dir_ui()

    def _on_save_obs_settings(self, *_args) -> None:
        host = self._obs_host_entry.get_text().strip() or "127.0.0.1"
        try:
            port = int(self._obs_port_entry.get_text().strip() or "4455")
        except ValueError:
            port = 4455
            self._obs_port_entry.set_text("4455")
        password = self._obs_pass_entry.get_text()
        self._settings = config.set_obs_connection(host, port, password)
        log.info("OBS connection → %s:%s", host, port)
        self.set_status("Conexão OBS salva")
        self._refresh_obs_status()

    def _show_obs_help_dialog(self, text: str) -> None:
        dialog = Gtk.MessageDialog(
            transient_for=self,
            modal=True,
            message_type=Gtk.MessageType.INFO,
            buttons=Gtk.ButtonsType.OK,
            text="OBS Studio",
        )
        dialog.format_secondary_text(text)
        dialog.run()
        dialog.destroy()

    def _on_obs_help(self, *_args) -> None:
        status = obs.get_status()
        self._show_obs_help_dialog(
            status.help_text or obs.install_help(installed=status.installed)
        )

    def _apply_obs_status(self, status: obs.ObsStatus) -> None:
        self._obs_suppress = True
        try:
            self.obs_record_btn.set_active(status.recording and status.connected)
            self.obs_stream_btn.set_active(status.streaming and status.connected)
            self._obs_record_label.set_text(
                "Gravando" if status.recording and status.connected else "Gravar"
            )
            self._obs_stream_label.set_text(
                "Ao vivo" if status.streaming and status.connected else "Stream"
            )
            hint = obs.short_help(status)
            self.obs_hint.set_text(hint)
            sensitive = True  # always clickable so user gets help on failure
            self.obs_record_btn.set_sensitive(sensitive)
            self.obs_stream_btn.set_sensitive(sensitive)
        finally:
            self._obs_suppress = False

    def _refresh_obs_status(self) -> None:
        # Avoid blocking the UI too often: run in idle
        def work():
            status = obs.get_status()
            self._apply_obs_status(status)
            return False

        GLib.idle_add(work)

    def _on_obs_record_toggled(self, button: Gtk.ToggleButton) -> None:
        if self._obs_suppress:
            return
        want = button.get_active()
        ok, msg, status = obs.set_recording(want)
        self._apply_obs_status(status)
        self.set_status(msg)
        if not ok and status.help_text:
            self._show_obs_help_dialog(status.help_text)

    def _on_obs_stream_toggled(self, button: Gtk.ToggleButton) -> None:
        if self._obs_suppress:
            return
        want = button.get_active()
        ok, msg, status = obs.set_streaming(want)
        self._apply_obs_status(status)
        self.set_status(msg)
        if not ok and status.help_text:
            self._show_obs_help_dialog(status.help_text)

    def _set_position_controls_sensitive(self, sensitive: bool) -> None:
        for btn in self._position_buttons:
            btn.set_sensitive(sensitive)

    def _finish_position_change(self) -> None:
        self._position_busy = False
        self._set_position_controls_sensitive(True)

    def _on_position_toggled(self, button: Gtk.RadioButton, position: str) -> None:
        if not button.get_active() or self._position_busy:
            return
        self._settings = config.set_position(position)
        log.info("overlay position → %s", position)
        self._position_busy = True
        self._set_position_controls_sensitive(False)
        self._apply_layout_orientation()
        if self.get_visible():
            self._schedule_place(
                retries=4, on_done=self._finish_position_change
            )
        else:
            self._finish_position_change()

    def _on_opacity_changed(self, scale: Gtk.Scale) -> None:
        if self._opacity_suppress:
            return
        opacity = float(scale.get_value()) / 100.0
        self._settings = config.set_opacity(opacity)
        self._apply_surface_opacity()

    def _section(self, title: str) -> Gtk.Box:
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.get_style_context().add_class("section")
        label = Gtk.Label(label=title.upper())
        label.get_style_context().add_class("section-title")
        label.set_xalign(0)
        box.pack_start(label, False, False, 0)
        return box

    def _metric(
        self, parent: Gtk.Box, name: str
    ) -> Tuple[Gtk.ProgressBar, Gtk.Label]:
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        name_l = Gtk.Label(label=name)
        name_l.set_xalign(0)
        name_l.set_width_chars(8)
        name_l.get_style_context().add_class("metric-name")
        bar = Gtk.ProgressBar()
        bar.set_fraction(0)
        bar.set_valign(Gtk.Align.CENTER)
        bar.set_hexpand(True)
        bar.set_size_request(72, -1)
        val = Gtk.Label(label="—")
        val.set_xalign(1)
        val.set_width_chars(5)
        val.get_style_context().add_class("metric-value")
        row.pack_start(name_l, False, False, 0)
        row.pack_start(bar, True, True, 0)
        row.pack_start(val, False, False, 0)
        parent.pack_start(row, False, False, 0)
        return bar, val

    @staticmethod
    def _set_bar(
        bar: Gtk.ProgressBar,
        value_label: Gtk.Label,
        fraction: Optional[float],
        text: str,
    ) -> None:
        ctx = bar.get_style_context()
        for cls in ("level-warn", "level-low"):
            ctx.remove_class(cls)
        if fraction is None:
            bar.set_fraction(0)
            value_label.set_text("—")
            return
        frac = max(0.0, min(1.0, fraction))
        bar.set_fraction(frac)
        value_label.set_text(text)
        if frac <= 0.2:
            ctx.add_class("level-low")
        elif frac <= 0.4:
            ctx.add_class("level-warn")

    def _battery_visual(self, info: battery.BatteryInfo) -> Tuple[Optional[float], str]:
        if not info.present:
            return None, "—"
        text = battery.format_battery(info)
        if info.percent is not None:
            return info.percent / 100.0, f"{info.percent}%"
        if info.level and info.level in _LEVEL_FRACTION:
            return _LEVEL_FRACTION[info.level], info.level
        return None, text

    def _monitor_geometry(self):
        display = Gdk.Display.get_default()
        if display is None:
            return None, None
        monitor = display.get_primary_monitor() or display.get_monitor(0)
        if monitor is None:
            return None, None
        return monitor, monitor.get_geometry()

    def _margin(self) -> int:
        margin = int(self._settings.get("margin", config.DEFAULTS["margin"]))
        return max(config.MARGIN_MIN, min(config.MARGIN_MAX, margin))

    def _is_vertical(self) -> bool:
        return config.is_vertical(str(self._settings.get("position", "top")))

    def _apply_layout_orientation(self) -> None:
        vertical = self._is_vertical()
        self._body.set_orientation(
            Gtk.Orientation.VERTICAL if vertical else Gtk.Orientation.HORIZONTAL
        )
        self._body.set_spacing(10 if vertical else 0)
        ctx = self._body.get_style_context()
        ctx.remove_class("overlay-body-horizontal")
        ctx.remove_class("overlay-body-vertical")
        ctx.add_class(
            "overlay-body-vertical" if vertical else "overlay-body-horizontal"
        )
        for child in self._body.get_children():
            self._body.set_child_packing(child, True, True, 0, Gtk.PackType.START)
        # Drop previous width locks so horizontal↔vertical can resize cleanly
        self.set_geometry_hints(None, Gdk.Geometry(), Gdk.WindowHints(0))
        self.set_size_request(-1, -1)
        self.queue_resize()

    def _panel_size(self, geo) -> Tuple[int, int]:
        """Return (width, max_height_hint). max_height_hint < 0 means natural height."""
        margin = self._margin()
        if self._is_vertical():
            width = min(300, max(240, geo.width // 5))
            width = min(width, geo.width - 2 * margin)
            max_h = max(200, geo.height - 2 * margin)
            return width, max_h
        width = min(980, max(320, geo.width - 2 * margin))
        return width, -1

    def _anchor_xy(self, geo, width: int, height: int) -> Tuple[int, int]:
        margin = self._margin()
        position = config.normalize_position(self._settings.get("position", "top"))

        if position == "top":
            x = geo.x + (geo.width - width) // 2
            y = geo.y + margin
        elif position == "bottom":
            x = geo.x + (geo.width - width) // 2
            y = geo.y + geo.height - height - margin
        elif position == "left":
            x = geo.x + margin
            y = geo.y + (geo.height - height) // 2
        else:  # right
            x = geo.x + geo.width - width - margin
            y = geo.y + (geo.height - height) // 2

        x = max(geo.x + margin, min(x, geo.x + geo.width - width - margin))
        y = max(geo.y + margin, min(y, geo.y + geo.height - height - margin))
        return x, y

    def _constrain_size(self, width: int, max_height: int) -> None:
        self.set_size_request(width, -1)
        hints = Gdk.Geometry()
        hints.min_width = width
        hints.max_width = width
        mask = Gdk.WindowHints.MIN_SIZE | Gdk.WindowHints.MAX_SIZE
        if max_height > 0:
            hints.max_height = max_height
        self.set_geometry_hints(None, hints, mask)
        self.resize(width, 1)

    def _cancel_scheduled_place(self) -> None:
        if self._place_timeout_id:
            GLib.source_remove(self._place_timeout_id)
            self._place_timeout_id = 0
        if self._alloc_handler_id:
            self.disconnect(self._alloc_handler_id)
            self._alloc_handler_id = 0

    def _schedule_place(self, retries: int = 4, on_done=None) -> None:
        """Re-anchor after GTK finishes reallocating the new layout.

        Rapid H↔V switches can report a stale size on the first pass; we place
        on idle and confirm with short delayed retries.
        """
        self._cancel_scheduled_place()
        state = {"n": 0, "max": max(1, retries)}

        def finish():
            self._place_timeout_id = 0
            if on_done is not None:
                on_done()
            return False

        def tick():
            self._place_timeout_id = 0
            self._place_on_monitor()
            state["n"] += 1
            if state["n"] < state["max"] and self.get_visible():
                self._place_timeout_id = GLib.timeout_add(20 + state["n"] * 30, tick)
            else:
                finish()
            return False

        self._place_timeout_id = GLib.idle_add(tick)

    def _place_on_monitor(
        self,
        known_w: Optional[int] = None,
        known_h: Optional[int] = None,
    ) -> None:
        if self._placing:
            return
        _monitor, geo = self._monitor_geometry()
        if geo is None:
            return

        self._placing = True
        try:
            width, max_h = self._panel_size(geo)
            self._constrain_size(width, max_h)

            while Gtk.events_pending():
                Gtk.main_iteration_do(False)

            # Prefer natural size — allocation often lags one frame behind
            # orientation changes and was centering with the previous H/V size.
            _min_req, nat_req = self.get_preferred_size()
            alloc = self.get_allocation()
            actual_w = known_w if known_w and known_w > 1 else 0
            actual_h = known_h if known_h and known_h > 1 else 0
            if actual_w <= 1:
                actual_w = nat_req.width if nat_req.width > 1 else alloc.width
            if actual_h <= 1:
                actual_h = nat_req.height if nat_req.height > 1 else alloc.height
            if actual_w <= 1:
                actual_w = width
            if actual_h <= 1:
                actual_h = 200 if self._is_vertical() else 120
            actual_w = min(actual_w, width)
            if max_h > 0:
                actual_h = min(actual_h, max_h)

            x, y = self._anchor_xy(geo, actual_w, actual_h)
            self.move(x, y)
        finally:
            self._placing = False

    def _prepare_window(self) -> None:
        _monitor, geo = self._monitor_geometry()
        if geo is None:
            return
        width, max_h = self._panel_size(geo)
        self._constrain_size(width, max_h)
        self.show_all()
        self.hide()

    def _on_delete(self, *_args):
        self.hide_overlay()
        return True

    def _on_key(self, _widget, event):
        if event.keyval == Gdk.KEY_Escape:
            self.hide_overlay()
            return True
        return False

    def show_overlay(self) -> None:
        self.refresh()
        self._apply_layout_orientation()
        self.show_all()
        self.present()
        self._schedule_place(retries=3)
        self._refresh_obs_status()

    def hide_overlay(self) -> None:
        self._cancel_scheduled_place()
        if self._position_busy:
            self._finish_position_change()
        self.hide()

    def toggle(self) -> None:
        if self.get_visible():
            self.hide_overlay()
        else:
            self.show_overlay()

    def set_status(self, text: str, timeout_ms: int = 4000) -> None:
        # Show basename for long paths
        if text.startswith("Salvo: "):
            path = text[7:]
            text = f"Salvo: {Path(path).name}"
        self.status_label.set_text(text)
        if self._status_timeout is not None:
            GLib.source_remove(self._status_timeout)
            self._status_timeout = None

        def clear():
            self.status_label.set_text("")
            self._status_timeout = None
            return False

        if timeout_ms > 0:
            self._status_timeout = GLib.timeout_add(timeout_ms, clear)

    def refresh(self) -> None:
        self.clock_label.set_text(datetime.now().strftime("%H:%M"))

        laptop = battery.get_laptop_battery()
        ctrl = battery.get_controller_battery()
        lf, lt = self._battery_visual(laptop)
        cf, ct = self._battery_visual(ctrl)
        self._set_bar(self.laptop_bar, self.laptop_val, lf, lt)
        self._set_bar(self.ctrl_bar, self.ctrl_val, cf, ct)

        perf = performance.get_performance()
        cpu = perf.cpu_percent
        ram = perf.ram_percent
        self._set_bar(
            self.cpu_bar,
            self.cpu_val,
            None if cpu is None else cpu / 100.0,
            "—" if cpu is None else f"{cpu:.0f}%",
        )
        self._set_bar(
            self.ram_bar,
            self.ram_val,
            None if ram is None else ram / 100.0,
            "—" if ram is None else f"{ram:.0f}%",
        )
        if perf.gpu_percent is not None:
            self.gpu_bar.get_parent().show()
            self._set_bar(
                self.gpu_bar,
                self.gpu_val,
                perf.gpu_percent / 100.0,
                f"{perf.gpu_percent:.0f}%",
            )
            self.gpu_bar.set_tooltip_text(perf.gpu_label or "GPU")
        else:
            self._set_bar(self.gpu_bar, self.gpu_val, None, "—")

        state = audio.get_audio_state()
        self._suppress_volume = True
        try:
            if state.available:
                self.volume_scale.set_sensitive(True)
                self.mute_btn.set_sensitive(True)
                self.volume_scale.set_value(min(100, state.volume_percent))
                self.mute_btn.set_active(state.muted)
                self._update_mute_icon(state.muted, state.volume_percent)
            else:
                self.volume_scale.set_sensitive(False)
                self.mute_btn.set_sensitive(False)
        finally:
            self._suppress_volume = False

    def _update_mute_icon(self, muted: bool, volume: int) -> None:
        if muted or volume <= 0:
            icon = "audio-volume-muted-symbolic"
        elif volume < 35:
            icon = "audio-volume-low-symbolic"
        elif volume < 70:
            icon = "audio-volume-medium-symbolic"
        else:
            icon = "audio-volume-high-symbolic"
        self.mute_btn.set_image(_icon_image(icon))

    def _on_volume_changed(self, scale):
        if self._suppress_volume:
            return
        vol = int(scale.get_value())
        audio.set_volume_percent(vol)
        self._update_mute_icon(self.mute_btn.get_active(), vol)

    def _on_mute_toggled(self, button):
        if self._suppress_volume:
            return
        muted = button.get_active()
        audio.set_mute(muted)
        self._update_mute_icon(muted, int(self.volume_scale.get_value()))

    def _screenshot_tooltip(self) -> str:
        return (
            f"Escolha área, janela, monitor ou tela inteira. "
            f"Salva em {self._screenshot_dir_display()}"
        )

    def _on_screenshot_menu(self, button: Gtk.Button) -> None:
        menu = Gtk.Menu()
        menu.get_style_context().add_class("screenshot-menu")

        def add_item(label: str, mode: str, monitor_index=None) -> None:
            item = Gtk.MenuItem(label=label)
            item.connect(
                "activate",
                lambda *_: self._start_screenshot(mode, monitor_index),
            )
            menu.append(item)

        add_item("Área da tela…", "area")
        add_item("Janela / aplicativo…", "window")

        monitors = screenshot.list_monitors()
        if len(monitors) > 1:
            sep = Gtk.SeparatorMenuItem()
            menu.append(sep)
            for mon in monitors:
                add_item(f"Monitor: {mon['label']}", "monitor", mon["index"])
        elif len(monitors) == 1:
            add_item(f"Este monitor ({monitors[0]['width']}×{monitors[0]['height']})", "monitor", monitors[0]["index"])

        sep2 = Gtk.SeparatorMenuItem()
        menu.append(sep2)
        add_item("Tela inteira (todos os monitores)", "fullscreen")

        menu.show_all()
        menu.popup_at_widget(
            button,
            Gdk.Gravity.SOUTH_WEST,
            Gdk.Gravity.NORTH_WEST,
            None,
        )

    def _start_screenshot(self, mode: str, monitor_index=None) -> None:
        was_visible = self.get_visible()
        if was_visible:
            self.hide()

        # Delay so the overlay is fully gone before interactive selection / capture
        delay_ms = 350 if mode in ("area", "window") else 200

        def capture():
            ok, msg = screenshot.take_screenshot(
                mode=mode, monitor_index=monitor_index
            )
            if was_visible:
                self.show_overlay()
            if ok:
                self.set_status(f"Salvo: {msg}")
            else:
                self.set_status(msg)
            return False

        GLib.timeout_add(delay_ms, capture)

    def _on_power_off_controller(self, *_args):
        dialog = Gtk.MessageDialog(
            transient_for=self,
            modal=True,
            message_type=Gtk.MessageType.QUESTION,
            buttons=Gtk.ButtonsType.NONE,
            text="Desligar o controle?",
        )
        dialog.format_secondary_text(
            "O controle apaga como se você segurasse o botão Xbox. "
            "Para ligar de novo, basta apertar o botão Xbox."
        )
        dialog.add_button("Cancelar", Gtk.ResponseType.CANCEL)
        dialog.add_button("Desligar", Gtk.ResponseType.ACCEPT)
        dialog.set_default_response(Gtk.ResponseType.ACCEPT)
        response = dialog.run()
        dialog.destroy()
        if response != Gtk.ResponseType.ACCEPT:
            return

        ok, msg = controller.power_off_controllers()
        self.set_status(msg)
        if ok:
            # Controller is gone — close the Guide panel
            GLib.timeout_add(400, lambda: (self.hide_overlay() or False))


class OverlayApp(Gtk.Application):
    def __init__(self):
        super().__init__(
            application_id=APP_ID,
            flags=Gio.ApplicationFlags.FLAGS_NONE,
        )
        self.window: OverlayWindow | None = None
        self.listener: GuideListener | None = None
        self._refresh_id = None
        self._css_provider: Gtk.CssProvider | None = None
        self.set_inactivity_timeout(0)

    def do_startup(self):
        Gtk.Application.do_startup(self)
        self.hold()
        self._load_css()
        self._watch_theme_changes()

    def do_activate(self):
        if self.window is None:
            self.window = OverlayWindow()
            self.add_window(self.window)
            self.listener = GuideListener(on_guide=self._on_guide)
            if self.listener.available:
                self.listener.start()
                log.info("Guide listener started")
            else:
                log.error("evdev unavailable — install python3-evdev")
            self._refresh_id = GLib.timeout_add(REFRESH_MS, self._tick)
            self.window.hide()
            log.info("overlay daemon ready (hidden); press Guide to toggle")

    def _watch_theme_changes(self) -> None:
        settings = Gtk.Settings.get_default()
        if settings is None:
            return
        for prop in (
            "gtk-theme-name",
            "gtk-application-prefer-dark-theme",
            "gtk-icon-theme-name",
        ):
            settings.connect(f"notify::{prop}", self._on_theme_setting_changed)
        theme = settings.get_property("gtk-theme-name")
        log.info("following system GTK theme: %s", theme)

    def _on_theme_setting_changed(self, settings, _pspec) -> None:
        theme = settings.get_property("gtk-theme-name")
        log.info("system theme changed → %s; reloading overlay styles", theme)
        self._load_css()
        if self.window is not None:
            self.window._apply_surface_opacity()
            self.window.queue_draw()

    def _load_css(self):
        css_file = _css_path()
        provider = Gtk.CssProvider()
        try:
            provider.load_from_path(str(css_file))
        except GLib.Error as exc:
            log.warning("CSS load failed: %s", exc)
            return

        screen = Gdk.Screen.get_default()
        if screen is None:
            return

        # Replace previous provider so theme swaps don't stack rules
        if self._css_provider is not None:
            Gtk.StyleContext.remove_provider_for_screen(screen, self._css_provider)

        Gtk.StyleContext.add_provider_for_screen(
            screen, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )
        self._css_provider = provider

    def _on_guide(self):
        GLib.idle_add(self._toggle_safe)

    def _toggle_safe(self):
        if self.window:
            self.window.toggle()
        return False

    def _tick(self):
        if self.window and self.window.get_visible():
            self.window.refresh()
        return True

    def do_shutdown(self):
        if self.listener:
            self.listener.stop()
        if self._refresh_id is not None:
            GLib.source_remove(self._refresh_id)
            self._refresh_id = None
        Gtk.Application.do_shutdown(self)


def run() -> int:
    setup_logging()
    app = OverlayApp()
    return app.run(None)
