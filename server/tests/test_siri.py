import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from siri import parse_vehiclemonitor  # noqa: E402

FIXTURE = Path(__file__).resolve().parent.parent.parent / "fixtures" / "sample_vehiclemonitor.json"


def test_parse_sample_fixture():
    payload = json.loads(FIXTURE.read_text())
    vehicles = parse_vehiclemonitor(payload)
    assert len(vehicles) == 10

    rapid = next(v for v in vehicles if v["line"] == "14R")
    assert rapid["lat"] == pytest.approx(37.7749)
    assert rapid["lon"] == pytest.approx(-122.4194)
    assert rapid["direction"] == "1"
    assert rapid["bearing"] == 45
    assert rapid["id"] == "SFMTA_8213"


def test_tolerates_missing_location():
    payload = {
        "Siri": {"ServiceDelivery": {"VehicleMonitoringDelivery": {
            "VehicleActivity": [
                {"MonitoredVehicleJourney": {"LineRef": "38",
                                             "VehicleLocation": {"Latitude": None}}},
                {"MonitoredVehicleJourney": None},
            ]}}}
    }
    assert parse_vehiclemonitor(payload) == []


def test_delivery_as_list():
    payload = {
        "Siri": {"ServiceDelivery": {"VehicleMonitoringDelivery": [{
            "VehicleActivity": [{"MonitoredVehicleJourney": {
                "LineRef": "22",
                "VehicleLocation": {"Longitude": -122.4, "Latitude": 37.7},
            }}],
        }]}},
    }
    vehicles = parse_vehiclemonitor(payload)
    assert len(vehicles) == 1
    assert vehicles[0]["line"] == "22"
