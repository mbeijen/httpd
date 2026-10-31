import inspect
import logging
import os
import subprocess

from pyhttpd.env import HttpdTestEnv, HttpdTestSetup

log = logging.getLogger(__name__)


class LuaTestSetup(HttpdTestSetup):

    def __init__(self, env: 'HttpdTestEnv'):
        super().__init__(env=env)
        self.add_source_dir(os.path.dirname(inspect.getfile(LuaTestSetup)))
        self.add_optional_modules(["lua"])


class LuaTestEnv(HttpdTestEnv):

    def __init__(self, pytestconfig=None):
        super().__init__(pytestconfig=pytestconfig)
        # A LogLevel for an unloaded module is a fatal config error.
        log_modules = ["core"]
        if self.has_lua_module:
            log_modules.insert(0, "lua")
        self.add_httpd_log_modules(log_modules)

    def setup_httpd(self, setup: HttpdTestSetup = None):
        super().setup_httpd(setup=LuaTestSetup(env=self))

    @property
    def has_lua_module(self) -> bool:
        """Whether mod_lua was built, shared or static."""
        if self.has_shared_module("lua"):
            return True
        p = subprocess.run([os.path.join(self.bin_dir, "httpd"), "-l"],
                           capture_output=True, text=True)
        return "mod_lua.c" in p.stdout
