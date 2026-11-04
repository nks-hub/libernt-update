import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

from registry import export_registry, registry_bytes


class FactoryHive:
    def __init__(self, filename):
        self.nodes = {"": {}, "Empty": {}, "Link": {"SymbolicLinkValue": (6, b"target")}}
        if Path(filename).name == "system":
            self.nodes = {
                "": {},
                "Select": {"Current": (4, b"\x01\0\0\0")},
                "ControlSet001": {"OutsideScope": (1, b"private")},
                "ControlSet001\\Control": {},
                "ControlSet001\\Services": {},
                "ControlSet001\\Services\\DemoDriver": {"Start": (4, b"\0\0\0\0")},
                "ControlSet001\\Services\\DemoDriver\\Parameters": {},
                "ControlSet001\\Enum": {"MachineState": (3, b"private")},
                "ControlSet002": {},
                "ControlSet002\\Services": {"Inactive": (3, b"private")},
            }

    def root(self):
        return ""

    def node_get_child(self, parent, name):
        path = parent + "\\" + name if parent else name
        return path if path in self.nodes else None

    def node_get_value(self, node, name):
        return (node, name) if name in self.nodes[node] else None

    def value_dword(self, value):
        return int.from_bytes(self.value_value(value)[1], "little")

    def node_values(self, node):
        return [(node, name) for name in self.nodes[node]]

    def value_value(self, value):
        return self.nodes[value[0]][value[1]]

    def value_key(self, value):
        return value[1]

    def node_children(self, node):
        prefix = node + "\\" if node else ""
        return [path for path in reversed(self.nodes) if path.startswith(prefix) and path != node
                and "\\" not in path[len(prefix):]]

    def node_name(self, node):
        return node.rsplit("\\", 1)[-1]


class RegistryExport(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        config = self.root / "Windows/system32/config"
        config.mkdir(parents=True)
        for name in ("system", "software", "default", "sam", "security"):
            (config / name).touch()
        self.opened = []

        def open_hive(filename):
            self.opened.append(Path(filename).name)
            return FactoryHive(filename)

        replacement = types.SimpleNamespace(Hivex=open_hive)
        self.mock = patch.dict("sys.modules", {"hivex": replacement})
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def test_system_contains_only_selected_control_and_services(self):
        snapshot = export_registry(self.root)
        system = snapshot["hives"]["SYSTEM"]
        self.assertEqual(system["values"], [{"key": "CurrentControlSet\\Services\\DemoDriver", "name": "Start",
                                             "type": 4, "data": "00000000"}])
        self.assertEqual(system["keys"], ["CurrentControlSet\\Control", "CurrentControlSet\\Services",
                                          "CurrentControlSet\\Services\\DemoDriver",
                                          "CurrentControlSet\\Services\\DemoDriver\\Parameters"])

    def test_empty_keys_are_preserved_and_links_and_machine_hives_are_excluded(self):
        snapshot = export_registry(self.root)
        self.assertEqual(snapshot["hives"]["SOFTWARE"], {"keys": ["Empty"], "values": []})
        self.assertEqual(snapshot["hives"]["DEFAULT"], {"keys": ["Empty"], "values": []})
        self.assertEqual(self.opened, ["system", "software", "default"])
        self.assertEqual(json.loads(registry_bytes(snapshot)), snapshot)

    def test_missing_hive_is_rejected(self):
        (self.root / "Windows/system32/config/software").unlink()
        with self.assertRaisesRegex(ValueError, "missing factory registry hive"):
            export_registry(self.root)


if __name__ == "__main__":
    unittest.main()
