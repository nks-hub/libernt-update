"""The caller must provide factory build hives, not a backup of an installed machine."""
import json
from pathlib import Path


def export_registry(tree: Path) -> dict:
    import hivex

    output = {}
    directory = tree / "Windows" / "system32" / "config"
    names = {path.name.lower(): path for path in directory.iterdir()}
    for name in ("system", "software", "default"):
        if name not in names:
            raise ValueError(f"missing factory registry hive: {name}")
        hive = hivex.Hivex(str(names[name]))
        control_set = None
        if name == "system":
            select = hive.node_get_child(hive.root(), "Select")
            current = hive.node_get_value(select, "Current") if select else None
            if not current:
                raise ValueError("factory SYSTEM hive has no selected control set")
            control_set = f"ControlSet{hive.value_dword(current):03d}"
            if not hive.node_get_child(hive.root(), control_set):
                raise ValueError("factory SYSTEM control set does not exist")
        values = []
        keys = []
        pending = [(hive.root(), "")]
        while pending:
            node, path = pending.pop()
            parts = path.split("\\")
            if name == "system":
                if path and parts[0].lower() != control_set.lower():
                    continue
                if len(parts) > 1 and parts[1].lower() not in ("control", "services"):
                    continue
            entries = [(value, hive.value_value(value)) for value in hive.node_values(node)]
            if any(item[1][0] == 6 for item in entries):
                continue
            include = bool(path) and (name != "system" or len(parts) > 1)
            target = "CurrentControlSet" + path[len(control_set):] if name == "system" else path
            if include:
                keys.append(target)
            for value, (kind, data) in entries if include else []:
                value_name = hive.value_key(value)
                if "\0" in path or "\0" in value_name or len(data) > 1024 * 1024:
                    raise ValueError("invalid or oversized factory registry value")
                values.append({"key": target, "name": value_name, "type": kind, "data": data.hex()})
            for child in hive.node_children(node):
                child_name = hive.node_name(child)
                if "\0" in child_name or "\\" in child_name:
                    raise ValueError("invalid factory registry key")
                pending.append((child, path + "\\" + child_name if path else child_name))
        values.sort(key=lambda value: (value["key"].casefold(), value["name"].casefold()))
        output[name.upper()] = {"keys": sorted(keys, key=str.casefold), "values": values}
    return {"format": 1, "hives": output}


def registry_bytes(snapshot: dict) -> bytes:
    return json.dumps(snapshot, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
