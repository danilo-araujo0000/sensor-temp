from __future__ import annotations

import base64
import json
import os
import re
import sqlite3
import ssl
import threading
import time
import uuid
import mimetypes
import cv2
import subprocess
import shutil
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


HOST = "0.0.0.0"
PORT = 5080
COOLDOWN_SECONDS = 20
RECENT_MOTION_SECONDS = 60
DASHBOARD_BIN_SECONDS = 20
DASHBOARD_BIN_COUNT = 6
DETAIL_RANGE_SECONDS = {
    "1h": 60 * 60,
    "5h": 5 * 60 * 60,
    "12h": 12 * 60 * 60,
    "24h": 24 * 60 * 60,
}
DETAIL_BIN_COUNT = 12
APP_TIMEZONE = timezone(timedelta(hours=-3))
BASE_DIR = Path(__file__).resolve().parent
ENV_PATH = BASE_DIR / ".env"
DB_PATH = BASE_DIR / "sensor_events.db"
DASHBOARD_HTML_PATH = BASE_DIR / "templates" / "dashboard.html"
SENSOR_DETAIL_HTML_PATH = BASE_DIR / "templates" / "sensor_detail.html"
SENSOR_REPORT_HTML_PATH = BASE_DIR / "templates" / "sensor_report.html"
SNAPSHOTS_DIR = BASE_DIR / "snapshots"
SNAPSHOTS_DIR.mkdir(exist_ok=True)
SENSOR_ICONS_DIR = BASE_DIR / "sensor_icons"
SENSOR_ICONS_DIR.mkdir(exist_ok=True)


def load_env_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key:
            values[key] = value

    return values


ENV = load_env_file(ENV_PATH)


def env_value(name: str, default: str) -> str:
    return os.environ.get(name, ENV.get(name, default))


def env_int(name: str, default: int) -> int:
    try:
        return int(env_value(name, str(default)))
    except ValueError:
        return default


def env_bool(name: str, default: bool) -> bool:
    value = env_value(name, "true" if default else "false").strip().lower()
    return value in {"1", "true", "yes", "on", "sim", "ativo", "enabled"}


SNAPSHOTS_CLEANUP_ENABLED = env_bool("SNAPSHOTS_CLEANUP_ENABLED", True)
SNAPSHOTS_CLEANUP_ON_START = env_bool("SNAPSHOTS_CLEANUP_ON_START", True)
SNAPSHOTS_CLEANUP_INTERVAL_SECONDS = max(env_int("SNAPSHOTS_CLEANUP_INTERVAL_SECONDS", 3600), 60)
SNAPSHOTS_RETENTION_DAYS = env_int("SNAPSHOTS_RETENTION_DAYS", 30)
SNAPSHOTS_MAX_SIZE_MB = env_int("SNAPSHOTS_MAX_SIZE_MB", 2048)
SNAPSHOT_MEDIA_EXTENSIONS = {".mp4", ".jpg", ".jpeg", ".png"}
AUTOMATION_LOOP_SECONDS = max(env_int("AUTOMATION_LOOP_SECONDS", 30), 5)
EXTERNAL_API_BASE_URL = env_value("EXTERNAL_API_BASE_URL", "https://retro.tdanilo.com").rstrip("/")
EXTERNAL_API_TOKEN = env_value("EXTERNAL_API_TOKEN", "")
EXTERNAL_API_TIMEOUT_SECONDS = max(env_int("EXTERNAL_API_TIMEOUT_SECONDS", 15), 3)
EXTERNAL_API_INSECURE_TLS = env_bool("EXTERNAL_API_INSECURE_TLS", False)
EXTERNAL_API_DEVICE_CACHE_SECONDS = max(env_int("EXTERNAL_API_DEVICE_CACHE_SECONDS", 300), 30)
DEVICE_HEARTBEAT_TIMEOUT_SECONDS = max(env_int("DEVICE_HEARTBEAT_TIMEOUT_SECONDS", 10), 3)
MODERATOR_API_BASE_URL = env_value("MODERATOR_API_BASE_URL", "").rstrip("/")
MODERATOR_API_USERNAME = env_value("MODERATOR_API_USERNAME", "")
MODERATOR_API_PASSWORD = env_value("MODERATOR_API_PASSWORD", "")
MODERATOR_API_TIMEOUT_SECONDS = max(env_int("MODERATOR_API_TIMEOUT_SECONDS", 15), 3)


def resolve_ffmpeg_bin() -> str:
    ffmpeg_path = shutil.which("ffmpeg")
    if ffmpeg_path:
        return ffmpeg_path

    winget_packages = Path.home() / "AppData" / "Local" / "Microsoft" / "WinGet" / "Packages"
    for candidate in winget_packages.glob("Gyan.FFmpeg_*/*/bin/ffmpeg.exe"):
        return str(candidate)

    return "ffmpeg"


FFMPEG_BIN = resolve_ffmpeg_bin()
EXTERNAL_API_SSL_CONTEXT = ssl._create_unverified_context() if EXTERNAL_API_INSECURE_TLS else ssl.create_default_context()
EXTERNAL_DEVICE_CACHE = {"fetched_at": 0.0, "devices": [], "error": None}
EXTERNAL_DEVICE_CACHE_LOCK = threading.Lock()
MODERATOR_COOKIE_HEADER = ""
MODERATOR_COOKIE_LOCK = threading.Lock()


def app_now() -> datetime:
    return datetime.now(APP_TIMEZONE)


def ensure_column(conn: sqlite3.Connection, table: str, column: str, definition: str) -> None:
    existing = {row[1] for row in conn.execute(f"PRAGMA table_info({table})").fetchall()}
    if column not in existing:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {definition}")


def init_db() -> None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS sensors (
                sensor_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                pin INTEGER NOT NULL,
                registered_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            )
            """
        )
        ensure_column(conn, "sensors", "name", "TEXT")
        ensure_column(conn, "sensors", "icon", "TEXT")
        ensure_column(conn, "sensors", "enabled", "INTEGER NOT NULL DEFAULT 1")
        ensure_column(conn, "sensors", "rtsp_url", "TEXT")
        ensure_column(conn, "sensors", "schedule_mode", "TEXT DEFAULT 'always'")
        ensure_column(conn, "sensors", "schedule_start", "TEXT DEFAULT '00:00'")
        ensure_column(conn, "sensors", "schedule_end", "TEXT DEFAULT '23:59'")
        ensure_column(conn, "sensors", "schedule_days", "TEXT DEFAULT '0,1,2,3,4,5,6'")
        ensure_column(conn, "sensors", "schedule_alternate", "TEXT DEFAULT 'none'")
        ensure_column(conn, "sensors", "show_on_dashboard", "INTEGER NOT NULL DEFAULT 1")
        ensure_column(conn, "sensors", "automation_motion_enabled", "INTEGER NOT NULL DEFAULT 0")
        ensure_column(conn, "sensors", "automation_motion_device_id", "TEXT")
        ensure_column(conn, "sensors", "automation_motion_action", "TEXT DEFAULT 'power'")
        ensure_column(conn, "sensors", "automation_motion_state", "TEXT DEFAULT 'ON'")
        ensure_column(conn, "sensors", "automation_idle_enabled", "INTEGER NOT NULL DEFAULT 0")
        ensure_column(conn, "sensors", "automation_idle_minutes", "INTEGER NOT NULL DEFAULT 10")
        ensure_column(conn, "sensors", "automation_idle_device_id", "TEXT")
        ensure_column(conn, "sensors", "automation_idle_action", "TEXT DEFAULT 'power'")
        ensure_column(conn, "sensors", "automation_idle_state", "TEXT DEFAULT 'OFF'")
        ensure_column(conn, "sensors", "automation_idle_triggered_epoch", "INTEGER")
        ensure_column(conn, "sensors", "automation_last_event", "TEXT")
        ensure_column(conn, "sensors", "automation_last_status", "TEXT")
        ensure_column(conn, "sensors", "automation_last_response", "TEXT")
        ensure_column(conn, "sensors", "automation_last_run_at", "TEXT")
        ensure_column(conn, "sensors", "automation_enabled", "INTEGER NOT NULL DEFAULT 0")
        ensure_column(conn, "sensors", "automation_trigger", "TEXT DEFAULT 'motion'")
        ensure_column(conn, "sensors", "automation_device_id", "TEXT")
        ensure_column(conn, "sensors", "automation_action", "TEXT DEFAULT 'power'")
        ensure_column(conn, "sensors", "automation_state", "TEXT DEFAULT 'ON'")
        ensure_column(conn, "sensors", "automation_repeat_mode", "TEXT DEFAULT 'once'")
        ensure_column(conn, "sensors", "automation_repeat_seconds", "INTEGER NOT NULL DEFAULT 30")
        ensure_column(conn, "sensors", "automation_last_trigger_epoch", "INTEGER")
        ensure_column(conn, "sensors", "automation_last_condition_key", "TEXT")
        ensure_column(conn, "sensors", "automation_notify_enabled", "INTEGER NOT NULL DEFAULT 0")
        ensure_column(conn, "sensors", "automation_notify_contact_ids", "TEXT")
        ensure_column(conn, "sensors", "automation_notify_message", "TEXT")
        ensure_column(conn, "sensors", "device_last_seen_at", "TEXT")
        ensure_column(conn, "sensors", "device_last_seen_epoch", "INTEGER")
        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_sensors_device
            ON sensors(device_id)
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS movements (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                sensor_id TEXT NOT NULL,
                device_id TEXT NOT NULL,
                occurred_at TEXT NOT NULL,
                occurred_epoch INTEGER NOT NULL,
                FOREIGN KEY(sensor_id) REFERENCES sensors(sensor_id)
            )
            """
        )
        ensure_column(conn, "movements", "image_path", "TEXT")
        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_movements_sensor_time
            ON movements(sensor_id, occurred_epoch DESC)
            """
        )
        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_movements_device_time
            ON movements(device_id, occurred_epoch DESC)
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS contacts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                phone TEXT NOT NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            )
            """
        )
        conn.execute(
            """
            CREATE UNIQUE INDEX IF NOT EXISTS idx_contacts_phone
            ON contacts(phone)
            """
        )
        conn.commit()


def default_sensor_name(sensor_id: str) -> str:
    key = sensor_id.split(":")[-1]
    mapping = {
        "mov_entrada": "Entrada Principal",
        "mov_sala": "Sala de Estar",
        "mov_corredor": "Corredor",
        "mov_garagem": "Garagem",
        "mov_quintal": "Quintal",
    }
    if key in mapping:
        return mapping[key]
    return key.replace("_", " ").title()


def default_sensor_icon(sensor_id: str) -> str:
    key = sensor_id.split(":")[-1]
    mapping = {
        "mov_entrada": "entry",
        "mov_sala": "sofa",
        "mov_corredor": "hall",
        "mov_garagem": "garage",
        "mov_quintal": "garden",
    }
    return mapping.get(key, "motion")


ICON_BUILTIN_NAMES = {"entry", "sofa", "hall", "garage", "garden", "motion"}
ICON_UPLOAD_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg"}


def sanitize_sensor_icon_value(icon_value: object, sensor_id: str) -> str:
    raw = str(icon_value or "").strip()
    if not raw:
        return default_sensor_icon(sensor_id)
    return raw


def infer_icon_kind(icon_value: str, sensor_id: str) -> str:
    normalized = sanitize_sensor_icon_value(icon_value, sensor_id)
    if normalized.startswith("fa:"):
        return "fa"
    if normalized.startswith("upload:"):
        return "upload"
    return "builtin"


def sensor_icon_url(icon_value: str, sensor_id: str) -> str:
    normalized = sanitize_sensor_icon_value(icon_value, sensor_id)
    if normalized.startswith("upload:"):
        return f"/sensor-icons/{normalized.replace('upload:', '', 1)}"
    return ""


def delete_uploaded_sensor_icon(icon_value: str) -> None:
    if not icon_value.startswith("upload:"):
        return
    filename = icon_value.replace("upload:", "", 1).strip()
    if not filename:
        return
    path = SENSOR_ICONS_DIR / Path(filename).name
    try:
        if path.exists() and path.is_file():
            path.unlink()
    except OSError as exc:
        print(f"Falha ao remover icone enviado {path}: {exc}", flush=True)


def store_uploaded_sensor_icon(sensor_id: str, file_name: str, file_base64: str) -> str:
    name = Path(str(file_name or "").strip()).name
    extension = Path(name).suffix.lower()
    if extension not in ICON_UPLOAD_EXTENSIONS:
        raise ValueError("icon_extension_not_allowed")

    try:
        binary = base64.b64decode(str(file_base64 or ""), validate=True)
    except (ValueError, TypeError):
        raise ValueError("invalid_icon_base64")

    if not binary:
        raise ValueError("empty_icon_file")
    if len(binary) > 2 * 1024 * 1024:
        raise ValueError("icon_file_too_large")

    safe_sensor = re.sub(r"[^a-zA-Z0-9_-]+", "_", sensor_id)[:80] or "sensor"
    filename = f"{safe_sensor}_{int(time.time())}{extension}"
    path = SENSOR_ICONS_DIR / filename
    path.write_bytes(binary)
    return f"upload:{filename}"


def fetch_sensor(sensor_id: str) -> sqlite3.Row | None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        row = conn.execute(
            """
            SELECT sensor_id, device_id, pin, name, icon, enabled, registered_at, updated_at, rtsp_url,
                   schedule_mode, schedule_start, schedule_end, schedule_days, schedule_alternate, show_on_dashboard,
                   automation_motion_enabled, automation_motion_device_id, automation_motion_action,
                   automation_motion_state, automation_idle_enabled, automation_idle_minutes,
                   automation_idle_device_id, automation_idle_action, automation_idle_state,
                   automation_idle_triggered_epoch, automation_last_event, automation_last_status,
                   automation_last_response, automation_last_run_at, automation_enabled,
                   automation_trigger, automation_device_id, automation_action, automation_state,
                   automation_repeat_mode, automation_repeat_seconds, automation_last_trigger_epoch,
                   automation_last_condition_key, automation_notify_enabled, automation_notify_contact_ids,
                   automation_notify_message, device_last_seen_at, device_last_seen_epoch
            FROM sensors
            WHERE sensor_id = ?
            """,
            (sensor_id,),
        ).fetchone()
    return row


def fetch_last_movement(sensor_id: str) -> tuple[int, str, str] | None:
    with sqlite3.connect(DB_PATH) as conn:
        row = conn.execute(
            """
            SELECT occurred_epoch, occurred_at, image_path
            FROM movements
            WHERE sensor_id = ?
            ORDER BY occurred_epoch DESC
            LIMIT 1
            """,
            (sensor_id,),
        ).fetchone()
    return row if row else None


def coerce_bool(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "on", "yes", "enabled", "ativo"}
    return False


def sanitize_action(action: object, default: str = "power") -> str:
    normalized = str(action or default).strip().lower()
    return normalized if normalized in {"power", "motion"} else default


def sanitize_state_for_action(action: str, state: object, default: str) -> str:
    normalized = str(state or default).strip().upper()
    valid_states = {
        "power": {"ON", "OFF", "1", "0"},
        "motion": {"DETECTED", "NOT_DETECTED", "ON", "OFF", "1", "0"},
    }
    return normalized if normalized in valid_states[action] else default


def sanitize_trigger(trigger: object, default: str = "motion") -> str:
    normalized = str(trigger or default).strip().lower()
    return normalized if normalized in {"motion", "idle"} else default


def sanitize_repeat_mode(mode: object, default: str = "once") -> str:
    normalized = str(mode or default).strip().lower()
    return normalized if normalized in {"once", "repeat"} else default


def external_api_ready() -> bool:
    return bool(EXTERNAL_API_BASE_URL and EXTERNAL_API_TOKEN)


def is_device_online(sensor: sqlite3.Row | dict, now_epoch: int | None = None) -> bool:
    sensor_dict = dict(sensor)
    last_seen_epoch = sensor_dict.get("device_last_seen_epoch")
    if last_seen_epoch is None:
        return False
    reference_epoch = now_epoch if now_epoch is not None else int(app_now().timestamp())
    return reference_epoch - int(last_seen_epoch) <= DEVICE_HEARTBEAT_TIMEOUT_SECONDS


def mark_device_heartbeat(device_id: str) -> dict:
    normalized = device_id.strip()
    if not normalized:
        raise ValueError("device_id_required")

    now = app_now()
    now_iso = now.isoformat()
    now_epoch = int(now.timestamp())

    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.execute(
            """
            UPDATE sensors
            SET device_last_seen_at = ?,
                device_last_seen_epoch = ?,
                updated_at = updated_at
            WHERE device_id = ?
            """,
            (now_iso, now_epoch, normalized),
        )
        conn.commit()

    return {
        "device_id": normalized,
        "updated_sensors": cursor.rowcount,
        "last_seen_at": now_iso,
        "last_seen_epoch": now_epoch,
    }


def update_sensor_automation_status(
    sensor_id: str,
    *,
    event_name: str,
    status: str,
    response_text: str,
    idle_triggered_epoch: int | None = None,
    last_trigger_epoch: int | None = None,
    condition_key: str | None = None,
) -> None:
    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET automation_last_event = ?,
                automation_last_status = ?,
                automation_last_response = ?,
                automation_last_run_at = ?,
                automation_idle_triggered_epoch = COALESCE(?, automation_idle_triggered_epoch),
                automation_last_trigger_epoch = COALESCE(?, automation_last_trigger_epoch),
                automation_last_condition_key = COALESCE(?, automation_last_condition_key),
                updated_at = ?
            WHERE sensor_id = ?
            """,
            (
                event_name,
                status[:32],
                response_text[:500],
                agora,
                idle_triggered_epoch,
                last_trigger_epoch,
                condition_key,
                agora,
                sensor_id,
            ),
        )
        conn.commit()


def reset_idle_trigger(sensor_id: str) -> None:
    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET automation_idle_triggered_epoch = NULL,
                automation_last_trigger_epoch = NULL,
                automation_last_condition_key = NULL,
                updated_at = ?
            WHERE sensor_id = ?
            """,
            (agora, sensor_id),
        )
        conn.commit()


def external_api_request(path: str, method: str = "GET", payload: dict | None = None) -> dict:
    requires_auth = path.startswith("/api/")
    if requires_auth and not external_api_ready():
        raise RuntimeError("external_api_not_configured")

    body = None
    headers = {}
    if requires_auth:
        headers["Authorization"] = f"Bearer {EXTERNAL_API_TOKEN}"
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = Request(f"{EXTERNAL_API_BASE_URL}{path}", data=body, headers=headers, method=method)
    try:
        with urlopen(request, timeout=EXTERNAL_API_TIMEOUT_SECONDS, context=EXTERNAL_API_SSL_CONTEXT) as response:
            raw = response.read().decode("utf-8")
            content_type = response.headers.get("Content-Type", "")
            parsed = json.loads(raw) if "json" in content_type.lower() and raw else raw
            return {"status": response.status, "body": parsed, "text": raw}
    except HTTPError as exc:
        error_text = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"http_{exc.code}:{error_text[:300]}") from exc
    except URLError as exc:
        raise RuntimeError(f"network_error:{exc.reason}") from exc


def moderator_api_ready() -> bool:
    return bool(MODERATOR_API_BASE_URL and MODERATOR_API_USERNAME and MODERATOR_API_PASSWORD)


def moderator_api_request(path: str, method: str = "GET", payload: dict | None = None, *, use_cookie: bool = True) -> dict:
    if not MODERATOR_API_BASE_URL:
        raise RuntimeError("moderator_api_not_configured")

    body = None if payload is None else json.dumps(payload).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if use_cookie and MODERATOR_COOKIE_HEADER:
        headers["Cookie"] = MODERATOR_COOKIE_HEADER

    request = Request(
        f"{MODERATOR_API_BASE_URL}{path}",
        data=body,
        method=method,
        headers=headers,
    )
    try:
        with urlopen(request, timeout=MODERATOR_API_TIMEOUT_SECONDS) as response:
            text = response.read().decode("utf-8", errors="replace")
            return {
                "status": response.status,
                "text": text,
                "headers": response.headers,
            }
    except HTTPError as exc:
        text = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"moderator_http_{exc.code}: {text[:300]}") from exc
    except URLError as exc:
        raise RuntimeError(f"moderator_network_error: {exc.reason}") from exc


def moderator_login(force: bool = False) -> None:
    global MODERATOR_COOKIE_HEADER
    if not moderator_api_ready():
        raise RuntimeError("moderator_api_not_configured")

    with MODERATOR_COOKIE_LOCK:
        if MODERATOR_COOKIE_HEADER and not force:
            return
        response = moderator_api_request(
            "/api/auth/login",
            "POST",
            {
                "username": MODERATOR_API_USERNAME,
                "password": MODERATOR_API_PASSWORD,
            },
            use_cookie=False,
        )
        cookies = response["headers"].get_all("Set-Cookie") if hasattr(response["headers"], "get_all") else []
        cookie_pairs = []
        for cookie in cookies or []:
            cookie_pairs.append(cookie.split(";", 1)[0])
        if not cookie_pairs:
            raise RuntimeError("moderator_login_without_cookie")
        MODERATOR_COOKIE_HEADER = "; ".join(cookie_pairs)


def get_contacts_by_ids(contact_ids: list[int]) -> list[dict]:
    if not contact_ids:
        return []
    unique_ids = sorted(set(int(item) for item in contact_ids))
    placeholders = ",".join("?" for _ in unique_ids)
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            f"""
            SELECT id, name, phone
            FROM contacts
            WHERE id IN ({placeholders})
            ORDER BY name COLLATE NOCASE ASC, id ASC
            """,
            unique_ids,
        ).fetchall()
    return [dict(row) for row in rows]


def build_notification_message(sensor: sqlite3.Row | dict, event_name: str) -> str:
    sensor_dict = dict(sensor)
    template = str(sensor_dict.get("automation_notify_message") or "").strip()
    sensor_name = str(sensor_dict.get("name") or default_sensor_name(sensor_dict["sensor_id"]))
    event_label = "movimento detectado" if event_name == "motion" else "sem movimento"
    if not template:
        template = "Alerta: {event} no sensor {sensor_name}."
    return template.format(
        event=event_label,
        sensor_name=sensor_name,
        sensor_id=sensor_dict["sensor_id"],
        device_id=sensor_dict.get("device_id", ""),
    )


def notify_contacts(sensor: sqlite3.Row | dict, event_name: str) -> dict:
    sensor_dict = dict(sensor)
    if not bool(sensor_dict.get("automation_notify_enabled")):
        return {"ok": False, "reason": "notification_disabled"}
    raw_ids = str(sensor_dict.get("automation_notify_contact_ids") or "")
    contact_ids = [int(item) for item in raw_ids.split(",") if item.strip().isdigit()]
    contacts = get_contacts_by_ids(contact_ids)
    if not contacts:
        return {"ok": False, "reason": "notification_contacts_not_configured"}
    if not moderator_api_ready():
        return {"ok": False, "reason": "moderator_api_not_configured"}

    message = build_notification_message(sensor, event_name)
    moderator_login()
    sent = []
    errors = []
    for contact in contacts:
        payload = {
            "chatId": str(contact["phone"]),
            "text": message,
        }
        try:
            response = moderator_api_request("/api/messages/send", "POST", payload)
            sent.append({"contact_id": contact["id"], "phone": contact["phone"], "status": response["status"]})
        except RuntimeError as exc:
            if "moderator_http_401" in str(exc):
                try:
                    moderator_login(force=True)
                    response = moderator_api_request("/api/messages/send", "POST", payload)
                    sent.append({"contact_id": contact["id"], "phone": contact["phone"], "status": response["status"]})
                    continue
                except RuntimeError as retry_exc:
                    errors.append({"contact_id": contact["id"], "error": str(retry_exc)})
            else:
                errors.append({"contact_id": contact["id"], "error": str(exc)})

    return {
        "ok": bool(sent) and not errors,
        "sent": sent,
        "errors": errors,
        "message": message,
    }


def fetch_external_devices(force: bool = False) -> dict:
    now_ts = time.time()
    with EXTERNAL_DEVICE_CACHE_LOCK:
        if (
            not force
            and EXTERNAL_DEVICE_CACHE["devices"]
            and now_ts - float(EXTERNAL_DEVICE_CACHE["fetched_at"]) < EXTERNAL_API_DEVICE_CACHE_SECONDS
        ):
            return {
                "ok": True,
                "devices": EXTERNAL_DEVICE_CACHE["devices"],
                "cached": True,
                "base_url": EXTERNAL_API_BASE_URL,
            }

    devices: list[dict] = []
    try:
        doc_response = external_api_request("/doc/api.json", "GET")
        doc_body = doc_response["body"] if isinstance(doc_response["body"], dict) else {}
        doc_devices = doc_body.get("devices", [])
        capability_index: dict[str, dict] = {}
        for capability in doc_body.get("capabilities", []):
            if not isinstance(capability, dict):
                continue
            capability_index[str(capability.get("id") or "").strip()] = capability

        for item in doc_devices:
            if not isinstance(item, dict):
                continue
            device_id = str(item.get("id") or item.get("device_id") or "").strip()
            if not device_id:
                continue
            capability = capability_index.get(device_id, {})
            supported = capability.get("supported", {}) if isinstance(capability, dict) else {}
            action_states = {}
            for action_name, action_value in supported.items():
                if action_name not in {"power", "motion"}:
                    continue
                if isinstance(action_value, list):
                    action_states[action_name] = [str(value).upper() for value in action_value]

            devices.append(
                {
                    "device_id": device_id,
                    "name": str(item.get("friendly_name") or item.get("name") or device_id),
                    "type": str(item.get("type") or "device"),
                    "description": str(item.get("description") or ""),
                    "supported_actions": sorted(action_states.keys()),
                    "supported_states": action_states,
                }
            )
    except Exception:
        response = external_api_request("/api/devices", "GET")
        raw_body = response["body"]
        if isinstance(raw_body, dict):
            raw_devices = raw_body.get("devices", raw_body.get("items", []))
        elif isinstance(raw_body, list):
            raw_devices = raw_body
        else:
            raw_devices = []

        for item in raw_devices:
            if not isinstance(item, dict):
                continue
            device_id = str(item.get("id") or item.get("device_id") or "").strip()
            if not device_id:
                continue
            device_type = str(item.get("type") or item.get("category") or "device")
            supported_actions = []
            supported_states = {}
            if device_type in {"light", "plug"}:
                supported_actions.append("power")
                supported_states["power"] = ["ON", "OFF"]
            if device_type == "motion":
                supported_actions.append("motion")
                supported_states["motion"] = ["DETECTED", "NOT_DETECTED"]
            devices.append(
                {
                    "device_id": device_id,
                    "name": str(item.get("friendly_name") or item.get("name") or device_id),
                    "type": device_type,
                    "description": str(item.get("description") or ""),
                    "supported_actions": supported_actions,
                    "supported_states": supported_states,
                }
            )

    with EXTERNAL_DEVICE_CACHE_LOCK:
        EXTERNAL_DEVICE_CACHE["fetched_at"] = now_ts
        EXTERNAL_DEVICE_CACHE["devices"] = devices
        EXTERNAL_DEVICE_CACHE["error"] = None

    return {
        "ok": True,
        "devices": devices,
        "cached": False,
        "base_url": EXTERNAL_API_BASE_URL,
    }


def trigger_external_device(sensor: sqlite3.Row | dict, event_name: str) -> dict:
    sensor_dict = dict(sensor)
    enabled = bool(sensor_dict.get("automation_enabled"))
    configured_trigger = sanitize_trigger(sensor_dict.get("automation_trigger"), "motion")
    action = sanitize_action(sensor_dict.get("automation_action"), "power")
    default_state = "ON" if configured_trigger == "motion" else "OFF"
    state = sanitize_state_for_action(action, sensor_dict.get("automation_state"), default_state)
    target_device_id = str(sensor_dict.get("automation_device_id") or "").strip()

    if not enabled:
        return {"ok": False, "reason": "automation_disabled"}
    if configured_trigger != event_name:
        return {"ok": False, "reason": "event_not_configured"}
    if not target_device_id:
        return {"ok": False, "reason": "device_not_configured"}
    if not external_api_ready():
        return {"ok": False, "reason": "external_api_not_configured"}

    endpoint = f"/api/device/{target_device_id}/{action}"
    payload: dict[str, object] = {"state": state}
    response = external_api_request(endpoint, "POST", payload)
    return {
        "ok": True,
        "device_id": target_device_id,
        "action": action,
        "state": state,
        "status": response["status"],
        "response": response["text"][:500],
    }

def list_snapshot_media_files() -> list[Path]:
    if not SNAPSHOTS_DIR.exists():
        return []
    return [
        path
        for path in SNAPSHOTS_DIR.iterdir()
        if path.is_file() and path.suffix.lower() in SNAPSHOT_MEDIA_EXTENSIONS
    ]


def clear_deleted_media_references(deleted_names: list[str]) -> None:
    video_names = [name for name in deleted_names if name.lower().endswith(".mp4")]
    if not video_names:
        return

    placeholders = ",".join("?" for _ in video_names)
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            f"UPDATE movements SET image_path = NULL WHERE image_path IN ({placeholders})",
            video_names,
        )
        conn.commit()


def cleanup_snapshots() -> dict:
    now_ts = time.time()
    retention_seconds = SNAPSHOTS_RETENTION_DAYS * 24 * 60 * 60
    max_bytes = SNAPSHOTS_MAX_SIZE_MB * 1024 * 1024
    deleted_names: list[str] = []
    bytes_deleted = 0

    def remove_file(path: Path) -> None:
        nonlocal bytes_deleted
        try:
            size = path.stat().st_size
            path.unlink()
            bytes_deleted += size
            deleted_names.append(path.name)
        except FileNotFoundError:
            pass
        except OSError as exc:
            print(f"Falha ao remover snapshot {path.name}: {exc}", flush=True)

    if SNAPSHOTS_RETENTION_DAYS > 0:
        for path in list_snapshot_media_files():
            try:
                age_seconds = now_ts - path.stat().st_mtime
            except FileNotFoundError:
                continue
            if age_seconds > retention_seconds:
                remove_file(path)

    files_with_size: list[tuple[Path, int, float]] = []
    total_bytes = 0
    for path in list_snapshot_media_files():
        try:
            stat = path.stat()
        except FileNotFoundError:
            continue
        files_with_size.append((path, stat.st_size, stat.st_mtime))
        total_bytes += stat.st_size

    if SNAPSHOTS_MAX_SIZE_MB > 0 and total_bytes > max_bytes:
        for path, size, _mtime in sorted(files_with_size, key=lambda item: item[2]):
            if total_bytes <= max_bytes:
                break
            remove_file(path)
            total_bytes -= size

    clear_deleted_media_references(deleted_names)

    remaining_files = list_snapshot_media_files()
    remaining_bytes = 0
    for path in remaining_files:
        try:
            remaining_bytes += path.stat().st_size
        except FileNotFoundError:
            pass

    return {
        "ok": True,
        "enabled": SNAPSHOTS_CLEANUP_ENABLED,
        "retention_days": SNAPSHOTS_RETENTION_DAYS,
        "max_size_mb": SNAPSHOTS_MAX_SIZE_MB,
        "files_deleted": len(deleted_names),
        "bytes_deleted": bytes_deleted,
        "remaining_files": len(remaining_files),
        "remaining_bytes": remaining_bytes,
    }


def cleanup_snapshots_worker() -> None:
    while True:
        time.sleep(SNAPSHOTS_CLEANUP_INTERVAL_SECONDS)
        try:
            result = cleanup_snapshots()
            if result["files_deleted"]:
                print(
                    f"Limpeza de snapshots: {result['files_deleted']} arquivos removidos",
                    flush=True,
                )
        except Exception as exc:
            print(f"Erro na limpeza de snapshots: {exc}", flush=True)


def start_snapshot_cleanup() -> None:
    if not SNAPSHOTS_CLEANUP_ENABLED:
        print("Limpeza de snapshots desativada por .env", flush=True)
        return

    if SNAPSHOTS_CLEANUP_ON_START:
        try:
            result = cleanup_snapshots()
            print(
                "Limpeza inicial de snapshots: "
                f"{result['files_deleted']} arquivos removidos, "
                f"{result['remaining_files']} restantes",
                flush=True,
            )
        except Exception as exc:
            print(f"Erro na limpeza inicial de snapshots: {exc}", flush=True)

    thread = threading.Thread(target=cleanup_snapshots_worker, daemon=True)
    thread.start()


def capture_and_save_frame(movement_id: int, rtsp_url: str) -> None:
    try:
        filename = f"clip_{movement_id}_{uuid.uuid4().hex[:8]}.mp4"
        filepath = SNAPSHOTS_DIR / filename
        
        ffmpeg_cmd = [
            FFMPEG_BIN, "-y",
            "-rtsp_transport", "tcp",
            "-use_wallclock_as_timestamps", "1",
            "-i", rtsp_url,
            "-t", "21",
            "-c:v", "libx264",
            "-preset", "superfast",
            "-crf", "28",
            "-pix_fmt", "yuv420p",
            "-r", "15",
            "-an",
            str(filepath),
            "-vframes", "1",
            "-q:v", "2",
            str(filepath).replace(".mp4", ".jpg")
        ]
        
        subprocess.run(ffmpeg_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=40)
        
        if filepath.exists() and filepath.stat().st_size > 0:
            with sqlite3.connect(DB_PATH) as conn:
                conn.execute("UPDATE movements SET image_path = ? WHERE id = ?", (filename, movement_id))
                conn.commit()
    except Exception as e:
        print(f"RTSP FFmpeg Capture Error: {e}", flush=True)


def upsert_sensors(payload: dict) -> dict:
    device_id = str(payload.get("device_id", "")).strip()
    sensores = payload.get("sensors")

    if not device_id:
        raise ValueError("device_id_required")
    if not isinstance(sensores, list) or not sensores:
        raise ValueError("sensors_required")

    agora = app_now().isoformat()
    registrados = 0

    with sqlite3.connect(DB_PATH) as conn:
        for sensor in sensores:
            sensor_id = str(sensor.get("sensor_id", "")).strip()
            pin = sensor.get("pin")
            name = str(sensor.get("name") or default_sensor_name(sensor_id))
            icon = str(sensor.get("icon") or default_sensor_icon(sensor_id))
            enabled = coerce_bool(sensor.get("enabled", True))
            show_on_dashboard = coerce_bool(sensor.get("show_on_dashboard", True))

            if not sensor_id:
                raise ValueError("sensor_id_required")
            if pin is None:
                raise ValueError("pin_required")

            conn.execute(
                """
                INSERT INTO sensors (sensor_id, device_id, pin, name, icon, enabled, show_on_dashboard, registered_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(sensor_id) DO UPDATE SET
                    device_id = excluded.device_id,
                    pin = excluded.pin,
                    name = excluded.name,
                    icon = excluded.icon,
                    updated_at = excluded.updated_at
                """,
                (sensor_id, device_id, int(pin), name, icon, 1 if enabled else 0, 1 if show_on_dashboard else 0, agora, agora),
            )
            registrados += 1
        conn.commit()

    return {
        "registered": True,
        "device_id": device_id,
        "count": registrados,
    }


def set_sensor_enabled(sensor_id: str, enabled: bool) -> dict | None:
    if fetch_sensor(sensor_id) is None:
        return None

    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET enabled = ?, updated_at = ?
            WHERE sensor_id = ?
            """,
            (1 if enabled else 0, agora, sensor_id),
        )
        conn.commit()

    sensor = fetch_sensor(sensor_id)
    return {
        "sensor_id": sensor["sensor_id"],
        "enabled": bool(sensor["enabled"]),
        "updated_at": sensor["updated_at"],
    }

def set_sensor_dashboard_visibility(sensor_id: str, show: bool) -> dict | None:
    if fetch_sensor(sensor_id) is None:
        return None

    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET show_on_dashboard = ?, updated_at = ?
            WHERE sensor_id = ?
            """,
            (1 if show else 0, agora, sensor_id),
        )
        conn.commit()

    sensor = fetch_sensor(sensor_id)
    return {
        "sensor_id": sensor["sensor_id"],
        "enabled": bool(sensor["enabled"]),
        "updated_at": sensor["updated_at"],
    }


def update_sensor_name(sensor_id: str, name: str) -> dict | None:
    if fetch_sensor(sensor_id) is None:
        return None

    normalized = name.strip()
    if not normalized:
        raise ValueError("name_required")

    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET name = ?, updated_at = ?
            WHERE sensor_id = ?
            """,
            (normalized, agora, sensor_id),
        )
        conn.commit()

    sensor = fetch_sensor(sensor_id)
    return {
        "sensor_id": sensor["sensor_id"],
        "name": sensor["name"],
        "updated_at": sensor["updated_at"],
    }


def update_sensor_icon(sensor_id: str, icon_value: str) -> dict | None:
    sensor = fetch_sensor(sensor_id)
    if sensor is None:
        return None

    normalized = sanitize_sensor_icon_value(icon_value, sensor_id)
    agora = app_now().isoformat()
    previous_icon = str(sensor["icon"] or "").strip()

    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET icon = ?, updated_at = ?
            WHERE sensor_id = ?
            """,
            (normalized, agora, sensor_id),
        )
        conn.commit()

    if previous_icon != normalized:
        delete_uploaded_sensor_icon(previous_icon)

    sensor = fetch_sensor(sensor_id)
    resolved_icon = sanitize_sensor_icon_value(sensor["icon"], sensor_id)
    return {
        "sensor_id": sensor["sensor_id"],
        "icon": resolved_icon,
        "icon_kind": infer_icon_kind(resolved_icon, sensor_id),
        "icon_url": sensor_icon_url(resolved_icon, sensor_id),
        "updated_at": sensor["updated_at"],
    }


def delete_sensor(sensor_id: str) -> dict | None:
    sensor = fetch_sensor(sensor_id)
    if sensor is None:
        return None

    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        image_rows = conn.execute(
            """
            SELECT image_path
            FROM movements
            WHERE sensor_id = ? AND image_path IS NOT NULL AND image_path != ''
            """,
            (sensor_id,),
        ).fetchall()
        cursor = conn.execute("DELETE FROM movements WHERE sensor_id = ?", (sensor_id,))
        movements_deleted = cursor.rowcount
        conn.execute("DELETE FROM sensors WHERE sensor_id = ?", (sensor_id,))
        conn.commit()

    media_deleted = 0
    for row in image_rows:
        media_path = SNAPSHOTS_DIR / row["image_path"]
        for path in (media_path, media_path.with_suffix(".jpg")):
            try:
                if path.exists() and path.is_file():
                    path.unlink()
                    media_deleted += 1
            except OSError as exc:
                print(f"Falha ao remover midia {path}: {exc}", flush=True)

    return {
        "sensor_id": sensor_id,
        "movements_deleted": movements_deleted,
        "media_deleted": media_deleted,
    }


def update_sensor_schedule(payload: dict) -> dict | None:
    sensor_id = str(payload.get("sensor_id", "")).strip()
    if fetch_sensor(sensor_id) is None:
        return None
        
    mode = str(payload.get("schedule_mode", "always"))
    start = str(payload.get("schedule_start", "00:00"))
    end = str(payload.get("schedule_end", "23:59"))
    days = str(payload.get("schedule_days", "0,1,2,3,4,5,6"))
    alt = str(payload.get("schedule_alternate", "none"))
    
    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET schedule_mode=?, schedule_start=?, schedule_end=?, schedule_days=?, schedule_alternate=?, updated_at=?
            WHERE sensor_id=?
            """,
            (mode, start, end, days, alt, agora, sensor_id)
        )
        conn.commit()
    return dict(fetch_sensor(sensor_id))


def update_sensor_automation(payload: dict) -> dict | None:
    sensor_id = str(payload.get("sensor_id", "")).strip()
    if fetch_sensor(sensor_id) is None:
        return None

    automation_enabled = coerce_bool(payload.get("automation_enabled", False))
    automation_trigger = sanitize_trigger(payload.get("automation_trigger"), "motion")
    automation_device_id = str(payload.get("automation_device_id", "")).strip()
    automation_action = sanitize_action(payload.get("automation_action"), "power")
    default_state = "ON" if automation_trigger == "motion" else "OFF"
    automation_state = sanitize_state_for_action(automation_action, payload.get("automation_state"), default_state)
    automation_repeat_mode = sanitize_repeat_mode(payload.get("automation_repeat_mode"), "once")
    automation_notify_enabled = coerce_bool(payload.get("automation_notify_enabled", False))
    raw_contact_ids = payload.get("automation_notify_contact_ids", [])
    if isinstance(raw_contact_ids, list):
        contact_ids: list[int] = []
        for item in raw_contact_ids:
            try:
                contact_ids.append(int(item))
            except (TypeError, ValueError):
                continue
        automation_notify_contact_ids = ",".join(str(contact_id) for contact_id in sorted(set(contact_ids)))
    else:
        automation_notify_contact_ids = ""
    automation_notify_message = str(payload.get("automation_notify_message", "") or "").strip()
    try:
        automation_idle_minutes = max(1, int(payload.get("automation_idle_minutes", 10)))
    except (TypeError, ValueError):
        automation_idle_minutes = 10
    try:
        automation_repeat_seconds = max(1, int(payload.get("automation_repeat_seconds", 30)))
    except (TypeError, ValueError):
        automation_repeat_seconds = 30

    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE sensors
            SET automation_motion_enabled = ?,
                automation_motion_device_id = ?,
                automation_motion_action = ?,
                automation_motion_state = ?,
                automation_idle_enabled = ?,
                automation_idle_minutes = ?,
                automation_idle_device_id = ?,
                automation_idle_action = ?,
                automation_idle_state = ?,
                automation_idle_triggered_epoch = NULL,
                automation_enabled = ?,
                automation_trigger = ?,
                automation_device_id = ?,
                automation_action = ?,
                automation_state = ?,
                automation_repeat_mode = ?,
                automation_repeat_seconds = ?,
                automation_notify_enabled = ?,
                automation_notify_contact_ids = ?,
                automation_notify_message = ?,
                automation_last_trigger_epoch = NULL,
                automation_last_condition_key = NULL,
                updated_at = ?
            WHERE sensor_id = ?
            """,
            (
                0,
                "",
                "power",
                "ON",
                0,
                automation_idle_minutes,
                "",
                "power",
                "OFF",
                1 if automation_enabled else 0,
                automation_trigger,
                automation_device_id,
                automation_action,
                automation_state,
                automation_repeat_mode,
                automation_repeat_seconds,
                1 if automation_notify_enabled else 0,
                automation_notify_contact_ids,
                automation_notify_message,
                agora,
                sensor_id,
            ),
        )
        conn.commit()

    updated = fetch_sensor(sensor_id)
    return dict(updated) if updated else None


def should_run_automation(sensor: sqlite3.Row | dict, event_name: str, condition_key: str, now_epoch: int) -> bool:
    sensor_dict = dict(sensor)
    if not bool(sensor_dict.get("automation_enabled")):
        return False
    if sanitize_trigger(sensor_dict.get("automation_trigger"), "motion") != event_name:
        return False

    repeat_mode = sanitize_repeat_mode(sensor_dict.get("automation_repeat_mode"), "once")
    last_condition_key = str(sensor_dict.get("automation_last_condition_key") or "")
    last_trigger_epoch = sensor_dict.get("automation_last_trigger_epoch")

    if repeat_mode == "once":
        return last_condition_key != condition_key

    try:
        repeat_seconds = max(1, int(sensor_dict.get("automation_repeat_seconds") or 30))
    except (TypeError, ValueError):
        repeat_seconds = 30

    if last_condition_key != condition_key:
        return True
    if last_trigger_epoch is None:
        return True
    return now_epoch - int(last_trigger_epoch) >= repeat_seconds


def evaluate_and_run_sensor_automation(
    sensor: sqlite3.Row | dict,
    event_name: str,
    now: datetime,
    *,
    force: bool = False,
) -> dict:
    sensor_dict = dict(sensor)
    schedule_allowed, _reason = evaluate_sensor_schedule(sensor, now)
    if not schedule_allowed:
        return {"ok": False, "reason": "schedule_blocked"}

    now_epoch = int(now.timestamp())
    if event_name == "idle" and not is_device_online(sensor_dict, now_epoch):
        return {"ok": False, "reason": "device_offline"}

    last_motion = fetch_last_movement(sensor_dict["sensor_id"])
    if last_motion is None:
        return {"ok": False, "reason": "no_motion_history"}

    last_motion_epoch = int(last_motion[0])

    if event_name == "motion":
        if now_epoch - last_motion_epoch > RECENT_MOTION_SECONDS:
            return {"ok": False, "reason": "condition_inactive"}
        condition_key = f"motion:{last_motion_epoch}"
    else:
        idle_threshold = max(1, int(sensor_dict.get("automation_idle_minutes") or 10)) * 60
        if now_epoch - last_motion_epoch < idle_threshold:
            return {"ok": False, "reason": "condition_inactive"}
        condition_key = f"idle:{last_motion_epoch}"

    if not force and not should_run_automation(sensor, event_name, condition_key, now_epoch):
        return {"ok": False, "reason": "repeat_window_active"}

    device_result = trigger_external_device(sensor, event_name)
    notify_result = notify_contacts(sensor, event_name)
    actionable_results = [
        item
        for item in (device_result, notify_result)
        if item.get("reason") not in {"device_not_configured", "external_api_not_configured", "notification_disabled", "notification_contacts_not_configured", "moderator_api_not_configured"}
    ]
    success = bool(device_result.get("ok") or notify_result.get("ok"))
    result = {
        "ok": success,
        "device": device_result,
        "notification": notify_result,
        "response": f"device={device_result}; notification={notify_result}",
    }
    if success:
        update_sensor_automation_status(
            sensor_dict["sensor_id"],
            event_name=event_name,
            status="success",
            response_text=result.get("response", ""),
            idle_triggered_epoch=now_epoch if event_name == "idle" else None,
            last_trigger_epoch=now_epoch,
            condition_key=condition_key,
        )
    elif actionable_results:
        update_sensor_automation_status(
            sensor_dict["sensor_id"],
            event_name=event_name,
            status="skipped",
            response_text=result.get("response", ""),
            idle_triggered_epoch=now_epoch if event_name == "idle" else None,
            last_trigger_epoch=now_epoch,
            condition_key=condition_key,
        )
    return result


def run_idle_automation_checks() -> None:
    now = app_now()
    now_epoch = int(now.timestamp())

    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT sensor_id
            FROM sensors
            WHERE enabled = 1
              AND automation_enabled = 1
              AND COALESCE(TRIM(automation_device_id), '') != ''
            """
        ).fetchall()

    for row in rows:
        sensor = fetch_sensor(row["sensor_id"])
        if sensor is None:
            continue

        sensor_dict = dict(sensor)

        schedule_allowed, _reason = evaluate_sensor_schedule(sensor, now)
        if not schedule_allowed:
            continue

        try:
            trigger_name = sanitize_trigger(sensor_dict.get("automation_trigger"), "motion")
            evaluate_and_run_sensor_automation(sensor, trigger_name, now)
        except Exception as exc:
            update_sensor_automation_status(
                sensor["sensor_id"],
                event_name=sanitize_trigger(sensor_dict.get("automation_trigger"), "motion"),
                status="error",
                response_text=str(exc),
                idle_triggered_epoch=now_epoch if sanitize_trigger(sensor_dict.get("automation_trigger"), "motion") == "idle" else None,
            )


def automation_worker() -> None:
    while True:
        time.sleep(AUTOMATION_LOOP_SECONDS)
        try:
            run_idle_automation_checks()
        except Exception as exc:
            print(f"Erro no worker de automacao: {exc}", flush=True)


def start_automation_worker() -> None:
    thread = threading.Thread(target=automation_worker, daemon=True)
    thread.start()


def set_all_sensors_enabled(enabled: bool) -> dict:
    agora = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.execute(
            """
            UPDATE sensors
            SET enabled = ?, updated_at = ?
            """,
            (1 if enabled else 0, agora),
        )
        conn.commit()
    return {
        "updated": cursor.rowcount,
        "enabled": enabled,
        "updated_at": agora,
    }


def evaluate_sensor_schedule(sensor: sqlite3.Row | dict, current_time: datetime | None = None) -> tuple[bool, str]:
    agora = current_time or app_now()
    sensor_dict = dict(sensor)
    mode = sensor_dict.get("schedule_mode") or "always"
    if mode != "custom":
        return True, "schedule_allowed"

    weekday_map = (6, 0, 1, 2, 3, 4, 5)
    wd = str(weekday_map[agora.weekday()])
    days = sensor_dict.get("schedule_days") or "0,1,2,3,4,5,6"
    if wd not in days.split(","):
        return False, "schedule_day_blocked"

    alt = sensor_dict.get("schedule_alternate") or "none"
    if alt == "even" and agora.day % 2 != 0:
        return False, "schedule_alternate_blocked"
    if alt == "odd" and agora.day % 2 == 0:
        return False, "schedule_alternate_blocked"

    start_str = sensor_dict.get("schedule_start") or "00:00"
    end_str = sensor_dict.get("schedule_end") or "23:59"
    cur_mins = agora.hour * 60 + agora.minute

    def to_minutes(value: str) -> int:
        parts = value.split(":")
        return int(parts[0]) * 60 + int(parts[1]) if len(parts) == 2 else 0

    start_mins = to_minutes(start_str)
    end_mins = to_minutes(end_str)
    if start_mins <= end_mins:
        if not (start_mins <= cur_mins <= end_mins):
            return False, "schedule_time_blocked"
    else:
        if not (cur_mins >= start_mins or cur_mins <= end_mins):
            return False, "schedule_time_blocked"

    return True, "schedule_allowed"


def build_time_bins(range_key: str) -> tuple[int, int, str]:
    normalized = range_key if range_key in DETAIL_RANGE_SECONDS else "24h"
    range_seconds = DETAIL_RANGE_SECONDS[normalized]
    bin_seconds = max(1, range_seconds // DETAIL_BIN_COUNT)
    return range_seconds, bin_seconds, normalized


def insert_movement(payload: dict) -> dict:
    sensor_id = str(payload.get("sensor_id", "")).strip()
    if not sensor_id:
        raise ValueError("sensor_id_required")

    sensor = fetch_sensor(sensor_id)
    if sensor is None:
        return {
            "stored": False,
            "reason": "sensor_not_registered",
            "sensor_id": sensor_id,
        }

    if not bool(sensor["enabled"]):
        return {
            "stored": False,
            "reason": "sensor_disabled",
            "sensor_id": sensor_id,
            "device_id": sensor["device_id"],
        }

    agora = app_now()
    schedule_allowed, schedule_reason = evaluate_sensor_schedule(sensor, agora)
    if not schedule_allowed:
        return {"stored": False, "reason": schedule_reason, "sensor_id": sensor_id}

    agora_epoch = int(agora.timestamp())
    ocorrido_em = agora.isoformat()
    ultimo = fetch_last_movement(sensor_id)

    if ultimo and agora_epoch - int(ultimo[0]) < COOLDOWN_SECONDS:
        disponivel_em = datetime.fromtimestamp(int(ultimo[0]), tz=APP_TIMEZONE) + timedelta(seconds=COOLDOWN_SECONDS)
        return {
            "stored": False,
            "reason": "cooldown_active",
            "sensor_id": sensor_id,
            "device_id": sensor["device_id"],
            "retry_after_seconds": max(0, int(disponivel_em.timestamp()) - agora_epoch),
            "last_occurred_at": ultimo[1],
        }

    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.execute(
            """
            INSERT INTO movements (sensor_id, device_id, occurred_at, occurred_epoch)
            VALUES (?, ?, ?, ?)
            """,
            (sensor_id, sensor["device_id"], ocorrido_em, agora_epoch),
        )
        movement_id = cursor.lastrowid
        conn.commit()

    if dict(sensor).get("rtsp_url"):
        threading.Thread(
            target=capture_and_save_frame,
            args=(movement_id, sensor["rtsp_url"]),
            daemon=True
        ).start()

    reset_idle_trigger(sensor_id)

    try:
        updated_sensor = fetch_sensor(sensor_id) or sensor
        if sanitize_trigger(dict(updated_sensor).get("automation_trigger"), "motion") == "motion":
            evaluate_and_run_sensor_automation(updated_sensor, "motion", agora, force=True)
    except Exception as exc:
        update_sensor_automation_status(
            sensor_id,
            event_name="motion",
            status="error",
            response_text=str(exc),
        )

    return {
        "stored": True,
        "sensor_id": sensor_id,
        "device_id": sensor["device_id"],
        "occurred_at": ocorrido_em,
    }


def list_sensors(limit: int) -> list[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT
                s.sensor_id,
                s.device_id,
                s.pin,
                s.rtsp_url,
                COALESCE(s.name, s.sensor_id) AS name,
                COALESCE(s.icon, 'motion') AS icon,
                s.enabled,
                s.show_on_dashboard,
                s.device_last_seen_at,
                s.device_last_seen_epoch,
                s.registered_at,
                s.updated_at,
                (
                    SELECT m.occurred_at
                    FROM movements m
                    WHERE m.sensor_id = s.sensor_id
                    ORDER BY m.occurred_epoch DESC
                    LIMIT 1
                ) AS last_motion_at,
                (
                    SELECT m.occurred_epoch
                    FROM movements m
                    WHERE m.sensor_id = s.sensor_id
                    ORDER BY m.occurred_epoch DESC
                    LIMIT 1
                ) AS last_motion_epoch
            FROM sensors s
            ORDER BY s.device_id, s.pin
            LIMIT ?
            """,
            (limit,),
        ).fetchall()

    output = []
    now_epoch = int(app_now().timestamp())
    for row in rows:
        item = dict(row)
        item["enabled"] = bool(item["enabled"])
        item["icon"] = sanitize_sensor_icon_value(item.get("icon"), item["sensor_id"])
        item["icon_kind"] = infer_icon_kind(item["icon"], item["sensor_id"])
        item["icon_url"] = sensor_icon_url(item["icon"], item["sensor_id"])
        item["device_online"] = is_device_online(item, now_epoch)
        output.append(item)
    return output


def list_contacts() -> list[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT id, name, phone, created_at, updated_at
            FROM contacts
            ORDER BY name COLLATE NOCASE ASC, id ASC
            """
        ).fetchall()
    return [dict(row) for row in rows]


def create_contact(name: str, phone: str) -> dict:
    normalized_name = str(name or "").strip()
    normalized_phone = str(phone or "").strip()
    if not normalized_name:
        raise ValueError("contact_name_required")
    if not normalized_phone:
        raise ValueError("contact_phone_required")

    now_iso = app_now().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        try:
            cursor = conn.execute(
                """
                INSERT INTO contacts (name, phone, created_at, updated_at)
                VALUES (?, ?, ?, ?)
                """,
                (normalized_name, normalized_phone, now_iso, now_iso),
            )
            conn.commit()
        except sqlite3.IntegrityError:
            raise ValueError("contact_phone_already_exists")

        row = conn.execute(
            "SELECT id, name, phone, created_at, updated_at FROM contacts WHERE id = ?",
            (cursor.lastrowid,),
        ).fetchone()
    return dict(row)


def delete_contact(contact_id: int) -> dict | None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        row = conn.execute(
            "SELECT id, name, phone FROM contacts WHERE id = ?",
            (contact_id,),
        ).fetchone()
        if row is None:
            return None
        conn.execute("DELETE FROM contacts WHERE id = ?", (contact_id,))
        conn.commit()
    return dict(row)


def list_movements(limit: int) -> list[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT m.id, m.sensor_id, m.device_id, s.pin, COALESCE(s.name, s.sensor_id) AS sensor_name, m.occurred_at
            FROM movements m
            JOIN sensors s ON s.sensor_id = m.sensor_id
            ORDER BY m.id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [dict(row) for row in rows]


def list_sensor_movements(sensor_id: str, limit: int, offset: int = 0) -> list[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT id, occurred_at, image_path
            FROM movements
            WHERE sensor_id = ?
            ORDER BY occurred_epoch DESC
            LIMIT ? OFFSET ?
            """,
            (sensor_id, limit, offset),
        ).fetchall()
    return [dict(row) for row in rows]


def get_sensor_detail(sensor_id: str, range_key: str) -> dict | None:
    sensor = fetch_sensor(sensor_id)
    if sensor is None:
        return None

    range_seconds, bin_seconds, normalized_range = build_time_bins(range_key)
    now = app_now()
    now_epoch = int(now.timestamp())
    cutoff_epoch = now_epoch - range_seconds

    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT occurred_epoch, occurred_at
            FROM movements
            WHERE sensor_id = ? AND occurred_epoch >= ?
            ORDER BY occurred_epoch ASC
            """,
            (sensor_id, cutoff_epoch),
        ).fetchall()

    last_motion = fetch_last_movement(sensor_id)
    last_motion_epoch = int(last_motion[0]) if last_motion else None
    last_motion_at = last_motion[1] if last_motion else None
    last_motion_image = last_motion[2] if last_motion else None
    last_motion_ago_seconds = None if last_motion_epoch is None else max(0, now_epoch - last_motion_epoch)
    
    last_motion_at_formatted = None
    if last_motion_at:
        try:
            from datetime import datetime as dt_temp
            dt = dt_temp.fromisoformat(last_motion_at)
            # Fuso já é -3 (ou o logado ISO) – converter puramente para String formatada
            dt_local = dt.astimezone(APP_TIMEZONE)
            last_motion_at_formatted = dt_local.strftime("%d/%m/%Y %H:%M:%S")
        except Exception:
            last_motion_at_formatted = last_motion_at

    series = []
    counts = [0] * DETAIL_BIN_COUNT
    for row in rows:
      idx = int((int(row["occurred_epoch"]) - cutoff_epoch) // bin_seconds)
      if idx < 0:
          continue
      if idx >= DETAIL_BIN_COUNT:
          idx = DETAIL_BIN_COUNT - 1
      counts[idx] += 1

    for index in range(DETAIL_BIN_COUNT):
        start_epoch = cutoff_epoch + index * bin_seconds
        start_at = datetime.fromtimestamp(start_epoch, tz=APP_TIMEZONE)
        label = start_at.strftime("%H:%M")
        if normalized_range == "24h":
            label = start_at.strftime("%Hh")
        series.append(
            {
                "label": label,
                "count": counts[index],
                "is_current": index == DETAIL_BIN_COUNT - 1,
            }
        )

    sensor_dict = dict(sensor)
    sensor_dict["enabled"] = bool(sensor_dict["enabled"])
    sensor_dict["device_online"] = is_device_online(sensor_dict, now_epoch)
    recent_motion = last_motion_epoch is not None and now_epoch - last_motion_epoch <= RECENT_MOTION_SECONDS

    if not sensor_dict["enabled"]:
        tone = "inactive"
        status_label = "INATIVO"
    elif not sensor_dict["device_online"]:
        tone = "warning"
        status_label = "OFFLINE"
    elif recent_motion:
        tone = "alert"
        status_label = "MOVIMENTO"
    else:
        tone = "active"
        status_label = "ATIVO"

    return {
        "ok": True,
        "sensor": {
            "sensor_id": sensor_dict["sensor_id"],
            "device_id": sensor_dict["device_id"],
            "pin": sensor_dict["pin"],
            "name": sensor_dict["name"] or default_sensor_name(sensor_dict["sensor_id"]),
            "icon": sanitize_sensor_icon_value(sensor_dict["icon"], sensor_dict["sensor_id"]),
            "icon_kind": infer_icon_kind(sensor_dict["icon"] or "", sensor_dict["sensor_id"]),
            "icon_url": sensor_icon_url(sensor_dict["icon"] or "", sensor_dict["sensor_id"]),
            "enabled": sensor_dict["enabled"],
            "rtsp_url": sensor_dict.get("rtsp_url", ""),
            "schedule_mode": sensor_dict.get("schedule_mode", "always"),
            "schedule_start": sensor_dict.get("schedule_start", "00:00"),
            "schedule_end": sensor_dict.get("schedule_end", "23:59"),
            "schedule_days": sensor_dict.get("schedule_days", "0,1,2,3,4,5,6"),
            "schedule_alternate": sensor_dict.get("schedule_alternate", "none"),
            "show_on_dashboard": bool(sensor_dict.get("show_on_dashboard", 1)),
            "automation_motion_enabled": bool(sensor_dict.get("automation_motion_enabled", 0)),
            "automation_motion_device_id": sensor_dict.get("automation_motion_device_id", "") or "",
            "automation_motion_action": sensor_dict.get("automation_motion_action", "power") or "power",
            "automation_motion_state": sensor_dict.get("automation_motion_state", "ON") or "ON",
            "automation_idle_enabled": bool(sensor_dict.get("automation_idle_enabled", 0)),
            "automation_idle_minutes": int(sensor_dict.get("automation_idle_minutes", 10) or 10),
            "automation_idle_device_id": sensor_dict.get("automation_idle_device_id", "") or "",
            "automation_idle_action": sensor_dict.get("automation_idle_action", "power") or "power",
            "automation_idle_state": sensor_dict.get("automation_idle_state", "OFF") or "OFF",
            "automation_last_event": sensor_dict.get("automation_last_event", "") or "",
            "automation_last_status": sensor_dict.get("automation_last_status", "") or "",
            "automation_last_response": sensor_dict.get("automation_last_response", "") or "",
            "automation_last_run_at": sensor_dict.get("automation_last_run_at", "") or "",
            "automation_enabled": bool(sensor_dict.get("automation_enabled", 0)),
            "automation_trigger": sensor_dict.get("automation_trigger", "motion") or "motion",
            "automation_device_id": sensor_dict.get("automation_device_id", "") or "",
            "automation_action": sensor_dict.get("automation_action", "power") or "power",
            "automation_state": sensor_dict.get("automation_state", "ON") or "ON",
            "automation_repeat_mode": sensor_dict.get("automation_repeat_mode", "once") or "once",
            "automation_repeat_seconds": int(sensor_dict.get("automation_repeat_seconds", 30) or 30),
            "automation_notify_enabled": bool(sensor_dict.get("automation_notify_enabled", 0)),
            "automation_notify_contact_ids": [
                int(item)
                for item in str(sensor_dict.get("automation_notify_contact_ids", "") or "").split(",")
                if item.strip().isdigit()
            ],
            "automation_notify_message": sensor_dict.get("automation_notify_message", "") or "",
            "device_online": sensor_dict["device_online"],
            "device_last_seen_at": sensor_dict.get("device_last_seen_at", "") or "",
            "device_last_seen_epoch": sensor_dict.get("device_last_seen_epoch"),
            "status_label": status_label,
            "tone": tone,
            "registered_at": sensor_dict["registered_at"],
            "updated_at": sensor_dict["updated_at"],
            "last_motion_at": last_motion_at,
            "last_motion_ago_seconds": last_motion_ago_seconds,
            "last_motion_at_formatted": last_motion_at_formatted,
            "last_motion_image": last_motion_image,
        },
        "analysis": {
            "range": normalized_range,
            "range_seconds": range_seconds,
            "bin_seconds": bin_seconds,
            "total_movements": sum(counts),
            "series": series,
        },
    }


def build_dashboard_payload() -> dict:
    sensores = list_sensors(500)
    agora_epoch = int(app_now().timestamp())
    cutoff_epoch = agora_epoch - DASHBOARD_BIN_SECONDS * DASHBOARD_BIN_COUNT

    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT sensor_id, occurred_epoch
            FROM movements
            WHERE occurred_epoch >= ?
            ORDER BY occurred_epoch ASC
            """,
            (cutoff_epoch,),
        ).fetchall()

    movimentos_por_sensor: dict[str, list[int]] = defaultdict(list)
    for row in rows:
        movimentos_por_sensor[row["sensor_id"]].append(int(row["occurred_epoch"]))

    enabled_count = 0
    moving_count = 0
    dashboard_sensors = []

    for sensor in sensores:
        bins = [0] * DASHBOARD_BIN_COUNT
        for occurred_epoch in movimentos_por_sensor.get(sensor["sensor_id"], []):
            idx = int((occurred_epoch - cutoff_epoch) // DASHBOARD_BIN_SECONDS)
            if idx < 0:
                continue
            if idx >= DASHBOARD_BIN_COUNT:
                idx = DASHBOARD_BIN_COUNT - 1
            bins[idx] += 1

        recent_motion = sensor["last_motion_epoch"] is not None and agora_epoch - int(sensor["last_motion_epoch"]) <= RECENT_MOTION_SECONDS
        device_online = bool(sensor.get("device_online"))
        if sensor["enabled"]:
            enabled_count += 1
        if sensor["enabled"] and device_online and recent_motion:
            moving_count += 1

        if not sensor["enabled"]:
            tone = "inactive"
            status_label = "INATIVO"
        elif not device_online:
            tone = "warning"
            status_label = "OFFLINE"
        elif recent_motion:
            tone = "alert"
            status_label = "MOVIMENTO"
        else:
            tone = "active"
            status_label = "ATIVO"

        dashboard_sensors.append(
            {
                "sensor_id": sensor["sensor_id"],
                "device_id": sensor["device_id"],
                "pin": sensor["pin"],
                "name": sensor["name"],
                "icon": sensor["icon"],
                "icon_kind": sensor.get("icon_kind", infer_icon_kind(sensor["icon"], sensor["sensor_id"])),
                "icon_url": sensor.get("icon_url", sensor_icon_url(sensor["icon"], sensor["sensor_id"])),
                "enabled": sensor["enabled"],
                "device_online": device_online,
                "show_on_dashboard": bool(sensor["show_on_dashboard"]),
                "status_label": status_label,
                "tone": tone,
                "recent_bins": bins,
                "last_motion_at": sensor["last_motion_at"],
            }
        )

    total_sensors = len(dashboard_sensors)
    if total_sensors == 0 or enabled_count == 0:
        system_label = "SYSTEM DISARMED"
        system_tone = "inactive"
    elif enabled_count == total_sensors:
        system_label = "SYSTEM ARMED"
        system_tone = "active"
    else:
        system_label = "SYSTEM PARTIAL"
        system_tone = "warning"

    return {
        "ok": True,
        "summary": {
            "total_sensors": total_sensors,
            "enabled_sensors": enabled_count,
            "moving_sensors": moving_count,
            "cooldown_seconds": COOLDOWN_SECONDS,
            "bin_seconds": DASHBOARD_BIN_SECONDS,
        },
        "system": {
            "label": system_label,
            "tone": system_tone,
        },
        "sensors": dashboard_sensors,
    }


def parse_limit(query: str, default: int = 50) -> int:
    params = parse_qs(query)
    limit_str = params.get("limit", [str(default)])[0]
    try:
        return max(1, min(500, int(limit_str)))
    except ValueError:
        return default


def load_dashboard_html() -> str:
    if DASHBOARD_HTML_PATH.exists():
        return DASHBOARD_HTML_PATH.read_text(encoding="utf-8")
    return "<html><body><h1>Dashboard indisponivel</h1></body></html>"


def load_sensor_detail_html() -> str:
    if SENSOR_DETAIL_HTML_PATH.exists():
        return SENSOR_DETAIL_HTML_PATH.read_text(encoding="utf-8")
    return "<html><body><h1>Detalhe indisponivel</h1></body></html>"


def load_sensor_report_html() -> str:
    if SENSOR_REPORT_HTML_PATH.exists():
        return SENSOR_REPORT_HTML_PATH.read_text(encoding="utf-8")
    return "<html><body><h1>Relatorio indisponivel</h1></body></html>"


class SensorHubHandler(BaseHTTPRequestHandler):
    server_version = "SensorHubAPI/3.0"

    def _send_json(self, status: HTTPStatus, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, status: HTTPStatus, html: str) -> None:
        body = html.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise ValueError("invalid_content_length")

        if length <= 0:
            raise ValueError("empty_body")

        try:
            raw = self.rfile.read(length)
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ValueError("invalid_json")

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)

        if parsed.path == "/":
            self._send_html(HTTPStatus.OK, load_dashboard_html())
            return
            
        if parsed.path.startswith("/snapshots/"):
            filename = parsed.path.replace("/snapshots/", "", 1)
            filepath = SNAPSHOTS_DIR / filename
            if filepath.exists() and filepath.is_file():
                self.send_response(HTTPStatus.OK)
                mt, _ = mimetypes.guess_type(str(filepath))
                self.send_header("Content-Type", mt or "image/jpeg")
                self.end_headers()
                with open(filepath, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})
            return

        if parsed.path.startswith("/sensor-icons/"):
            filename = parsed.path.replace("/sensor-icons/", "", 1)
            filepath = SENSOR_ICONS_DIR / Path(filename).name
            if filepath.exists() and filepath.is_file():
                self.send_response(HTTPStatus.OK)
                mt, _ = mimetypes.guess_type(str(filepath))
                self.send_header("Content-Type", mt or "application/octet-stream")
                self.end_headers()
                with open(filepath, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})
            return

        if parsed.path == "/sensor":
            sensor_id = params.get("sensor_id", [""])[0].strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
            else:
                self._send_html(HTTPStatus.OK, load_sensor_detail_html())
            return

        if parsed.path == "/report":
            sensor_id = params.get("sensor_id", [""])[0].strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
            else:
                self._send_html(HTTPStatus.OK, load_sensor_report_html())
            return

        if parsed.path == "/health":
            self._send_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "service": "sensor-hub-api",
                    "db_path": str(DB_PATH),
                    "cooldown_seconds": COOLDOWN_SECONDS,
                    "device_heartbeat_timeout_seconds": DEVICE_HEARTBEAT_TIMEOUT_SECONDS,
                    "timezone": "UTC-3",
                },
            )
            return

        if parsed.path == "/api/system/cleanup":
            self._send_json(HTTPStatus.OK, cleanup_snapshots())
            return

        if parsed.path == "/api/dashboard":
            self._send_json(HTTPStatus.OK, build_dashboard_payload())
            return

        if parsed.path == "/api/contacts":
            self._send_json(HTTPStatus.OK, {"ok": True, "contacts": list_contacts()})
            return

        if parsed.path == "/api/external/devices":
            force = coerce_bool(params.get("refresh", ["false"])[0])
            try:
                payload = fetch_external_devices(force=force)
                payload["configured"] = external_api_ready()
                self._send_json(HTTPStatus.OK, payload)
            except Exception as exc:
                self._send_json(
                    HTTPStatus.BAD_GATEWAY,
                    {
                        "ok": False,
                        "configured": external_api_ready(),
                        "error": str(exc),
                        "base_url": EXTERNAL_API_BASE_URL,
                    },
                )
            return

        if parsed.path == "/api/sensors/movements":
            sensor_id = params.get("sensor_id", [""])[0].strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            
            limit = parse_limit(parsed.query, 10)
            offset_str = params.get("offset", ["0"])[0]
            offset = int(offset_str) if offset_str.isdigit() else 0
            
            moves = list_sensor_movements(sensor_id, limit, offset)
            self._send_json(HTTPStatus.OK, {"ok": True, "movements": moves})
            return

        if parsed.path == "/api/sensors/test_rtsp":
            url = params.get("rtsp_url", [""])[0].strip()
            if not url:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "rtsp_url_required"})
                return

            try:
                cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
                if not cap.isOpened():
                    self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "camera_unreachable"})
                    return
                # ler 1 frame com timeout limitrofe nativo
                ret, frame = cap.read()
                cap.release()
                if ret:
                    success, buffer = cv2.imencode('.jpg', frame, [int(cv2.IMWRITE_JPEG_QUALITY), 60])
                    if success:
                        self.send_response(HTTPStatus.OK)
                        self.send_header("Content-Type", "image/jpeg")
                        self.end_headers()
                        self.wfile.write(buffer.tobytes())
                        return
                    else:
                        self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": "encode_failed"})
                        return
                else:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "no_frame"})
                    return
            except Exception as e:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(e)})
            return

        if parsed.path == "/api/sensors/detail":
            sensor_id = params.get("sensor_id", [""])[0].strip()
            range_key = params.get("range", ["24h"])[0].strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return

            detail = get_sensor_detail(sensor_id, range_key)
            if detail is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                self._send_json(HTTPStatus.OK, detail)
            return

        if parsed.path == "/sensors":
            self._send_json(
                HTTPStatus.OK,
                {"ok": True, "sensors": list_sensors(parse_limit(parsed.query, 200))},
            )
            return

        if parsed.path == "/movements":
            self._send_json(
                HTTPStatus.OK,
                {"ok": True, "movements": list_movements(parse_limit(parsed.query, 50))},
            )
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)

        try:
            payload = self._read_json()
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
            return

        if parsed.path == "/sensors/register":
            try:
                result = upsert_sensors(payload)
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return
            self._send_json(HTTPStatus.CREATED, {"ok": True, **result})
            return

        if parsed.path == "/devices/heartbeat":
            try:
                result = mark_device_heartbeat(str(payload.get("device_id", "")))
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return
            self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        if parsed.path == "/movements":
            try:
                result = insert_movement(payload)
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return

            if result["stored"]:
                self._send_json(HTTPStatus.CREATED, {"ok": True, **result})
            elif result["reason"] in {"cooldown_active", "sensor_disabled"}:
                self._send_json(HTTPStatus.ACCEPTED, {"ok": True, **result})
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, **result})
            return

        if parsed.path == "/api/sensors/toggle":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            result = set_sensor_enabled(sensor_id, coerce_bool(payload.get("enabled", False)))
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        if parsed.path == "/api/sensors/config_dashboard":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            show = coerce_bool(payload.get("show_on_dashboard", True))
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            result = set_sensor_dashboard_visibility(sensor_id, show)
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return


        if parsed.path == "/api/sensors/rename":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            name = str(payload.get("name", ""))
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            try:
                result = update_sensor_name(sensor_id, name)
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                    self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        if parsed.path == "/api/sensors/delete":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            result = delete_sensor(sensor_id)
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        if parsed.path == "/api/sensors/icon":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            icon_mode = str(payload.get("icon_mode", "builtin")).strip().lower()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return

            try:
                if icon_mode == "fa":
                    fa_name = str(payload.get("fa_icon", "")).strip()
                    if not fa_name:
                        raise ValueError("fa_icon_required")
                    icon_value = f"fa:{fa_name}"
                elif icon_mode == "upload":
                    icon_value = store_uploaded_sensor_icon(
                        sensor_id,
                        str(payload.get("file_name", "")).strip(),
                        str(payload.get("file_base64", "")).strip(),
                    )
                else:
                    builtin_icon = str(payload.get("builtin_icon", "")).strip()
                    icon_value = builtin_icon if builtin_icon in ICON_BUILTIN_NAMES else default_sensor_icon(sensor_id)
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return

            result = update_sensor_icon(sensor_id, icon_value)
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
            else:
                self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        if parsed.path == "/api/contacts/create":
            try:
                result = create_contact(
                    str(payload.get("name", "")),
                    str(payload.get("phone", "")),
                )
            except ValueError as exc:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": str(exc)})
                return
            self._send_json(HTTPStatus.CREATED, {"ok": True, "contact": result})
            return

        if parsed.path == "/api/contacts/delete":
            contact_id_raw = payload.get("contact_id")
            try:
                contact_id = int(contact_id_raw)
            except (TypeError, ValueError):
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "contact_id_required"})
                return
            result = delete_contact(contact_id)
            if result is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "contact_not_found"})
            else:
                self._send_json(HTTPStatus.OK, {"ok": True, "contact": result})
            return

        if parsed.path == "/api/sensors/rtsp":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            rtsp_url = str(payload.get("rtsp_url", "")).strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            
            agora = app_now().isoformat()
            with sqlite3.connect(DB_PATH) as conn:
                conn.execute(
                    "UPDATE sensors SET rtsp_url = ?, updated_at = ? WHERE sensor_id = ?",
                    (rtsp_url, agora, sensor_id),
                )
                conn.commit()
            self._send_json(HTTPStatus.OK, {"ok": True})
            return

        if parsed.path == "/api/sensors/schedule":
            try:
                result = update_sensor_schedule(payload)
                if result is None:
                    self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
                else:
                    self._send_json(HTTPStatus.OK, {"ok": True, **result})
            except Exception as e:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(e)})
            return

        if parsed.path == "/api/sensors/automation":
            try:
                result = update_sensor_automation(payload)
                if result is None:
                    self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
                else:
                    self._send_json(HTTPStatus.OK, {"ok": True, **result})
            except Exception as e:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"ok": False, "error": str(e)})
            return

        if parsed.path == "/api/sensors/automation/test":
            sensor_id = str(payload.get("sensor_id", "")).strip()
            event_name = sanitize_trigger(payload.get("event", "motion"), "motion")
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
                return
            sensor = fetch_sensor(sensor_id)
            if sensor is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "sensor_not_found"})
                return

            try:
                result = evaluate_and_run_sensor_automation(sensor, event_name, app_now(), force=True)
                if result.get("ok"):
                    self._send_json(HTTPStatus.OK, {"ok": True, **result})
                else:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, **result})
            except Exception as e:
                update_sensor_automation_status(
                    sensor_id,
                    event_name=event_name,
                    status="error",
                    response_text=str(e),
                )
                self._send_json(HTTPStatus.BAD_GATEWAY, {"ok": False, "error": str(e)})
            return

        if parsed.path == "/api/system/state":
            enabled = coerce_bool(payload.get("enabled", False))
            result = set_all_sensors_enabled(enabled)
            self._send_json(HTTPStatus.OK, {"ok": True, **result})
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})

    def log_message(self, format: str, *args) -> None:
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] {self.address_string()} - {format % args}", flush=True)


def main() -> None:
    init_db()
    start_snapshot_cleanup()
    start_automation_worker()
    server = ThreadingHTTPServer((HOST, PORT), SensorHubHandler)
    print(f"Servidor escutando em http://{HOST}:{PORT}", flush=True)
    print(f"Banco SQLite: {DB_PATH}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
