import argparse
import json
import time

import serial  # pyserial


def write_ndjson(path: str, obj: dict) -> None:
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="COM port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="debug-8351e9.log", help="NDJSON output path")
    ap.add_argument("--session", default="8351e9", help="Expected sessionId (optional filter)")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)

    run_id = f"host-{int(time.time())}"
    write_ndjson(
        args.out,
        {
            "sessionId": args.session,
            "runId": run_id,
            "hypothesisId": "HOST",
            "location": "serial_ndjson_logger.py:main",
            "message": "serial logger started",
            "data": {"port": args.port, "baud": args.baud},
            "timestamp": int(time.time() * 1000),
        },
    )

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode(errors="replace").strip()
            if not line:
                continue

            # If the device prints JSON, preserve it; otherwise wrap it.
            if line.startswith("{") and line.endswith("}"):
                try:
                    obj = json.loads(line)
                    if args.session and obj.get("sessionId") not in (None, args.session):
                        continue
                    write_ndjson(args.out, obj)
                    continue
                except Exception:
                    pass

            write_ndjson(
                args.out,
                {
                    "sessionId": args.session,
                    "runId": run_id,
                    "hypothesisId": "HOST",
                    "location": "serial_ndjson_logger.py:main",
                    "message": "serial line",
                    "data": {"line": line},
                    "timestamp": int(time.time() * 1000),
                },
            )
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())

