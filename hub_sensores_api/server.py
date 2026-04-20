from __future__ import annotations

import json
import sqlite3
import threading
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
DB_PATH = BASE_DIR / "sensor_events.db"
DASHBOARD_HTML_PATH = BASE_DIR / "templates" / "dashboard.html"
SENSOR_DETAIL_HTML_PATH = BASE_DIR / "templates" / "sensor_detail.html"
SENSOR_REPORT_HTML_PATH = BASE_DIR / "templates" / "sensor_report.html"
SNAPSHOTS_DIR = BASE_DIR / "snapshots"
SNAPSHOTS_DIR.mkdir(exist_ok=True)


def resolve_ffmpeg_bin() -> str:
    ffmpeg_path = shutil.which("ffmpeg")
    if ffmpeg_path:
        return ffmpeg_path

    winget_packages = Path.home() / "AppData" / "Local" / "Microsoft" / "WinGet" / "Packages"
    for candidate in winget_packages.glob("Gyan.FFmpeg_*/*/bin/ffmpeg.exe"):
        return str(candidate)

    return "ffmpeg"


FFMPEG_BIN = resolve_ffmpeg_bin()


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


def fetch_sensor(sensor_id: str) -> sqlite3.Row | None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        row = conn.execute(
            """
            SELECT sensor_id, device_id, pin, name, icon, enabled, registered_at, updated_at, rtsp_url,
                   schedule_mode, schedule_start, schedule_end, schedule_days, schedule_alternate
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

            if not sensor_id:
                raise ValueError("sensor_id_required")
            if pin is None:
                raise ValueError("pin_required")

            conn.execute(
                """
                INSERT INTO sensors (sensor_id, device_id, pin, name, icon, enabled, registered_at, updated_at)
                VALUES (?, ?, ?, ?, ?, 1, ?, ?)
                ON CONFLICT(sensor_id) DO UPDATE SET
                    device_id = excluded.device_id,
                    pin = excluded.pin,
                    name = excluded.name,
                    icon = excluded.icon,
                    updated_at = excluded.updated_at
                """,
                (sensor_id, device_id, int(pin), name, icon, agora, agora),
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
    
    # Schedule Logic
    s_dict = dict(sensor)
    mode = s_dict.get("schedule_mode") or "always"
    if mode == "custom":
        wd = str(agora.weekday())
        days = s_dict.get("schedule_days") or "0,1,2,3,4,5,6"
        if wd not in days.split(","):
            return {"stored": False, "reason": "schedule_day_blocked", "sensor_id": sensor_id}
            
        alt = s_dict.get("schedule_alternate") or "none"
        if alt == "even" and agora.day % 2 != 0:
            return {"stored": False, "reason": "schedule_alternate_blocked", "sensor_id": sensor_id}
        if alt == "odd" and agora.day % 2 == 0:
            return {"stored": False, "reason": "schedule_alternate_blocked", "sensor_id": sensor_id}
            
        start_str = s_dict.get("schedule_start") or "00:00"
        end_str = s_dict.get("schedule_end") or "23:59"
        cur_mins = agora.hour * 60 + agora.minute
        
        def to_m(t):
            p = t.split(":")
            return int(p[0])*60 + int(p[1]) if len(p) == 2 else 0
            
        s_mins, e_mins = to_m(start_str), to_m(end_str)
        if s_mins <= e_mins:
            if not (s_mins <= cur_mins <= e_mins):
                return {"stored": False, "reason": "schedule_time_blocked", "sensor_id": sensor_id}
        else:
            if not (cur_mins >= s_mins or cur_mins <= e_mins):
                return {"stored": False, "reason": "schedule_time_blocked", "sensor_id": sensor_id}

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
    for row in rows:
        item = dict(row)
        item["enabled"] = bool(item["enabled"])
        output.append(item)
    return output


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
    recent_motion = last_motion_epoch is not None and now_epoch - last_motion_epoch <= RECENT_MOTION_SECONDS

    if not sensor_dict["enabled"]:
        tone = "inactive"
        status_label = "INATIVO"
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
            "icon": sensor_dict["icon"] or default_sensor_icon(sensor_dict["sensor_id"]),
            "enabled": sensor_dict["enabled"],
            "rtsp_url": sensor_dict.get("rtsp_url", ""),
            "schedule_mode": sensor_dict.get("schedule_mode", "always"),
            "schedule_start": sensor_dict.get("schedule_start", "00:00"),
            "schedule_end": sensor_dict.get("schedule_end", "23:59"),
            "schedule_days": sensor_dict.get("schedule_days", "0,1,2,3,4,5,6"),
            "schedule_alternate": sensor_dict.get("schedule_alternate", "none"),
            "status_label": sensor_dict["status_label"],
            "tone": sensor_dict["tone"],
            "registered_at": sensor_dict["registered_at"],
            "updated_at": sensor_dict["updated_at"],
            "last_motion_at": last_motion_at,
            "last_motion_ago_seconds": sensor_dict["last_motion_ago_seconds"],
            "last_motion_at_formatted": sensor_dict["last_motion_at_formatted"],
            "last_motion_image": sensor_dict["last_motion_image"],
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
        if sensor["enabled"]:
            enabled_count += 1
        if sensor["enabled"] and recent_motion:
            moving_count += 1

        if not sensor["enabled"]:
            tone = "inactive"
            status_label = "INATIVO"
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
                "enabled": sensor["enabled"],
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
                    "timezone": "UTC-3",
                },
            )
            return

        if parsed.path == "/api/dashboard":
            self._send_json(HTTPStatus.OK, build_dashboard_payload())
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
    server = ThreadingHTTPServer((HOST, PORT), SensorHubHandler)
    print(f"Servidor escutando em http://{HOST}:{PORT}", flush=True)
    print(f"Banco SQLite: {DB_PATH}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
