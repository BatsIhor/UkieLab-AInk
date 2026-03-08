"""HTTP client for the AI Canvas device."""

import time
import requests
from PIL import Image
from pbm_utils import parse_pbm


class CanvasClient:
    """Wraps all AI Canvas HTTP API endpoints."""

    def __init__(self, base_url: str, timeout: float = 15.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()

    def render(self, commands: list[dict], zone: str | None = None) -> dict:
        """POST /canvas — send drawing commands."""
        body = {"commands": commands}
        if zone:
            body["zone"] = zone
        resp = self.session.post(
            f"{self.base_url}/canvas",
            json=body,
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def clear(self, color: str = "white") -> dict:
        """Clear the display via POST /canvas with a clear command.

        Uses the main /canvas endpoint instead of /canvas/clear to avoid
        issues with the dedicated clear endpoint's body accumulator under
        rapid sequential requests.
        """
        return self.render([{"op": "clear", "color": color}])

    def screenshot(self) -> Image.Image:
        """GET /canvas/screenshot — download PBM and parse to PIL Image."""
        resp = self.session.get(
            f"{self.base_url}/canvas/screenshot",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return parse_pbm(resp.content)

    def screenshot_raw(self) -> bytes:
        """GET /canvas/screenshot — return raw PBM bytes."""
        resp = self.session.get(
            f"{self.base_url}/canvas/screenshot",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.content

    def device_info(self) -> dict:
        """GET /device — device capabilities."""
        resp = self.session.get(
            f"{self.base_url}/device",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def health(self) -> dict:
        """GET /health — device health status."""
        resp = self.session.get(
            f"{self.base_url}/health",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def get_canvas(self) -> dict:
        """GET /canvas — current frame command log."""
        resp = self.session.get(
            f"{self.base_url}/canvas",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def set_zones(self, zones: list[dict]) -> dict:
        """POST /zones — define named zones."""
        resp = self.session.post(
            f"{self.base_url}/zones",
            json=zones,
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def get_zones(self) -> list:
        """GET /zones — list defined zones."""
        resp = self.session.get(
            f"{self.base_url}/zones",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def delete_zones(self) -> dict:
        """DELETE /zones — remove all zones."""
        resp = self.session.delete(
            f"{self.base_url}/zones",
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def set_name(self, name: str) -> dict:
        """POST /device/name — rename device."""
        resp = self.session.post(
            f"{self.base_url}/device/name",
            json={"name": name},
            timeout=self.timeout,
        )
        resp.raise_for_status()
        return resp.json()

    def wait_refresh(self, timeout: float = 10.0):
        """Wait for display refresh to complete.

        Polls /health for display_busy=false. Falls back to a fixed delay
        if display_busy stays stuck (can happen after OOM or rapid requests).
        """
        deadline = time.time() + timeout
        settled = False
        while time.time() < deadline:
            h = self.health()
            if not h.get("display_busy", False):
                settled = True
                break
            time.sleep(0.5)
        if not settled:
            # EPD refresh takes ~3s; wait a safe fixed amount
            time.sleep(4)

    def render_and_wait(
        self, commands: list[dict], zone: str | None = None, wait: float = 10.0
    ) -> dict:
        """Render commands and wait for display refresh to complete."""
        result = self.render(commands, zone=zone)
        self.wait_refresh(timeout=wait)
        return result
