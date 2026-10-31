import pytest

from pyhttpd.conf import HttpdConf


class TestRequestBody:

    @pytest.fixture(autouse=True, scope='class')
    def _class_scope(self, env):
        env.require(env.has_lua_module, "mod_lua not built",
                    dep="mod_lua",
                    detected=f"not found in {env.libexec_dir}")
        conf = HttpdConf(env, extras={
            f"test1.{env.http_tld}": """
            AddHandler lua-script .lua
            """,
        })
        conf.add_vhost_test1()
        conf.install()
        assert env.apache_restart() == 0

    # r:requestbody() returns the body of the methods that carry content
    # to an application, which includes QUERY (RFC 10008), and nothing
    # for the others.
    @pytest.mark.parametrize(["method", "expected"], [
        ["POST", b"body=select=surname"],
        ["PUT", b"body=select=surname"],
        ["QUERY", b"body=select=surname"],
        ["DELETE", b"(none)"],
    ])
    def test_lua_001_01(self, env, method, expected):
        r = env.curl_raw([env.mkurl("http", "test1", "/requestbody.lua")],
                         options=["-X", method,
                                  "--data-binary", "select=surname",
                                  "-H", "Content-Type: text/plain"])
        assert r.exit_code == 0, f"{r}"
        assert r.response["status"] == 200
        assert r.response["body"] == expected
