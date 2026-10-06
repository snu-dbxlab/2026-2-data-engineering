"""
Shared harness for the PA2 autograder.

Builds both configurations of a PostgreSQL tree, creates a cluster, and offers
a small SQL interface.  Works on Gradescope (/autograder/...) and locally.

Design notes that matter for grading:

  * Both configurations are built from the *student's* tree.  The default build is
    what proves their shared-header edits are properly #ifdef'd; without it, a
    student can break the default PostgreSQL build and never find out.
  * initdb is run from the default build (see spec Part 1 section 3): a snudbx
    initdb probes shared_buffers downward, and N pools multiply the shared
    memory requirement, so cluster creation can land on a pool too small to
    bootstrap.  Nothing here changes an on-disk format, so the cluster is
    readable by both builds.
"""

import os
import re
import shutil
import socket
import subprocess
import time

# --- locations -------------------------------------------------------------

ON_GRADESCOPE = os.path.isdir("/autograder/results")
ROOT = os.environ.get("PA2_WORK", "/autograder/work" if ON_GRADESCOPE else "/tmp/pa2-work")
SRC = os.environ.get("PA2_SRC", os.path.join(ROOT, "src"))

BUILD = {cfg: os.path.join(ROOT, "build-" + cfg) for cfg in ("default", "snudbx")}
INST = {cfg: os.path.join(ROOT, "inst-" + cfg) for cfg in ("default", "snudbx")}
PGDATA = os.path.join(ROOT, "pgdata")

JOBS = str(min(os.cpu_count() or 4, 16))

CONFIGURE_COMMON = [
    "--enable-cassert", "--enable-debug",
    "--without-icu", "--without-zlib", "--without-readline",
]


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

# Instructor extension (tests 3.5-3.8).  Present only in the Gradescope copy of
# the autograder; the local bundle handed to students leaves it out.
GRADER_EXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grader_ext")


def build(cfg):
    """Configure + make + install one configuration.  Cached per process."""
    if cfg in _built:
        if isinstance(_built[cfg], Exception):
            raise _built[cfg]
        return _built[cfg]
    try:
        if not os.path.exists(os.path.join(SRC, "configure")):
            raise BuildError("no PostgreSQL source tree at %s (is PA2_SRC set?)" % SRC)
        os.makedirs(BUILD[cfg], exist_ok=True)
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
        # installed snudbx server, and only once the student has created it.
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
    shutil.rmtree(PGDATA, ignore_errors=True)
    r = _run([os.path.join(INST["default"], "bin", "initdb"),
              "-D", PGDATA, "-U", "postgres", "--no-sync"])
    if r.returncode != 0:
        raise BuildError("initdb failed:\n%s" % _tail(r.stdout))


class Server:
    """Context manager: a running postmaster from one configuration."""

    def __init__(self, cfg="snudbx", settings=None, expect_start=True):
        self.cfg = cfg
        self.settings = settings or {}
        self.expect_start = expect_start
        self.log = os.path.join(ROOT, "pg-%s.log" % cfg)
        self.started = False

    def __enter__(self):
        build(self.cfg)
        initdb()
        opts = ["-p", str(port())]
        # Assumption A3 (PA2_ASSUMPTIONS.md): every server runs with synchronous
        # I/O.  PostgreSQL 18 defaults to io_method=worker.
        settings = {"io_method": "sync"}
        settings.update(self.settings)
        for k, v in settings.items():
            opts += ["-c", "%s=%s" % (k, v)]
        if os.path.exists(self.log):
            os.remove(self.log)
        r = _run([os.path.join(INST[self.cfg], "bin", "pg_ctl"),
                  "-D", PGDATA, "-l", self.log, "-o", " ".join(opts), "-w", "start"],
                 timeout=120)
        self.started = (r.returncode == 0)
        if self.expect_start and not self.started:
            raise BuildError("server (%s) did not start:\n%s" % (self.cfg, self.logtext()))
        return self

    def __exit__(self, *exc):
        if self.started:
            _run([os.path.join(INST[self.cfg], "bin", "pg_ctl"),
                  "-D", PGDATA, "stop", "-m", "fast"], timeout=120)
            self.started = False
        return False

    def logtext(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return "(no log)"

    def sql(self, query, db="postgres"):
        """Run one query, return stripped stdout.  Raises on error."""
        r = _run([os.path.join(INST[self.cfg], "bin", "psql"),
                  "-p", str(port()), "-U", "postgres", "-d", db,
                  "-v", "ON_ERROR_STOP=1", "-Atc", query], timeout=300)
        if r.returncode != 0:
            raise RuntimeError("query failed: %s\n%s" % (query, r.stdout.strip()))
        return r.stdout.strip()

    def sql_or_error(self, query, db="postgres"):
        """Run one query; return (ok, text) instead of raising."""
        r = _run([os.path.join(INST[self.cfg], "bin", "psql"),
                  "-p", str(port()), "-U", "postgres", "-d", db,
                  "-v", "ON_ERROR_STOP=1", "-Atc", query], timeout=300)
        return (r.returncode == 0, r.stdout.strip())

    def create_extension(self, db="postgres"):
        """CREATE EXTENSION snudbx, with a build failure reported as the cause."""
        if _ext_error[0]:
            raise BuildError("contrib/snudbx failed to build:\n%s" % _ext_error[0])
        ok, out = self.sql_or_error("CREATE EXTENSION IF NOT EXISTS snudbx", db)
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
        """installcheck-parallel against this server; returns (ok, tail of output)."""
        env = dict(os.environ, PGPORT=str(port()), PGUSER="postgres")
        r = _run(["make", "-C", os.path.join(BUILD[self.cfg], "src", "test", "regress"),
                  "installcheck-parallel"], env=env, timeout=1800)
        return (r.returncode == 0, _tail(r.stdout, 30))

    def rows(self, query, db="postgres"):
        out = self.sql(query, db)
        return [line.split("|") for line in out.splitlines()] if out else []
