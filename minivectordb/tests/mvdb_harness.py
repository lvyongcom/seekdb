#!/usr/bin/env python3
"""Local driver for the MiniVectorDB public cases.

Why this exists
---------------
The shipped ``protocol_check/run.py`` cannot run on Windows: its transport uses
``selectors.DefaultSelector()``, which maps to ``SelectSelector`` there, and
Windows ``select()`` only accepts sockets. Registering a pipe raises
``OSError [WinError 10093]``. This harness reproduces the same request/response
sequencing with a blocking reader thread instead, so it works on Windows and
Linux alike.

It still reuses the official ``_assertions.assert_response`` and
``_transport.strict_json`` so result-comparison semantics stay byte-for-byte
identical to the real judge. Only the plumbing differs.

Usage
-----
    python tests/mvdb_harness.py --exe build/minivectordb.exe
    python tests/mvdb_harness.py --exe build/minivectordb.exe --handshake-only
    python tests/mvdb_harness.py --exe build/minivectordb.exe --case sql_crud
    python tests/mvdb_harness.py --command '["python3","x.py"]'

Exit code 0 means every selected case passed.
"""

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]           # .../seekdb-round2
ARTIFACT_ROOT = Path(__file__).resolve().parents[1]  # .../minivectordb
DEFAULT_PACKAGE = ROOT / "mysqltest"
DEFAULT_PROTOCOL = ROOT / "protocol_check"
# Data directories must live inside the workspace: restricted environments
# routinely deny writes to the system temp directory, which looks exactly like
# a storage bug when the database is the one being denied.
DEFAULT_DATA_ROOT = ARTIFACT_ROOT / ".tmpdata"

_EOF = object()


class ProtocolError(RuntimeError):
    pass


def load_official_modules(protocol_dir):
    """Import the judge's own comparison helpers from protocol_check/."""
    sys.path.insert(0, str(protocol_dir))
    from _assertions import assert_response  # noqa: E402
    from _transport import strict_json       # noqa: E402

    return strict_json, assert_response


class Session:
    """One database process, driven request by request."""

    def __init__(self, argv, data_dir, config, strict_json, log_path):
        self.base_argv = list(argv)
        self.data_dir = Path(data_dir)
        self.config = config
        self.strict_json = strict_json
        self.log_path = Path(log_path)
        self.proc = None
        self.queue = None
        self.log = None
        self.sequence = 0

    # -- process lifecycle -------------------------------------------------
    def start(self):
        argv = [part.replace("{data_dir}", str(self.data_dir)) for part in self.base_argv]
        env = dict(os.environ, VECTORDB_DATA_DIR=str(self.data_dir))
        self.log = open(self.log_path, "ab", buffering=0)
        try:
            self.proc = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self.log,
                bufsize=0,
                env=env,
            )
        except OSError as exc:
            raise ProtocolError(f"cannot start {argv!r}: {exc}") from exc

        self.queue = queue.Queue()
        # Bind the process and its queue into the thread. Reading self.proc /
        # self.queue inside the thread is a live lookup, and `--restart`
        # replaces both: the previous thread would then report the *old*
        # process's EOF into the *new* queue, which looked exactly like the
        # database dying at shutdown.
        proc = self.proc
        target = self.queue
        threading.Thread(target=self._pump_stdout, args=(proc, target),
                         daemon=True).start()

        message = self.read(self.config["startup_timeout"])
        if message != {"type": "ready"}:
            raise ProtocolError(f"expected ready, got {message!r}")

    def _pump_stdout(self, proc, target):
        try:
            while True:
                line = proc.stdout.readline()
                if not line:
                    target.put(_EOF)
                    return
                target.put(line)
        except Exception as exc:  # pragma: no cover - defensive
            target.put(exc)

    def read(self, timeout):
        try:
            item = self.queue.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError("JSON response timeout") from None
        if item is _EOF:
            # The exit status is the single most useful fact here: 0 means the
            # process returned on its own, a negative value is the signal that
            # killed it. poll() can still be None because the pipe closes a
            # moment before the child is reaped, so wait briefly for it.
            code = None
            alive = False
            if self.proc is not None:
                try:
                    code = self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    alive = True
            detail = ""
            if alive and self.proc is not None:
                # The process is still running yet its stdout reads as EOF: the
                # only way that happens is the child closing fd 1, so look.
                try:
                    fds = sorted(os.listdir(f"/proc/{self.proc.pid}/fd"))
                except OSError as exc:
                    fds = [f"<unavailable: {exc}>"]
                detail = f", still running, open fds {fds}"
                self.proc.kill()
            raise ProtocolError(
                f"database exited before response (exit status {code!r}{detail})")
        if isinstance(item, Exception):
            raise ProtocolError(f"read failure: {item}")
        raw = item.rstrip(b"\n").rstrip(b"\r")
        try:
            value = self.strict_json(raw.decode("utf-8"))
        except Exception as exc:
            raise ProtocolError(
                f"invalid JSON response: {exc}; stdout line (first 240 bytes)={raw[:240]!r}"
            ) from exc
        if not isinstance(value, dict):
            raise ProtocolError("response must be a JSON object")
        return value

    def send(self, request):
        payload = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        try:
            self.proc.stdin.write(payload)
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError) as exc:
            raise ProtocolError(f"database closed stdin: {exc}") from exc

    # -- requests ----------------------------------------------------------
    def request(self, sql, mode="exact"):
        self.sequence += 1
        request = {"id": self.sequence, "op": "execute", "sql": sql, "search_mode": mode}
        self.send(request)
        response = self.read(self.config["request_timeout"])
        if type(response.get("id")) is not type(request["id"]) or response["id"] != request["id"]:
            raise ProtocolError(
                f"response id differs from request: {response.get('id')!r} != {request['id']!r}"
            )
        if type(response.get("ok")) is not bool:
            raise ProtocolError(f"missing boolean ok in {response!r}")
        return response

    def shutdown(self):
        self.sequence += 1
        request = {"id": self.sequence, "op": "shutdown"}
        self.send(request)
        response = self.read(self.config["request_timeout"])
        if response.get("id") != request["id"]:
            raise ProtocolError("response id differs from shutdown request")
        if response.get("ok") is not True:
            raise ProtocolError(f"shutdown was refused: {response!r}")

        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=self.config["request_timeout"])
        except subprocess.TimeoutExpired:
            self.stop()
            raise ProtocolError("process did not exit after shutdown") from None

        extra = self._drain()
        if extra:
            raise ProtocolError(f"unexpected stdout after shutdown: {extra[:3]!r}")
        if self.proc.returncode != 0:
            raise ProtocolError(f"exit code {self.proc.returncode} after shutdown")
        self.stop()

    def _drain(self):
        leftovers = []
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if item is not _EOF and not isinstance(item, Exception):
                leftovers.append(item)
        return leftovers

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
            for stream in (self.proc.stdin, self.proc.stdout):
                try:
                    if stream is not None and not stream.closed:
                        stream.close()
                except OSError:
                    pass
            self.proc = None
        if self.log is not None:
            self.log.close()
            self.log = None


def check_case(session, case, package, config, assert_response):
    expectations = session.strict_json((package / case["result"]).read_text(encoding="utf-8"))
    statements = (package / case["test"]).read_text(encoding="utf-8").splitlines()

    index = 0
    mode = "exact"
    for line_number, text in enumerate(statements, 1):
        sql = text.strip()
        if not sql or sql.startswith("#"):
            continue
        if sql == "--restart":
            # Normal shutdown, then start again on the SAME data directory.
            try:
                session.shutdown()
                session.start()
            except ProtocolError as exc:
                raise ProtocolError(f"line {line_number}: {sql}: {exc}") from exc
        elif sql in ("--mode exact", "--mode ann"):
            mode = sql.split()[1]
        elif sql.startswith("--"):
            raise ValueError(f"unsupported public directive: {sql}")
        else:
            if index >= len(expectations):
                raise ValueError("more statements than expected results")
            try:
                response = session.request(sql, mode)
            except ProtocolError as exc:
                # Without this the caller only learns "the process went away",
                # not which statement was in flight.
                raise ProtocolError(f"line {line_number}: {sql}: {exc}") from exc
            try:
                assert_response(response, expectations[index], config)
            except AssertionError as exc:
                raise AssertionError(f"line {line_number}: {sql}: {exc}") from exc
            index += 1

    if index != len(expectations):
        raise ValueError(f"statement/result count mismatch: {index} statements, "
                         f"{len(expectations)} expectations")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--exe", help="path to a locally built minivectordb binary")
    target.add_argument("--command", help="JSON array argv; {data_dir} placeholder supported")
    parser.add_argument("--package", type=Path, default=DEFAULT_PACKAGE,
                        help=f"mysqltest package directory (default: {DEFAULT_PACKAGE})")
    parser.add_argument("--protocol-dir", type=Path, default=DEFAULT_PROTOCOL,
                        help="directory holding the judge's _assertions/_transport modules")
    parser.add_argument("--config", type=Path, default=None,
                        help="config.json override (default: <protocol-dir>/config.json)")
    parser.add_argument("--case", action="append", help="public case id; repeatable")
    parser.add_argument("--handshake-only", action="store_true",
                        help="only check ready + shutdown, run no SQL")
    parser.add_argument("--output", type=Path, default=None,
                        help="directory for logs and result.json (created if absent)")
    parser.add_argument("--keep-data", action="store_true",
                        help="keep per-case data directories for inspection")
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT,
                        help=f"where per-case data directories are created "
                             f"(default: {DEFAULT_DATA_ROOT})")
    args = parser.parse_args(argv)

    if args.handshake_only and args.case:
        parser.error("--handshake-only and --case cannot be combined")

    strict_json, assert_response = load_official_modules(args.protocol_dir)
    config_path = args.config or (args.protocol_dir / "config.json")
    config = strict_json(config_path.read_text(encoding="utf-8"))

    if args.exe:
        argv_target = [str(Path(args.exe).resolve())]
    else:
        argv_target = json.loads(args.command)
        if not isinstance(argv_target, list) or not argv_target:
            parser.error("--command must be a nonempty JSON array of strings")

    if args.handshake_only:
        cases = [{"id": "handshake"}]
    else:
        manifest = strict_json((args.package / "manifest.json").read_text(encoding="utf-8"))
        cases = manifest["cases"]
        if args.case:
            unknown = set(args.case) - {case["id"] for case in cases}
            if unknown:
                parser.error("unknown public case: " + ", ".join(sorted(unknown)))
            cases = [case for case in cases if case["id"] in args.case]

    output = args.output or (ARTIFACT_ROOT / "protocol-results" /
                             time.strftime("local-%Y%m%d-%H%M%S"))
    output.mkdir(parents=True, exist_ok=True)

    results = []
    aborted = None
    for case in cases:
        if aborted:
            results.append({"id": case["id"], "status": "SKIPPED", "message": aborted})
            continue

        started = time.monotonic()
        row = {"id": case["id"], "status": "PASS", "message": ""}
        # Every case starts from a fresh, empty data directory.
        args.data_root.mkdir(parents=True, exist_ok=True)
        data_dir = Path(tempfile.mkdtemp(prefix=f"mvdb-{case['id']}-",
                                         dir=args.data_root))
        session = Session(
            argv_target,
            data_dir,
            config,
            strict_json,
            output / f"{case['id']}.stderr.log",
        )
        try:
            session.start()
            if not args.handshake_only:
                check_case(session, case, args.package, config, assert_response)
            session.shutdown()
        except TimeoutError as exc:
            row.update(status="TIMEOUT", message=str(exc))
            aborted = "not run after first timeout"
        except subprocess.TimeoutExpired as exc:
            row.update(status="TIMEOUT", message=str(exc))
            aborted = "not run after first timeout"
        except Exception as exc:  # noqa: BLE001 - report, never crash the run
            row.update(status="FAIL", message=str(exc))
        finally:
            session.stop()
            if not args.keep_data:
                shutil.rmtree(data_dir, ignore_errors=True)

        row["duration_ms"] = round((time.monotonic() - started) * 1000)
        results.append(row)
        print(f"{row['id']}: {row['status']} {row['message']}".rstrip(), flush=True)

    passed = all(row["status"] == "PASS" for row in results)
    report = {
        "scope": "local-driver",
        "passed": passed,
        "target": argv_target,
        "cases": results,
        "note": "Local diagnostic only. Windows/Linux driver parity check, no scoring.",
    }
    (output / "result.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    total = len(results)
    ok = sum(1 for row in results if row["status"] == "PASS")
    print(f"\n{ok}/{total} passed  ->  {output / 'result.json'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
