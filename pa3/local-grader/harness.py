"""
Shared harness for the PA3 autograder.

Builds both configurations of a PostgreSQL tree, creates a cluster, and offers
a small SQL interface plus a few knobs for the flush worker.  Works on
Gradescope (/autograder/...) and locally.

Design notes that matter for grading:

  * Both configurations are built from the *student's* tree.  The default build is
    what proves their shared-file edits (postmaster.c, bgworker.c, headers) are
    properly #ifdef'd; without it, a student can break the default PostgreSQL
    build and never find out.
  * initdb is run from the default build: a snudbx initdb probes shared_buffers
    downward, and N pools multiply the shared memory requirement, so cluster
    creation can land on a pool too small to bootstrap.  Nothing here changes
    an on-disk format, so the cluster is readable by both builds.
  * The flush worker is paused with SIGSTOP / SIGCONT when a test needs a pool
    to stay SEALED.  That needs no student code and cannot be faked.  Two rules:
    stop it only while no pool is SEALED (otherwise it may hold SealedPoolLock
    exclusively while stopped, and the backend blocks), and always SIGCONT it
    before pg_ctl, or the shutdown waits forever.  Server.__exit__ does the
    latter.
"""

import os
import re
import shutil
import signal
import socket
import subprocess
import time

# --- locations -------------------------------------------------------------

ON_GRADESCOPE = os.path.isdir("/autograder/results")
ROOT = os.environ.get("PA3_WORK", "/autograder/work" if ON_GRADESCOPE else "/tmp/pa3-work")
SRC = os.environ.get("PA3_SRC", os.path.join(ROOT, "src"))

BUILD = {cfg: os.path.join(ROOT, "build-" + cfg) for cfg in ("default", "snudbx")}
INST = {cfg: os.path.join(ROOT, "inst-" + cfg) for cfg in ("default", "snudbx")}
# The cluster and server logs live in RUNDIR, which the server's OS user owns.
RUNDIR = os.path.join(ROOT, "run")
PGDATA = os.path.join(RUNDIR, "pgdata")

# initdb, pg_ctl and postgres refuse to run as root, and Gradescope runs the
# autograder as root.  There the server side runs as an unprivileged user.
IS_ROOT = hasattr(os, "geteuid") and os.geteuid() == 0
SERVER_USER = os.environ.get("PA3_SERVER_USER", "pa3server")

JOBS = str(min(os.cpu_count() or 4, 16))

CONFIGURE_COMMON = [
    "--enable-cassert", "--enable-debug",
    # Track header dependencies: the local runner reuses build directories,
    # and PA3 edits headers, so without this stale objects would be linked.
    "--enable-depend",
    "--without-icu", "--without-zlib", "--without-readline",
]

WORKER_TYPE = "snudbx flush worker"     # bgw_type and bgw_name, fixed by the spec


class BuildError(Exception):
    """A configure or make step failed.  Carries the tail of the log."""


def _run(cmd, cwd=None, timeout=1800, env=None):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, errors="replace")


def _tail(text, n=40):
    lines = text.strip().splitlines()
    return "\n".join(lines[-n:])


# --- build -----------------------------------------------------------------

_built = {}
_ext_error = [None]        # why contrib/snudbx failed to build, if it did
_grader_error = [None]     # why the instructor extension failed to build

# Instructor extension (hidden tests).  Present only in the Gradescope copy of
# the autograder; the local bundle handed to students leaves it out.
GRADER_EXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grader_ext")


def _fresh_build_dir(cfg):
    """Reuse BUILD[cfg] only if it was configured from this same source tree.

    make trusts the objects it finds in the build directory, so a directory
    left over from another tree (or one made before this check existed) would
    link stale code.  Such a directory and its install are discarded.
    """
    stamp = os.path.join(BUILD[cfg], ".srcdir")
    src = os.path.realpath(SRC)
    if os.path.isdir(BUILD[cfg]):
        old = open(stamp).read().strip() if os.path.exists(stamp) else None
        if old != src:
            shutil.rmtree(BUILD[cfg], ignore_errors=True)
            shutil.rmtree(INST[cfg], ignore_errors=True)
    os.makedirs(BUILD[cfg], exist_ok=True)
    with open(stamp, "w") as f:
        f.write(src + "\n")


def build(cfg):
    """Configure + make + install one configuration.  Cached per process."""
    if cfg in _built:
        if isinstance(_built[cfg], Exception):
            raise _built[cfg]
        return _built[cfg]
    try:
        if not os.path.exists(os.path.join(SRC, "configure")):
            raise BuildError("no PostgreSQL source tree at %s (is PA3_SRC set?)" % SRC)
        _fresh_build_dir(cfg)
        args = [os.path.join(SRC, "configure"), "--prefix=" + INST[cfg]] + CONFIGURE_COMMON
        if cfg == "snudbx":
            args.append("--enable-snudbx-buffer")
        r = _run(args, cwd=BUILD[cfg])
        if r.returncode != 0:
            raise BuildError("configure (%s) failed:\n%s" % (cfg, _tail(r.stdout)))
        for target in (["make", "-j" + JOBS, "-s"],
                       ["make", "-s", "install"],
                       ["make", "-C", "contrib", "-j" + JOBS, "-s"],
                       ["make", "-C", "contrib", "-s", "install"]):
            r = _run(target, cwd=BUILD[cfg])
            if r.returncode != 0:
                raise BuildError("%s (%s) failed:\n%s"
                                 % (" ".join(target), cfg, _tail(r.stdout)))
        # contrib/snudbx is the student's own extension.  It is not listed in
        # contrib/Makefile (which the patch may not touch) and reads state that
        # exists only under SNUDBX, so it is built on its own, against the
        # installed snudbx server.
        ext = os.path.join(SRC, "contrib", "snudbx")
        if cfg == "snudbx" and os.path.exists(os.path.join(ext, "Makefile")):
            r = _run(["make", "-C", ext, "-s", "USE_PGXS=1",
                      "PG_CONFIG=" + os.path.join(INST[cfg], "bin", "pg_config"),
                      "clean", "install"])
            if r.returncode != 0:
                _ext_error[0] = _tail(r.stdout)
        if cfg == "snudbx" and os.path.isdir(GRADER_EXT):
            r = _run(["make", "-C", GRADER_EXT, "-s",
                      "PG_CONFIG=" + os.path.join(INST[cfg], "bin", "pg_config"),
                      "clean", "install"])
            if r.returncode != 0:
                _grader_error[0] = _tail(r.stdout)
        _built[cfg] = True
        return True
    except Exception as e:                      # cache the failure too
        _built[cfg] = e
        raise


def compiler_diagnostics(cfg):
    """Warnings/errors from the last build of cfg, for reporting."""
    log = os.path.join(BUILD[cfg], "build.log")
    if not os.path.exists(log):
        return ""
    with open(log, errors="replace") as f:
        return "\n".join(l for l in f if re.search(r"\b(error|warning)\b", l, re.I))


# --- cluster ---------------------------------------------------------------

def _as_server(cmd):
    """Run a server-side command as SERVER_USER when we are root."""
    return ["runuser", "-u", SERVER_USER, "--"] + cmd if IS_ROOT else cmd


def _prepare_rundir():
    os.makedirs(RUNDIR, exist_ok=True)
    if IS_ROOT:
        if subprocess.run(["id", "-u", SERVER_USER], capture_output=True).returncode != 0:
            subprocess.run(["useradd", "--system", "--no-create-home", SERVER_USER], check=True)
        shutil.chown(RUNDIR, SERVER_USER, SERVER_USER)
        # The server user must be able to reach RUNDIR and the installs.
        d = RUNDIR
        while d not in ("/", ""):
            d = os.path.dirname(d)
            os.chmod(d, os.stat(d).st_mode | 0o011)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


_port = None


def port():
    global _port
    if _port is None:
        _port = free_port()
    return _port


def initdb():
    """Create the cluster with the DEFAULT (unmodified) build.  Idempotent."""
    if os.path.exists(os.path.join(PGDATA, "PG_VERSION")):
        return
    build("default")
    _prepare_rundir()
    shutil.rmtree(PGDATA, ignore_errors=True)
    r = _run(_as_server([os.path.join(INST["default"], "bin", "initdb"),
                         "-D", PGDATA, "-U", "postgres", "--no-sync"]))
    if r.returncode != 0:
        raise BuildError("initdb failed:\n%s" % _tail(r.stdout))


def _postmaster_pid():
    try:
        with open(os.path.join(PGDATA, "postmaster.pid")) as f:
            return int(f.readline())
    except (OSError, ValueError):
        return None


def stop_server(cfg):
    """Stop whatever postmaster runs on PGDATA.  A submission that crashes the
    server can leave a postmaster that a fast shutdown does not stop; it must
    not keep every later test from starting a server."""
    pg_ctl = os.path.join(INST[cfg], "bin", "pg_ctl")
    for mode in ("fast", "immediate"):
        if _postmaster_pid() is None:
            return
        try:
            _run(_as_server([pg_ctl, "-D", PGDATA, "stop", "-m", mode]), timeout=60)
        except subprocess.TimeoutExpired:
            pass
    pid = _postmaster_pid()
    if pid is not None:
        try:
            os.kill(pid, 9)
        except OSError:
            pass
        time.sleep(1)
        try:
            os.remove(os.path.join(PGDATA, "postmaster.pid"))
        except OSError:
            pass


# Regression tests that fail with the settings above on stock PostgreSQL too
# (plans differ without autovacuum, no parallel workers), plus stats: data
# pools use no ring, so VACUUM reuses no ring buffer.
REGRESS_KNOWN_FAILURES = {
    "uuid", "rangetypes", "box", "inet", "geometry", "groupingsets",
    "select_parallel", "cluster", "returning", "with", "stats",
    "copy", "create_am", "matview", "tablespace",
}

# Server-side wait until no pool is SEALED.  Touches only catalogs (the
# metadata pool), so it does not disturb the data pools it watches.
WAIT_RESET = ("DO $$ BEGIN FOR i IN 1..%d LOOP "
              "EXIT WHEN snudbx_sealed_pool() = -1; PERFORM pg_sleep(0.01); "
              "END LOOP; END $$")


class Server:
    """Context manager: a running postmaster from one configuration."""

    def __init__(self, cfg="snudbx", settings=None, expect_start=True):
        self.cfg = cfg
        self.settings = settings or {}
        self.expect_start = expect_start
        self.log = os.path.join(RUNDIR, "pg-%s.log" % cfg)
        self.started = False
        self._stopped_worker = None

    def __enter__(self):
        build(self.cfg)
        initdb()
        opts = ["-p", str(port())]
        # Assumption A3: every server runs with synchronous I/O.  PostgreSQL 18
        # defaults to io_method=worker.  Assumption A1: one backend does all
        # the work, so no parallel query workers and no autovacuum workers.
        settings = {"io_method": "sync",
                    "max_parallel_workers": "0",
                    "max_parallel_maintenance_workers": "0",
                    "autovacuum": "off"}
        settings.update(self.settings)
        for k, v in settings.items():
            opts += ["-c", "%s=%s" % (k, v)]
        if _postmaster_pid() is not None:        # left over from a crash
            stop_server(self.cfg)
        if os.path.exists(self.log):
            os.remove(self.log)
        r = _run(_as_server([os.path.join(INST[self.cfg], "bin", "pg_ctl"),
                             "-D", PGDATA, "-l", self.log, "-o", " ".join(opts),
                             "-w", "start"]),
                 timeout=120)
        self.started = (r.returncode == 0)
        if self.expect_start and not self.started:
            raise BuildError("server (%s) did not start:\n%s" % (self.cfg, self.logtext()))
        return self

    def __exit__(self, *exc):
        self.cont_worker()
        if self.started:
            stop_server(self.cfg)
            self.started = False
        return False

    def logtext(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return "(no log)"

    def _psql(self, args, timeout=300):
        return _run([os.path.join(INST[self.cfg], "bin", "psql"),
                     "-p", str(port()), "-U", "postgres"] + args, timeout=timeout)

    def sql(self, query, db="postgres"):
        """Run one query, return stripped stdout.  Raises on error."""
        r = self._psql(["-d", db, "-v", "ON_ERROR_STOP=1", "-Atc", query])
        if r.returncode != 0:
            raise RuntimeError("query failed: %s\n%s" % (query, r.stdout.strip()))
        return r.stdout.strip()

    def sql_or_error(self, query, db="postgres"):
        """Run one query; return (ok, text) instead of raising."""
        r = self._psql(["-d", db, "-v", "ON_ERROR_STOP=1", "-Atc", query])
        return (r.returncode == 0, r.stdout.strip())

    def timed_sql(self, query, db="postgres"):
        """Run one query; return (stripped stdout, wall time in ms)."""
        t0 = time.time()
        out = self.sql(query, db)
        return out, (time.time() - t0) * 1000.0

    def session(self, queries, db="postgres", timeout=600):
        """Run queries in order in one session; return (ok, outputs).

        outputs holds one stripped output per query that ran; it stops at
        the first error, whose message is the last element.
        """
        mark = "@@pa3-next@@"
        cmd = ["-d", db, "-q", "-v", "ON_ERROR_STOP=1", "-At"]
        for q in queries:
            cmd += ["-c", q, "-c", "\\echo " + mark]
        r = self._psql(cmd, timeout=timeout)
        parts = [x.strip() for x in r.stdout.split(mark + "\n")]
        if parts and parts[-1] == "":
            parts.pop()
        return (r.returncode == 0, parts)

    def rows(self, query, db="postgres"):
        out = self.sql(query, db)
        return [line.split("|") for line in out.splitlines()] if out else []

    def create_extension(self, db="postgres"):
        """CREATE EXTENSION snudbx, with a build failure reported as the cause.

        The extension is re-created so that its functions match the installed
        version (a cluster reused across runs may carry an older definition).
        """
        if _ext_error[0]:
            raise BuildError("contrib/snudbx failed to build:\n%s" % _ext_error[0])
        self.sql_or_error("DROP EXTENSION IF EXISTS snudbx", db)
        ok, out = self.sql_or_error("CREATE EXTENSION snudbx", db)
        if not ok:
            raise BuildError("CREATE EXTENSION snudbx failed:\n%s\n\n"
                             "Is contrib/snudbx/ present, with a Makefile, a .control "
                             "file and snudbx--1.0.sql?" % out)

    def create_grader(self, db="postgres"):
        """CREATE EXTENSION snudbx_grader (instructor probes)."""
        if _grader_error[0]:
            raise BuildError("instructor extension failed to build against your "
                             "headers:\n%s" % _grader_error[0])
        self.sql("CREATE EXTENSION IF NOT EXISTS snudbx_grader", db)

    def regress(self):
        """installcheck against this server; returns (ok, tail of output).

        The tests run one at a time (A1).  ok means every failing test is in
        REGRESS_KNOWN_FAILURES.
        """
        env = dict(os.environ, PGPORT=str(port()), PGUSER="postgres")
        rdir = os.path.join(BUILD[self.cfg], "src", "test", "regress")
        if IS_ROOT:
            # Some tests make the server write into rdir/results (COPY TO).
            subprocess.run(["chown", "-R", SERVER_USER, rdir], check=True)
        r = _run(_as_server(["make", "-C", rdir, "installcheck"]),
                 env=env, timeout=2400)
        failed = set(re.findall(r"^not ok\s+\d+\s+[+-]?\s*(\S+)", r.stdout, re.M))
        unexpected = sorted(failed - REGRESS_KNOWN_FAILURES)
        ok = "1.." in r.stdout and not unexpected
        out = _tail(r.stdout, 30)
        if unexpected:
            out = "unexpected failures: %s\n%s" % (", ".join(unexpected), out)
        return (ok, out)

    # --- PA3: the flush worker and the pools ---------------------------------

    def worker_pid(self):
        """pid of the flush worker from pg_stat_activity, or None."""
        out = self.sql("SELECT pid FROM pg_stat_activity WHERE backend_type = '%s'"
                       % WORKER_TYPE)
        return int(out) if out else None

    def wait_reset(self, seconds=6.0):
        """Wait until no pool is SEALED.  Returns True if that happened.

        Needs the student's snudbx_sealed_pool(); the loop runs inside the
        server so that the one client connection it uses touches only catalogs.
        """
        self.sql(WAIT_RESET % int(seconds * 100))
        return self.sql("SELECT snudbx_sealed_pool()") == "-1"

    def stop_worker(self):
        """SIGSTOP the flush worker, once no pool is SEALED.  Returns its pid."""
        if not self.wait_reset():
            raise RuntimeError("a pool stayed SEALED; cannot pause the worker safely")
        pid = self.worker_pid()
        if pid is None:
            raise RuntimeError("no flush worker in pg_stat_activity")
        os.kill(pid, signal.SIGSTOP)
        self._stopped_worker = pid
        return pid

    def cont_worker(self):
        """SIGCONT the worker stopped by stop_worker(), if any."""
        if self._stopped_worker is not None:
            try:
                os.kill(self._stopped_worker, signal.SIGCONT)
            except OSError:
                pass
            self._stopped_worker = None

    def io_writes(self, backend_type):
        """Relation writes by one backend type since the last pg_stat_reset_shared('io')."""
        return int(self.sql("SELECT coalesce(sum(writes), 0) FROM pg_stat_io "
                            "WHERE backend_type = '%s' AND object = 'relation' "
                            "AND context = 'normal'" % backend_type))

    def settled_io_writes(self, backend_type, seconds=3.0):
        """io_writes(backend_type) once it has stopped changing.  A backend hands
        its I/O statistics to the shared store only when it goes idle or exits,
        so a count read right after another session wrote can lag for a moment:
        wait until two readings 0.25 s apart agree (or `seconds` have passed)."""
        deadline = time.time() + seconds
        n = self.io_writes(backend_type)
        while time.time() < deadline:
            time.sleep(0.25)
            m = self.io_writes(backend_type)
            if m == n:
                return m
            n = m
        return n

    def crash(self):
        """Immediate shutdown: no shutdown checkpoint, so the next start
        recovers from WAL.  Combined with a CHECKPOINT just before, this shows
        whether pages the buffer manager dropped had really been written."""
        self.cont_worker()
        _run(_as_server([os.path.join(INST[self.cfg], "bin", "pg_ctl"),
                         "-D", PGDATA, "stop", "-m", "immediate"]), timeout=60)
        self.started = False

    def pgbench(self, script, transactions, db="postgres"):
        """Run a pgbench script with one client; return (ok, output).

        ok is False if pgbench failed or any transaction reported an ERROR.
        """
        r = _run([os.path.join(INST[self.cfg], "bin", "pgbench"),
                  "-p", str(port()), "-U", "postgres", "-n", "-c", "1",
                  "-t", str(transactions), "-f", script, db], timeout=900)
        ok = r.returncode == 0 and "ERROR" not in r.stdout
        return (ok, _tail(r.stdout, 20))
