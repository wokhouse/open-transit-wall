"""The Pages-hosted web flasher fetches /agencies cross-origin; the proxy
must answer its preflight and allow the GET (docs/web-flasher-plan.md)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import proxy
from fastapi.testclient import TestClient

client = TestClient(proxy.app)


def test_preflight_from_pages_origin_allows_get():
    r = client.options(
        "/agencies",
        headers={
            "Origin": "https://wokhouse.github.io",
            "Access-Control-Request-Method": "GET",
        },
    )
    assert r.status_code == 200
    assert r.headers["access-control-allow-origin"] == "*"
    assert "GET" in r.headers.get("access-control-allow-methods", "")


def test_agencies_get_carries_cors_header():
    r = client.get("/agencies", headers={"Origin": "https://wokhouse.github.io"})
    assert r.status_code == 200
    assert r.headers["access-control-allow-origin"] == "*"
    assert "agencies" in r.json()
