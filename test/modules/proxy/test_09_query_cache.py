import os
import time
import uuid

import pytest

from pyhttpd.conf import HttpdConf
from .env import TCPFaker


# Backend answering QUERY and GET with a cacheable response that echoes
# the request content, counting the requests per method, path and content.
class _QueryFaker(TCPFaker):

    def __init__(self, host, port):
        super().__init__(host, port)
        self.hits = {}

    def _make_response(self, data):
        head, _, content = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        method, path = lines[0].split(" ")[0:2]
        ctype = ""
        for line in lines[1:]:
            if line.lower().startswith("content-type:"):
                ctype = line.split(":", 1)[1].strip()
        key = (method, path, content)
        self.hits[key] = self.hits.get(key, 0) + 1
        body = (f"{method} hit={self.hits[key]} type={ctype} content=".encode()
                + content + b"\n")
        resp = ["HTTP/1.1 200 OK",
                "Cache-Control: max-age=60",
                "Last-Modified: Thu, 01 Jan 2026 00:00:00 GMT",
                "Content-Type: text/plain",
                f"Content-Length: {len(body)}",
                "Connection: close"]
        return ("\r\n".join(resp) + "\r\n\r\n").encode() + body


class TestQueryCache:

    @pytest.fixture(autouse=True, scope='class')
    def _class_scope(self, env):
        faker = _QueryFaker("127.0.0.1", env.http_port2)
        faker.start()
        cache_dir = os.path.join(env.gen_dir, "query-cache")
        os.makedirs(cache_dir, exist_ok=True)

        conf = HttpdConf(env)
        for name, extra in [
            ("test1", ["CacheQuery on", "CacheQueryMaxBodySize 64"]),
            ("test2", ["CacheQuery on", "CacheQuickHandler off"]),
            ("test3", []),
        ]:
            conf.start_vhost(domains=[f"{name}.{env.http_tld}"],
                             port=env.http_port, doc_root="htdocs",
                             with_ssl=False)
            conf.add([
                f'CacheRoot "{cache_dir}"',
                'CacheEnable disk "/q/"',
                'CacheHeader on',
                f"ProxyPass /q/ http://127.0.0.1:{env.http_port2}/",
            ] + extra)
            conf.end_vhost()
        conf.install()
        assert env.apache_restart() == 0
        TestQueryCache.faker = faker
        yield
        faker.stop()

    def path(self):
        return f"/q/{uuid.uuid4().hex[:12]}"

    def query(self, env, path, content, ctype="application/sql",
              vhost="test1", options=None):
        url = env.mkurl("http", vhost, path)
        opts = ["-X", "QUERY", "--data-binary", content,
                "-H", f"Content-Type: {ctype}"] + (options or [])
        r = env.curl_raw([url], options=opts)
        assert r.exit_code == 0, f"{r}"
        assert r.response["status"] == 200, f"{r}"
        # let CACHE_SAVE commit the entry before the next request
        time.sleep(0.1)
        return r.response

    def hits(self, method, path, content):
        return self.faker.hits.get((method, path[2:], content), 0)

    # the same QUERY is served from the cache (RFC 10008, Section 2.7)
    @pytest.mark.parametrize("vhost", ["test1", "test2"])
    def test_proxy_09_01(self, env, vhost):
        p = self.path()
        r1 = self.query(env, p, "SELECT 1", vhost=vhost)
        r2 = self.query(env, p, "SELECT 1", vhost=vhost)
        assert self.hits("QUERY", p, b"SELECT 1") == 1
        assert r1["body"] == r2["body"] == \
            b"QUERY hit=1 type=application/sql content=SELECT 1\n"
        assert r2["header"].get("x-cache", "").startswith("HIT"), f"{r2}"

    # the key incorporates the content and its media type
    def test_proxy_09_02(self, env):
        p = self.path()
        self.query(env, p, "SELECT 1")
        r = self.query(env, p, "SELECT 2")
        assert r["body"] == b"QUERY hit=1 type=application/sql content=SELECT 2\n"
        # the backend counts per content only, so this miss is its 2nd hit
        r = self.query(env, p, "SELECT 1", ctype="application/x-sql")
        assert r["body"] == b"QUERY hit=2 type=application/x-sql content=SELECT 1\n"
        # case and whitespace of the media type do not matter
        r = self.query(env, p, "SELECT 1", ctype="Application/SQL")
        assert r["header"].get("x-cache", "").startswith("HIT"), f"{r}"
        assert self.hits("QUERY", p, b"SELECT 1") == 2

    # GET and QUERY on the same URL do not share entries
    def test_proxy_09_03(self, env):
        p = self.path()
        self.query(env, p, "")
        r = env.curl_get(env.mkurl("http", "test1", p))
        assert r.response["body"] == b"GET hit=1 type= content=\n"

    # content above CacheQueryMaxBodySize is not cached, but forwarded intact
    def test_proxy_09_04(self, env):
        p = self.path()
        big = "x" * 100
        for _ in range(2):
            r = self.query(env, p, big)
            assert not r["header"].get("x-cache", "").startswith("HIT")
        assert self.hits("QUERY", p, big.encode()) == 2
        assert r["body"].endswith(big.encode() + b"\n")

    # chunked content is buffered and replayed as well
    def test_proxy_09_05(self, env):
        p = self.path()
        opts = ["-H", "Transfer-Encoding: chunked"]
        r1 = self.query(env, p, "SELECT 3", options=opts)
        r2 = self.query(env, p, "SELECT 3", options=opts)
        assert r1["body"].startswith(b"QUERY hit=1 ")
        assert r2["body"] == r1["body"]
        assert r2["header"].get("x-cache", "").startswith("HIT"), f"{r2}"

    # without CacheQuery, QUERY is not cached
    def test_proxy_09_06(self, env):
        p = self.path()
        self.query(env, p, "SELECT 4", vhost="test3")
        r = self.query(env, p, "SELECT 4", vhost="test3")
        assert r["body"] == b"QUERY hit=2 type=application/sql content=SELECT 4\n"
