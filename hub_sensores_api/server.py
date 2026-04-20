from __future__ import annotations

import json
import sqlite3
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
    "12h": 12 * 60 * 60,
    "24h": 24 * 60 * 60,
}
DETAIL_BIN_COUNT = 12
APP_TIMEZONE = timezone(timedelta(hours=-3))
BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "sensor_events.db"
DASHBOARD_HTML_PATH = BASE_DIR / "dashboard.html"
SENSOR_DETAIL_HTML_PATH = BASE_DIR / "sensor_detail.html"


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
            SELECT sensor_id, device_id, pin, name, icon, enabled, registered_at, updated_at
            FROM sensors
            WHERE sensor_id = ?
            """,
            (sensor_id,),
        ).fetchone()
    return row


def fetch_last_movement(sensor_id: str) -> tuple[int, str] | None:
    with sqlite3.connect(DB_PATH) as conn:
        row = conn.execute(
            """
            SELECT occurred_epoch, occurred_at
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
        conn.execute(
            """
            INSERT INTO movements (sensor_id, device_id, occurred_at, occurred_epoch)
            VALUES (?, ?, ?, ?)
            """,
            (sensor_id, sensor["device_id"], ocorrido_em, agora_epoch),
        )
        conn.commit()

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
    last_motion_ago_seconds = None if last_motion_epoch is None else max(0, now_epoch - last_motion_epoch)

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
            "status_label": status_label,
            "tone": tone,
            "registered_at": sensor_dict["registered_at"],
            "updated_at": sensor_dict["updated_at"],
            "last_motion_at": last_motion_at,
            "last_motion_ago_seconds": last_motion_ago_seconds,
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

        if parsed.path == "/sensor":
            sensor_id = params.get("sensor_id", [""])[0].strip()
            if not sensor_id:
                self._send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "sensor_id_required"})
            else:
                self._send_html(HTTPStatus.OK, load_sensor_detail_html())
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
