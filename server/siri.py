"""Defensive parser for the 511 SIRI VehicleMonitoring JSON response.

Schema per 511 docs (https://511.org/open-data/transit):
  Siri.ServiceDelivery.VehicleMonitoringDelivery[.VehicleActivity[]]
    .MonitoredVehicleJourney{ LineRef, DirectionRef, PublishedLineName,
                              VehicleLocation{Longitude,Latitude}, Bearing, VehicleRef }
Field casing/shape varies between SIRI implementations, so lookups are lenient.
"""

from __future__ import annotations


def _get(d, *keys):
    cur = d
    for key in keys:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(key)
    return cur


def _first_list(value):
    if isinstance(value, list):
        return value
    if value is None:
        return []
    return [value]


def parse_vehiclemonitor(payload: dict) -> list[dict]:
    """Extract vehicles: [{line, lat, lon, bearing, direction, id}, ...]."""
    delivery = _get(payload, "Siri", "ServiceDelivery", "VehicleMonitoringDelivery")
    activity = _first_list(_get(delivery, "VehicleActivity") if isinstance(delivery, dict)
                           else None)
    if not activity and isinstance(delivery, list):
        for entry in delivery:
            activity.extend(_first_list(_get(entry, "VehicleActivity")))

    vehicles = []
    for record in activity:
        mvj = _get(record, "MonitoredVehicleJourney") or {}
        location = mvj.get("VehicleLocation") or {}
        try:
            lat = float(location.get("Latitude"))
            lon = float(location.get("Longitude"))
        except (TypeError, ValueError):
            continue
        bearing = _get(record, "MonitoredVehicleJourney", "Bearing")
        try:
            bearing = int(float(bearing)) % 360 if bearing is not None else None
        except (TypeError, ValueError):
            bearing = None
        vehicles.append({
            "line": str(mvj.get("LineRef") or mvj.get("PublishedLineName") or "").strip(),
            "lat": lat,
            "lon": lon,
            "bearing": bearing,
            "direction": str(mvj.get("DirectionRef") or "").strip(),
            "id": str(mvj.get("VehicleRef") or "").strip(),
        })
    return vehicles
