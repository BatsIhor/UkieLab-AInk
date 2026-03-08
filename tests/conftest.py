"""Pytest fixtures for AI Canvas visual test suite."""

import os
import sys
import pytest

# Ensure tests/ is on the path for local imports
sys.path.insert(0, os.path.dirname(__file__))

from canvas_client import CanvasClient


def pytest_addoption(parser):
    parser.addoption(
        "--canvas-url",
        default=None,
        help="AI Canvas device URL (overrides CANVAS_URL env var)",
    )


@pytest.fixture(scope="session")
def base_url(request):
    """Resolve the device URL from CLI arg or environment."""
    url = request.config.getoption("--canvas-url") or os.environ.get("CANVAS_URL")
    if not url:
        pytest.skip("No device URL: set CANVAS_URL or pass --canvas-url")
    return url


@pytest.fixture(scope="session")
def canvas(base_url):
    """Session-scoped CanvasClient instance."""
    client = CanvasClient(base_url)
    # Verify connectivity
    try:
        client.health()
    except Exception as e:
        pytest.skip(f"Cannot reach device at {base_url}: {e}")
    return client


@pytest.fixture(scope="session")
def output_dir():
    """Path to tests/output/ directory, created if needed."""
    d = os.path.join(os.path.dirname(__file__), "output")
    os.makedirs(d, exist_ok=True)
    return d


@pytest.fixture(autouse=True)
def clear_before(canvas):
    """Clear the display before each test."""
    canvas.clear("white")
    canvas.wait_refresh()
