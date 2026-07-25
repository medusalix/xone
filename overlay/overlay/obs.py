"""OBS Studio control via obs-websocket v5 (record / stream)."""

from __future__ import annotations

import base64
import hashlib
import json
import logging
import shutil
import subprocess
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Optional, Tuple

from . import config

log = logging.getLogger("xone-overlay.obs")

try:
    from websocket import WebSocketBadStatusException, WebSocketTimeoutException
    from websocket import create_connection as _ws_connect

    _HAS_WS = True
except ImportError:
    _ws_connect = None  # type: ignore
    WebSocketTimeoutException = OSError  # type: ignore
    WebSocketBadStatusException = OSError  # type: ignore
    _HAS_WS = False

OBS_FLATPAK_ID = "com.obsproject.Studio"


@dataclass
class ObsStatus:
    installed: bool = False
    connected: bool = False
    recording: bool = False
    streaming: bool = False
    error: str = ""
    help_text: str = ""


def _settings() -> Dict[str, Any]:
    data = config.load()
    host = str(data.get("obs_host") or config.DEFAULTS["obs_host"]).strip() or "127.0.0.1"
    try:
        port = int(data.get("obs_port", config.DEFAULTS["obs_port"]))
    except (TypeError, ValueError):
        port = int(config.DEFAULTS["obs_port"])
    password = str(data.get("obs_password") or "")
    return {"host": host, "port": port, "password": password}


def is_obs_installed() -> bool:
    if shutil.which("obs") or shutil.which("obs-studio"):
        return True
    if shutil.which("flatpak"):
        try:
            subprocess.check_call(
                ["flatpak", "info", OBS_FLATPAK_ID],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
            return True
        except (OSError, subprocess.SubprocessError):
            pass
    return False


def install_help(installed: bool = False) -> str:
    if not installed:
        return (
            "OBS não encontrado.\n\n"
            "Instale com um dos comandos:\n"
            f"  flatpak install flathub {OBS_FLATPAK_ID}\n"
            "  ou: sudo apt install obs-studio\n\n"
            "Depois abra o OBS e ative o WebSocket:\n"
            "  Ferramentas → Configurações do servidor WebSocket → Ativar"
        )
    return (
        "OBS instalado, mas não conectado ao WebSocket.\n\n"
        "1. Abra o OBS Studio\n"
        "2. Ferramentas → Configurações do servidor WebSocket → Ativar\n"
        "3. Se houver senha, informe-a em Configurações do xone Overlay\n"
        "4. Porta padrão: 4455 (localhost)\n\n"
        "No Flatpak o OBS precisa estar em execução para aceitar o controle."
    )


def short_help(status: ObsStatus) -> str:
    if not _HAS_WS:
        return "Instale: sudo apt install python3-websocket"
    if not status.installed:
        return "OBS não encontrado — veja como instalar"
    if not status.connected:
        return "OBS não conectado — abra e ative o WebSocket"
    return ""


def _auth_string(password: str, salt: str, challenge: str) -> str:
    secret = base64.b64encode(
        hashlib.sha256((password + salt).encode("utf-8")).digest()
    ).decode("ascii")
    return base64.b64encode(
        hashlib.sha256((secret + challenge).encode("utf-8")).digest()
    ).decode("ascii")


class _ObsClient:
    def __init__(self, host: str, port: int, password: str, timeout: float = 2.5):
        if not _HAS_WS:
            raise RuntimeError("python3-websocket não instalado")
        self._ws = _ws_connect(
            f"ws://{host}:{port}",
            timeout=timeout,
            enable_multithread=True,
        )
        hello = self._recv()
        if hello.get("op") != 0:
            self.close()
            raise RuntimeError("resposta Hello inválida do OBS")
        data = hello.get("d") or {}
        identify: Dict[str, Any] = {"rpcVersion": 1}
        auth = data.get("authentication")
        if auth:
            identify["authentication"] = _auth_string(
                password, auth.get("salt", ""), auth.get("challenge", "")
            )
        self._send(1, identify)
        identified = self._recv()
        if identified.get("op") != 2:
            self.close()
            raise RuntimeError("autenticação WebSocket falhou (senha?)")

    def _send(self, op: int, data: Dict[str, Any]) -> None:
        self._ws.send(json.dumps({"op": op, "d": data}))

    def _recv(self) -> Dict[str, Any]:
        raw = self._ws.recv()
        return json.loads(raw)

    def request(self, request_type: str, request_data: Optional[Dict] = None) -> Dict[str, Any]:
        req_id = str(uuid.uuid4())
        payload: Dict[str, Any] = {
            "requestType": request_type,
            "requestId": req_id,
        }
        if request_data:
            payload["requestData"] = request_data
        self._send(6, payload)
        while True:
            msg = self._recv()
            if msg.get("op") != 7:
                continue
            d = msg.get("d") or {}
            if d.get("requestId") != req_id:
                continue
            status = d.get("requestStatus") or {}
            if not status.get("result", False):
                code = status.get("code")
                comment = status.get("comment") or "pedido recusado"
                raise RuntimeError(f"OBS: {comment} ({code})")
            return d.get("responseData") or {}

    def close(self) -> None:
        try:
            self._ws.close()
        except Exception:
            pass


def _connect() -> Tuple[Optional[_ObsClient], ObsStatus]:
    status = ObsStatus(installed=is_obs_installed())
    if not _HAS_WS:
        status.error = "dependência ausente"
        status.help_text = (
            "Falta a biblioteca WebSocket do Python.\n\n"
            "Instale com:\n"
            "  sudo apt install python3-websocket\n"
            "Depois reinicie o overlay:\n"
            "  systemctl --user restart xone-overlay"
        )
        return None, status

    cfg = _settings()
    try:
        client = _ObsClient(cfg["host"], cfg["port"], cfg["password"])
        status.connected = True
        return client, status
    except Exception as exc:
        log.debug("OBS connect failed: %s", exc)
        status.error = str(exc)
        status.help_text = install_help(installed=status.installed)
        # Refine common cases
        err = str(exc).lower()
        if "auth" in err or "senha" in err or "authentication" in err:
            status.help_text = (
                "Senha do WebSocket incorreta ou ausente.\n\n"
                "Em OBS: Ferramentas → Configurações do servidor WebSocket "
                "(copie a senha).\n"
                "No overlay: Configurações → OBS → cole a senha."
            )
        elif not status.installed:
            status.help_text = install_help(installed=False)
        return None, status


def get_status() -> ObsStatus:
    client, status = _connect()
    if client is None:
        return status
    try:
        rec = client.request("GetRecordStatus")
        stream = client.request("GetStreamStatus")
        status.recording = bool(rec.get("outputActive"))
        status.streaming = bool(stream.get("outputActive"))
        return status
    except Exception as exc:
        status.connected = False
        status.error = str(exc)
        status.help_text = install_help(installed=status.installed)
        return status
    finally:
        client.close()


def _toggle(action_start: str, action_stop: str, active_key: str, want_active: bool) -> Tuple[bool, str, ObsStatus]:
    client, status = _connect()
    if client is None:
        return False, short_help(status) or status.error, status
    try:
        if active_key == "recording":
            cur = bool(client.request("GetRecordStatus").get("outputActive"))
        else:
            cur = bool(client.request("GetStreamStatus").get("outputActive"))
        if want_active and not cur:
            client.request(action_start)
        elif not want_active and cur:
            client.request(action_stop)
        # refresh
        rec = client.request("GetRecordStatus")
        stream = client.request("GetStreamStatus")
        status.recording = bool(rec.get("outputActive"))
        status.streaming = bool(stream.get("outputActive"))
        if active_key == "recording":
            msg = "Gravação iniciada" if status.recording else "Gravação parada"
        else:
            msg = "Stream iniciado" if status.streaming else "Stream parado"
        return True, msg, status
    except Exception as exc:
        log.warning("OBS action failed: %s", exc)
        status.connected = False
        status.error = str(exc)
        status.help_text = status.help_text or install_help(installed=status.installed)
        return False, str(exc), status
    finally:
        client.close()


def set_recording(active: bool) -> Tuple[bool, str, ObsStatus]:
    return _toggle("StartRecord", "StopRecord", "recording", active)


def set_streaming(active: bool) -> Tuple[bool, str, ObsStatus]:
    return _toggle("StartStream", "StopStream", "streaming", active)
